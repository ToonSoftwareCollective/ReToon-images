// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * toon-ui: apps. An app is a folder on p4 (APPS_DIR/<id>) with a
 * manifest.json and a JavaScript file, run by QuickJS. Each app gets its own
 * JavaScript runtime with a memory limit and a time limit per call, and sees
 * only what this file gives it (no file system, no processes):
 *
 *   retoon.app            { id, name, version }
 *   retoon.settings       the manifest's defaults, overridden by
 *                         CONFIG_DIR/apps/<id>.json (written by the web interface)
 *   retoon.fetch(url, {method, headers, body, timeout, user, password, auth})
 *                         -> Promise of { status, text, headers }; auth is
 *                         "basic", "digest" or "any" (default, with a user);
 *                         headers: the answer's, names in lower case
 *   retoon.tile(fn)       fn(el, card) draws the app's dashboard card
 *                         ({"type": "custom:retoon-app", "app": "<id>"})
 *   retoon.screen(fn)     fn(el, card) draws the app's full screen; a tap on
 *                         the card opens it (card: the one tapped, level 10)
 *   retoon.update()       draw the card(s) and the open screen again
 *   retoon.onSettings(fn) called after the settings were changed
 *   retoon.watch(ids, fn) keep these entities (Home Assistant, mqtt.*,
 *                         toon.*) up to date; fn(id) is called when the
 *                         state or an attribute of one changes (level 7;
 *                         before only the state). A new call replaces the list.
 *   retoon.state(id)      { state, value, unit, name, attributes } or null;
 *                         value is the state as a number (NaN if it is none)
 *   retoon.notify(msg, fn)
 *                         a banner or popup (see notify.c for msg: title,
 *                         message, level, buttons, sound, timeout, tag...);
 *                         fn(button) after a tap on a button, fn(null) when
 *                         it was closed without one
 *   retoon.sound(name, volume)
 *                         "beep", "chime" or a WAV file of SOUND_DIR;
 *                         volume 0..100, default the sound setting
 *   retoon.log(...), console.log(...), setTimeout, setInterval,
 *   clearTimeout, clearInterval
 *
 * Drawing: el.box(opts), el.text(string, opts), el.button(label, opts, fn),
 * el.icon("mdi:name", opts), el.circle(opts), el.line([x1, y1, x2, y2, ...],
 * opts), el.flow(opts), el.time(opts, fn), el.onClick(fn), el.set(opts),
 * el.width, el.height.
 * Options: x, y, w, h, align ("top-mid", "center", ...), color, bg, opacity,
 * radius, border, borderColor, font ("small", "body", "head", "mid", "big"),
 * textAlign; text (el.set of a text); size ("small", "mid", "big" for an
 * icon); circle: x, y = centre, r; line: width; flow: x1, y1, x2, y2, speed
 * (pixels a second, below 0 = from x2,y2 to x1,y1, 0 = no dots), gap, size.
 * A flow's dots move by themselves; el.set({speed}) changes it without
 * starting again. el.time: a time picker (an hour and a minute wheel);
 * value "HH:MM", step (minutes, a divisor of 60), rows (visible rows); fn("HH:MM")
 * after the user turned a wheel; el.set({value}) turns them without calling fn.
 * Colours are "#rrggbb" or a theme name (text, muted,
 * accent, ok, warn, bad, card). The app draws everything again on
 * retoon.update(); the elements of an earlier drawing are gone by then.
 *
 * The manifest's "api" is the level of this interface the app needs; an app
 * that needs a higher level than APPS_API is not started. Level 1: the above
 * without what level 2 added: retoon.watch, retoon.state, el.icon,
 * el.circle, el.line, el.flow and el.set. Level 3 added retoon.notify and
 * retoon.sound. Level 4 added user, password and auth to retoon.fetch, and
 * headers to its answer, and el.time. Level 5 added retoon.publish,
 * retoon.subscribe and retoon.unsubscribe: MQTT on the Toon's own broker
 * (bus.c), where toon-mqttd puts the Toon's state (retoon/climate,
 * retoon/boiler, ...) and takes commands (retoon/boiler/set, ...).
 * Level 6 added el.image({url, every, user, password, auth, headers, x, y,
 * w, h, align, radius}): a JPEG (not progressive) or PNG from a URL, fitted
 * into w x h (standard 320 x 240) without growing; every: fetch a new one
 * that often (seconds, at least 2) while it is shown.
 * Level 7 added retoon.call("domain.service", data, opts): a Home Assistant
 * service over the Toon's own connection (no token in the app). Only the
 * services the manifest lists in "services" ("media_player.join",
 * "media_player.*"): what an app may do is visible before it is installed.
 * data is the service data, entity_id in it as usual; opts.response: true
 * for a service that answers (calendar.get_events, weather.get_forecasts).
 * Returns a Promise of that answer (null without one); it rejects with
 * Home Assistant's reason. A call made while the Toon is not connected (just
 * after a start, or a moment without connection) waits for it, up to a
 * minute; then it rejects with the connection's status. A toon.* or mqtt.*
 * entity_id goes to the Toon itself, as a card's button would.
 * Level 8 added to el.image: camera ("camera.door", a Home Assistant camera
 * over the Toon's own connection: it signs the path, so no token in the
 * app; only the cameras the manifest lists in "cameras", "camera.*" for
 * any), stream (MJPEG: frames as they come, from a camera or a url) and fps
 * (a stream's frames a second, 1-10, default 5). JPEGs go through
 * libjpeg-turbo (progressive ones too); one much larger than it is shown is
 * decoded at 1/2, 1/4 or 1/8 of its size.
 * Level 9: el.set({bg, border, borderColor, radius}) on the el that fn of
 * retoon.tile or retoon.screen gets colours the card itself (padding and
 * corners too) or the whole screen; it goes back to the standard colour
 * when the app draws again without it. Before level 9 the same call only
 * coloured the area inside the card's padding. el.set({points, width,
 * color, opacity}) on a line moves it (a clock's hand, a needle); el.set on
 * a circle takes x, y as its centre and r, as el.circle does (before: x, y
 * its top left). Colours may also be the dashboard's names (red, blue,
 * white...). The manifest's settings may be of type "color" (a colour
 * name or "#rrggbb", "" for the app's own) and "select" ("options": a list
 * of values, or of { value, label }).
 * Level 10: settings per card. The manifest's "cardSettings" (as "settings")
 * are set per card in the card's editor and kept in the card as "settings";
 * card.settings, as fn of retoon.tile gets it, is the app's settings with
 * those of the card over them (the defaults of cardSettings in between).
 * retoon.screen's fn gets the card that was tapped the same way: fn(el, card).
 * Level 11: el.image also shows GIFs (from a url or a camera, such as Home
 * Assistant's Buienradar rain radar); an animated one plays at its own
 * frame times, while it is on the screen. At most 100 frames and 8 MB of
 * them at the size shown; a stream shows only a GIF's first frame.
 * Level 12: el.arc({x, y, r, width, start, end, color, opacity, rounded}):
 * a part of a ring as one object (a meter's band); x, y its centre, r the
 * middle of the band, start/end in degrees (0 right, clockwise; 180-360 is
 * the upper half). el.set on an arc changes any of these.
 */
#include <ctype.h>
#include <curl/curl.h>
#include <dirent.h>
#include <math.h>
#include <mosquitto.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <quickjs/quickjs.h>
#include <setjmp.h>
#include <stdio.h>	/* before jpeglib.h, which needs FILE */
#include <jpeglib.h>
#include "src/libs/lodepng/lodepng.h"
#include "gif.h"
#include "ui.h"

#define APPS_API	12
#define APPS_DIR	"/mnt/data/toon-os/apps"
#define APPS_CONF	CONFIG_DIR "/apps"
#define MAX_APPS	16
#define MEM_LIMIT	(24 << 20)
#define CALL_LIMIT_MS	2000
#define MAX_BODY	(4 << 20)
#define CA_FILE		"/etc/ssl/certs/ca-certificates.crt"

struct app {
	char id[48], name[64], version[24];
	JSRuntime *rt;
	JSContext *ctx;
	JSValue tile_fn, screen_fn, settings_fn, state_fn;
	struct app_sub {		/* retoon.subscribe */
		char *pattern;
		JSValue fn;
	} *subs;
	int nsubs;
	char (*watch)[96];		/* retoon.watch: the entities */
	long *seen;			/* their change count as last told to the app
					   (entity.gen), -1 = no state yet */
	int nwatch;
	cJSON *manifest;
	bool failed;			/* did not start: its card shows why */
	char error[200];		/* the last error */
	time_t mt_settings;
	uint32_t deadline;		/* of the call running now */
};

static struct app apps[MAX_APPS];
static int napps;
static JSClassID el_class;

/* ---- elements ------------------------------------------------------------ */

/* shared by the JavaScript object and the LVGL object; whichever goes last frees it */
struct handle {
	lv_obj_t *obj;
	int refs;
};

struct click {
	struct app *a;
	JSValue fn;
};

static struct app *app_of(JSContext *ctx) { return JS_GetContextOpaque(ctx); }

static void handle_put(struct handle *h)
{
	if (h && --h->refs == 0)
		free(h);
}

static void on_obj_delete(lv_event_t *ev)
{
	struct handle *h = lv_event_get_user_data(ev);

	h->obj = NULL;
	handle_put(h);
}

static void el_finalizer(JSRuntime *rt, JSValue val)
{
	(void)rt;
	handle_put(JS_GetOpaque(val, el_class));
}

static JSClassDef el_def = { "Element", .finalizer = el_finalizer };

static JSValue el_new(JSContext *ctx, lv_obj_t *obj)
{
	JSValue v = JS_NewObjectClass(ctx, el_class);
	struct handle *h = calloc(1, sizeof(*h));

	h->obj = obj;
	h->refs = 2;
	lv_obj_add_event_cb(obj, on_obj_delete, LV_EVENT_DELETE, h);
	JS_SetOpaque(v, h);
	return v;
}

static lv_obj_t *el_obj(JSContext *ctx, JSValueConst this)
{
	struct handle *h = JS_GetOpaque(this, el_class);

	return h ? h->obj : NULL;
}

static uint32_t color_of(JSContext *ctx, JSValueConst v, uint32_t def)
{
	static const struct { const char *n; uint32_t c; } names[] = {
		{ "text", C_TEXT }, { "muted", C_MUTED }, { "accent", C_ACCENT },
		{ "ok", C_OK }, { "warn", C_WARN }, { "bad", C_BAD }, { "card", C_CARD },
		{ "background", C_BG },
	};
	const char *s;
	uint32_t c = def;

	if (!JS_IsString(v))
		return def;
	s = JS_ToCString(ctx, v);
	if (s && s[0] == '#' && strlen(s) == 7)
		c = strtoul(s + 1, NULL, 16);
	else
		for (size_t i = 0; s && i < sizeof(names) / sizeof(names[0]); i++)
			if (!strcmp(s, names[i].n))
				c = names[i].c;
	if (c == def && s && s[0] != '#')	/* the dashboard's names: red, blue... */
		c = dash_color(s, def);
	JS_FreeCString(ctx, s);
	return c;
}

static bool opt_num(JSContext *ctx, JSValueConst o, const char *key, double *out)
{
	JSValue v = JS_GetPropertyStr(ctx, o, key);
	bool ok = JS_IsNumber(v) && !JS_ToFloat64(ctx, out, v);

	JS_FreeValue(ctx, v);
	return ok;
}

static lv_align_t align_of(const char *s)
{
	static const struct { const char *n; lv_align_t a; } names[] = {
		{ "top-left", LV_ALIGN_TOP_LEFT }, { "top-mid", LV_ALIGN_TOP_MID },
		{ "top-right", LV_ALIGN_TOP_RIGHT }, { "left-mid", LV_ALIGN_LEFT_MID },
		{ "center", LV_ALIGN_CENTER }, { "right-mid", LV_ALIGN_RIGHT_MID },
		{ "bottom-left", LV_ALIGN_BOTTOM_LEFT }, { "bottom-mid", LV_ALIGN_BOTTOM_MID },
		{ "bottom-right", LV_ALIGN_BOTTOM_RIGHT },
	};

	for (size_t i = 0; s && i < sizeof(names) / sizeof(names[0]); i++)
		if (!strcmp(s, names[i].n))
			return names[i].a;
	return LV_ALIGN_TOP_LEFT;
}

static const lv_font_t *font_of(const char *s)
{
	if (!s)
		return f_body;
	if (!strcmp(s, "small"))
		return f_small;
	if (!strcmp(s, "head"))
		return f_head;
	if (!strcmp(s, "mid"))
		return f_mid;
	if (!strcmp(s, "big"))
		return f_big;
	if (!strcmp(s, "icon"))
		return i_small;
	return f_body;
}

static void apply_style(JSContext *ctx, lv_obj_t *o, JSValueConst opts);

static int array_len(JSContext *ctx, JSValueConst a, int64_t *len)
{
	JSValue v = JS_GetPropertyStr(ctx, a, "length");
	int r = JS_ToInt64(ctx, len, v);

	JS_FreeValue(ctx, v);
	return r;
}

/* the options every element understands */
static void apply_opts(JSContext *ctx, lv_obj_t *o, JSValueConst opts)
{
	double x = 0, y = 0, w, h;
	bool pos;
	JSValue v;

	if (!JS_IsObject(opts))
		return;
	/* apps work in pixels of a 1024 wide screen, like the dashboards */
	if (opt_num(ctx, opts, "w", &w))
		lv_obj_set_width(o, S(w));
	if (opt_num(ctx, opts, "h", &h))
		lv_obj_set_height(o, S(h));
	pos = opt_num(ctx, opts, "x", &x);
	pos |= opt_num(ctx, opts, "y", &y);
	v = JS_GetPropertyStr(ctx, opts, "align");
	if (JS_IsString(v)) {
		const char *s = JS_ToCString(ctx, v);

		lv_obj_align(o, align_of(s), S(x), S(y));
		JS_FreeCString(ctx, s);
	} else if (pos) {
		lv_obj_set_pos(o, S(x), S(y));
	}
	JS_FreeValue(ctx, v);
	apply_style(ctx, o, opts);
}

/* the options about looks, not place or size */
static void apply_style(JSContext *ctx, lv_obj_t *o, JSValueConst opts)
{
	double d;
	JSValue v;

	if (!JS_IsObject(opts))
		return;
	v = JS_GetPropertyStr(ctx, opts, "bg");
	if (JS_IsString(v)) {
		lv_obj_set_style_bg_color(o, lv_color_hex(color_of(ctx, v, C_CARD)), 0);
		lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	}
	JS_FreeValue(ctx, v);
	if (opt_num(ctx, opts, "opacity", &d))
		lv_obj_set_style_opa(o, (lv_opa_t)LV_CLAMP(0, d * 255, 255), 0);
	if (opt_num(ctx, opts, "radius", &d))
		lv_obj_set_style_radius(o, S(d), 0);
	if (opt_num(ctx, opts, "border", &d))
		lv_obj_set_style_border_width(o, (int32_t)d, 0);
	v = JS_GetPropertyStr(ctx, opts, "borderColor");
	if (JS_IsString(v))
		lv_obj_set_style_border_color(o, lv_color_hex(color_of(ctx, v, C_TEXT)), 0);
	JS_FreeValue(ctx, v);
	v = JS_GetPropertyStr(ctx, opts, "color");
	if (JS_IsString(v))
		lv_obj_set_style_text_color(o, lv_color_hex(color_of(ctx, v, C_TEXT)), 0);
	JS_FreeValue(ctx, v);
}

static lv_obj_t *plain_box(lv_obj_t *parent)
{
	lv_obj_t *o = lv_obj_create(parent);

	lv_obj_remove_style_all(o);
	lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
	return o;
}

static JSValue js_box(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
{
	lv_obj_t *p = el_obj(ctx, this), *o;

	if (!p)
		return JS_UNDEFINED;
	o = plain_box(p);
	apply_opts(ctx, o, argc > 0 ? argv[0] : JS_UNDEFINED);
	return el_new(ctx, o);
}

static JSValue js_text(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
{
	lv_obj_t *p = el_obj(ctx, this), *l;
	JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
	const char *s, *f = NULL, *ta = NULL;
	JSValue v;

	if (!p)
		return JS_UNDEFINED;
	l = lv_label_create(p);
	s = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
	lv_label_set_text(l, s ? s : "");
	JS_FreeCString(ctx, s);
	if (JS_IsObject(opts)) {
		v = JS_GetPropertyStr(ctx, opts, "font");
		if (JS_IsString(v))
			f = JS_ToCString(ctx, v);
		JS_FreeValue(ctx, v);
		v = JS_GetPropertyStr(ctx, opts, "textAlign");
		if (JS_IsString(v))
			ta = JS_ToCString(ctx, v);
		JS_FreeValue(ctx, v);
	}
	lv_obj_set_style_text_font(l, font_of(f), 0);
	lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
	if (ta)
		lv_obj_set_style_text_align(l, !strcmp(ta, "center") ? LV_TEXT_ALIGN_CENTER :
					    !strcmp(ta, "right") ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
	JS_FreeCString(ctx, f);
	JS_FreeCString(ctx, ta);
	lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
	apply_opts(ctx, l, opts);
	return el_new(ctx, l);
}

static void on_click(lv_event_t *ev);
static void on_click_delete(lv_event_t *ev);

static void add_click(JSContext *ctx, lv_obj_t *o, JSValueConst fn)
{
	struct click *c;

	if (!JS_IsFunction(ctx, fn))
		return;
	c = calloc(1, sizeof(*c));
	c->a = app_of(ctx);
	c->fn = JS_DupValue(ctx, fn);
	lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(o, on_click, LV_EVENT_CLICKED, c);
	lv_obj_add_event_cb(o, on_click_delete, LV_EVENT_DELETE, c);
}

static JSValue js_button(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
{
	lv_obj_t *p = el_obj(ctx, this), *b, *l;
	const char *s;

	if (!p)
		return JS_UNDEFINED;
	b = lv_button_create(p);
	lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD_HI), 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_set_height(b, S(48));
	l = lv_label_create(b);
	s = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
	lv_label_set_text(l, s ? s : "");
	JS_FreeCString(ctx, s);
	lv_obj_set_style_text_font(l, f_body, 0);
	lv_obj_center(l);
	apply_opts(ctx, b, argc > 1 ? argv[1] : JS_UNDEFINED);
	if (argc > 2)
		add_click(ctx, b, argv[2]);
	return el_new(ctx, b);
}

static JSValue js_on_click(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
{
	lv_obj_t *o = el_obj(ctx, this);

	if (o && argc > 0)
		add_click(ctx, o, argv[0]);
	return JS_UNDEFINED;
}

static JSValue js_width(JSContext *ctx, JSValueConst this)
{
	lv_obj_t *o = el_obj(ctx, this);

	if (!o)
		return JS_NewInt32(ctx, 0);
	lv_obj_update_layout(o);
	return JS_NewInt32(ctx, lv_obj_get_content_width(o) * 1024 / screen_w);
}

static JSValue js_height(JSContext *ctx, JSValueConst this)
{
	lv_obj_t *o = el_obj(ctx, this);

	if (!o)
		return JS_NewInt32(ctx, 0);
	lv_obj_update_layout(o);
	return JS_NewInt32(ctx, lv_obj_get_content_height(o) * 1024 / screen_w);
}

static JSValue js_icon(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
{
	lv_obj_t *p = el_obj(ctx, this), *l;
	JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
	const char *s, *sz = NULL;
	JSValue v;

	if (!p)
		return JS_UNDEFINED;
	l = lv_label_create(p);
	v = JS_IsObject(opts) ? JS_GetPropertyStr(ctx, opts, "size") : JS_UNDEFINED;
	if (JS_IsString(v))
		sz = JS_ToCString(ctx, v);
	JS_FreeValue(ctx, v);
	lv_obj_set_style_text_font(l, sz && !strcmp(sz, "big") ? i_big :
				   sz && !strcmp(sz, "mid") ? i_mid : i_small, 0);
	JS_FreeCString(ctx, sz);
	s = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
	lv_label_set_text(l, mdi(s ? s : "", mdi("mdi:help-circle-outline", "?")));
	JS_FreeCString(ctx, s);
	lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
	lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
	apply_opts(ctx, l, opts);
	return el_new(ctx, l);
}

/* circle: x, y are its centre */
static JSValue js_circle(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
{
	lv_obj_t *p = el_obj(ctx, this), *o;
	JSValueConst opts = argc > 0 ? argv[0] : JS_UNDEFINED;
	double x = 0, y = 0, r = 10;

	if (!p)
		return JS_UNDEFINED;
	o = plain_box(p);
	opt_num(ctx, opts, "x", &x);
	opt_num(ctx, opts, "y", &y);
	opt_num(ctx, opts, "r", &r);
	lv_obj_set_size(o, S(2 * r), S(2 * r));
	lv_obj_set_pos(o, S(x - r), S(y - r));
	lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
	lv_obj_add_flag(o, LV_OBJ_FLAG_USER_3);	/* a circle: x, y are its centre */
	lv_obj_set_style_bg_color(o, lv_color_hex(C_CARD_HI), 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	/* the rest (bg, border, ...); x, y were the centre */
	apply_style(ctx, o, opts);
	lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(o, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	return el_new(ctx, o);
}

static void on_line_delete(lv_event_t *ev)
{
	free(lv_line_get_points_mutable(lv_event_get_target(ev)));
}

/* an arc (level 12): a part of a ring, drawn by LVGL as one object instead
   of an app's many short lines. x, y its centre, r the middle of its band,
   width the band, start and end in degrees: 0 is to the right, clockwise
   as y grows downwards (180 to 360 is the upper half ring). Kept in the
   app's own units, so el.set can change one of them */
struct arc {
	double x, y, r, width, start, end;
};

static void on_arc_delete(lv_event_t *ev)
{
	free(lv_obj_get_user_data(lv_event_get_target(ev)));
}

static void arc_opts(JSContext *ctx, lv_obj_t *o, JSValueConst opts)
{
	struct arc *a = lv_obj_get_user_data(o);
	double span, s;
	int32_t d, w;
	JSValue v;

	opt_num(ctx, opts, "x", &a->x);
	opt_num(ctx, opts, "y", &a->y);
	opt_num(ctx, opts, "r", &a->r);
	opt_num(ctx, opts, "width", &a->width);
	opt_num(ctx, opts, "start", &a->start);
	opt_num(ctx, opts, "end", &a->end);
	w = LV_MAX(1, S(a->width));
	d = LV_MAX(1, S(2 * a->r) + w);
	lv_obj_set_size(o, d, d);
	lv_obj_set_pos(o, S(a->x) - d / 2, S(a->y) - d / 2);
	lv_obj_set_style_arc_width(o, w, 0);
	/* 0 .. 360 degrees from start; nothing at all for none */
	span = LV_CLAMP(0.0, a->end - a->start, 360.0);
	s = fmod(a->start, 360);
	if (s < 0)
		s += 360;
	if (span < 0.5) {
		lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
		if (span > 359.5)
			lv_arc_set_bg_angles(o, 0, 360);
		else
			lv_arc_set_bg_angles(o, (lv_value_precise_t)lround(s),
					     (lv_value_precise_t)(lround(s + span) % 360));
	}
	v = JS_GetPropertyStr(ctx, opts, "color");
	if (JS_IsString(v))
		lv_obj_set_style_arc_color(o, lv_color_hex(color_of(ctx, v, C_ACCENT)), 0);
	JS_FreeValue(ctx, v);
	v = JS_GetPropertyStr(ctx, opts, "rounded");
	if (!JS_IsUndefined(v))
		lv_obj_set_style_arc_rounded(o, JS_ToBool(ctx, v) > 0, 0);
	JS_FreeValue(ctx, v);
	if (opt_num(ctx, opts, "opacity", &s))
		lv_obj_set_style_arc_opa(o, (lv_opa_t)LV_CLAMP(0, s * 255, 255), 0);
}

static JSValue js_arc(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
{
	lv_obj_t *p = el_obj(ctx, this), *o;
	JSValueConst opts = argc > 0 ? argv[0] : JS_UNDEFINED;
	struct arc *a;

	if (!p)
		return JS_UNDEFINED;
	if (!(a = calloc(1, sizeof(*a))))
		return JS_ThrowOutOfMemory(ctx);
	*a = (struct arc){ .r = 50, .width = 8, .start = 180, .end = 360 };
	o = lv_arc_create(p);
	lv_obj_set_user_data(o, a);
	lv_obj_add_event_cb(o, on_arc_delete, LV_EVENT_DELETE, NULL);
	lv_obj_remove_style(o, NULL, LV_PART_KNOB);
	lv_obj_remove_style(o, NULL, LV_PART_INDICATOR);
	lv_obj_set_style_arc_opa(o, LV_OPA_TRANSP, LV_PART_INDICATOR);
	lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(o, 0, 0);
	lv_obj_set_style_pad_all(o, 0, 0);
	lv_obj_set_style_arc_color(o, lv_color_hex(C_ACCENT), 0);
	lv_obj_set_style_arc_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_arc_rounded(o, false, 0);
	lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
	arc_opts(ctx, o, opts);
	return el_new(ctx, o);
}

/* [x1, y1, x2, y2, ...] in screen pixels, or NULL (an exception set) */
static lv_point_precise_t *line_points(JSContext *ctx, JSValueConst arr, uint32_t *n)
{
	lv_point_precise_t *pts;
	int64_t len = 0;

	if (!JS_IsArray(ctx, arr) || array_len(ctx, arr, &len) || len < 4) {
		JS_ThrowTypeError(ctx, "line needs [x1, y1, x2, y2, ...]");
		return NULL;
	}
	len /= 2;
	pts = calloc(len, sizeof(*pts));
	for (int64_t i = 0; i < 2 * len; i++) {
		JSValue v = JS_GetPropertyUint32(ctx, arr, i);
		double d = 0;

		JS_ToFloat64(ctx, &d, v);
		JS_FreeValue(ctx, v);
		if (i % 2)
			pts[i / 2].y = S(d);
		else
			pts[i / 2].x = S(d);
	}
	*n = len;
	return pts;
}

/* a line's own looks: width, color, opacity */
static void line_style(JSContext *ctx, lv_obj_t *l, JSValueConst opts)
{
	double d;
	JSValue v;

	if (!JS_IsObject(opts))
		return;
	if (opt_num(ctx, opts, "width", &d))
		lv_obj_set_style_line_width(l, LV_MAX(1, S(d)), 0);
	v = JS_GetPropertyStr(ctx, opts, "color");
	if (JS_IsString(v))
		lv_obj_set_style_line_color(l, lv_color_hex(color_of(ctx, v, C_MUTED)), 0);
	JS_FreeValue(ctx, v);
	/* the line's own opacity: a whole-object one is drawn through a layer */
	if (opt_num(ctx, opts, "opacity", &d))
		lv_obj_set_style_line_opa(l, (lv_opa_t)LV_CLAMP(0, d * 255, 255), 0);
}

static JSValue js_line(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
{
	lv_obj_t *p = el_obj(ctx, this), *l;
	JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
	lv_point_precise_t *pts;
	uint32_t n;

	if (!p || argc < 1)
		return JS_ThrowTypeError(ctx, "line needs [x1, y1, x2, y2, ...]");
	if (!(pts = line_points(ctx, argv[0], &n)))
		return JS_EXCEPTION;
	l = lv_line_create(p);
	lv_line_set_points_mutable(l, pts, n);
	lv_obj_add_event_cb(l, on_line_delete, LV_EVENT_DELETE, NULL);
	lv_obj_set_style_line_width(l, S(2) > 1 ? S(2) : 1, 0);
	lv_obj_set_style_line_rounded(l, true, 0);
	lv_obj_set_style_line_color(l, lv_color_hex(C_MUTED), 0);
	line_style(ctx, l, opts);
	lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
	return el_new(ctx, l);
}

/* ---- flows: dots moving along a line by themselves ------------------------ */

/* The dots are drawn by the holder itself (not one object each): moving them
 * then only marks their old and new spots for drawing, without the style and
 * layout work of moving objects. */
#define FLOW_MAX_DOTS	24
#define FLOW_FRAME_MS	50

struct flow {
	lv_obj_t *holder;
	int ndots;
	float x1, y1, dx, dy, len;	/* screen pixels, in the holder */
	float speed, gap, phase;	/* screen pixels (a second) */
	int size;
	bool done;			/* moved in this round already */
	lv_color_t color;
	lv_area_t drawn[FLOW_MAX_DOTS];	/* where each dot is now, absolute; x1 > x2 = none */
	struct flow *next;
};

static struct flow *flows;
static lv_timer_t *flow_timer;

/* where dot i belongs now (absolute coordinates); false: not on the line */
static bool flow_dot(struct flow *f, int i, lv_area_t *a)
{
	float t = fmodf(f->phase + i * f->gap, f->ndots * f->gap);
	lv_area_t c;
	int32_t x, y;

	if (f->speed == 0 || t > f->len)
		return false;
	lv_obj_get_coords(f->holder, &c);
	x = c.x1 + (int32_t)(f->x1 + f->dx * t - f->size / 2.0f);
	y = c.y1 + (int32_t)(f->y1 + f->dy * t - f->size / 2.0f);
	lv_area_set(a, x, y, x + f->size - 1, y + f->size - 1);
	return true;
}

static void area_add(lv_area_t *all, const lv_area_t *a, bool *any)
{
	if (!*any) {
		*all = *a;
		*any = true;
		return;
	}
	all->x1 = LV_MIN(all->x1, a->x1);
	all->y1 = LV_MIN(all->y1, a->y1);
	all->x2 = LV_MAX(all->x2, a->x2);
	all->y2 = LV_MAX(all->y2, a->y2);
}

/* the old and new spots of all dots of a line, added to *all */
static void flow_place(struct flow *f, lv_area_t *all, bool *any)
{
	for (int i = 0; i < f->ndots; i++) {
		lv_area_t a, *old = &f->drawn[i];
		bool on = flow_dot(f, i, &a);

		if (on && old->x1 <= old->x2 && a.x1 == old->x1 && a.y1 == old->y1)
			continue;
		if (old->x1 <= old->x2)
			area_add(all, old, any);
		if (on)
			area_add(all, &a, any);
		else
			lv_area_set(&a, 1, 0, 0, 0);
		*old = a;
	}
}

static void flow_place_now(struct flow *f)
{
	lv_area_t all;
	bool any = false;

	flow_place(f, &all, &any);
	if (any)
		lv_obj_invalidate_area(f->holder, &all);
}

static void on_flow_draw(lv_event_t *ev)
{
	struct flow *f = lv_event_get_user_data(ev);
	lv_layer_t *layer = lv_event_get_layer(ev);
	lv_draw_rect_dsc_t d;

	lv_draw_rect_dsc_init(&d);
	d.radius = LV_RADIUS_CIRCLE;
	d.bg_color = f->color;
	d.bg_opa = LV_OPA_COVER;
	for (int i = 0; i < f->ndots; i++)
		if (f->drawn[i].x1 <= f->drawn[i].x2)
			lv_draw_rect(layer, &d, &f->drawn[i]);
}

/* Moves all dots. Every changed area costs LVGL a fixed amount of work
 * (about a millisecond on a Toon 2), so the dots of all lines of one drawing
 * become one area: the lines meet in the middle anyway. */
static void flow_tick(lv_timer_t *t)
{
	static uint32_t last;
	uint32_t now = lv_tick_get();
	float dt = (now - last) / 1000.0f;

	(void)t;
	if (!last || dt > 0.5f)
		dt = FLOW_FRAME_MS / 1000.0f;
	last = now;
	for (struct flow *f = flows; f; f = f->next)
		f->done = false;
	for (struct flow *f = flows; f; f = f->next) {
		lv_obj_t *parent = lv_obj_get_parent(f->holder);
		lv_area_t all;
		bool any = false;

		if (f->done)
			continue;
		/* this flow and the others drawn on the same element */
		for (struct flow *g = f; g; g = g->next) {
			float cycle = g->ndots * g->gap;

			if (g->done || lv_obj_get_parent(g->holder) != parent)
				continue;
			g->done = true;
			if (g->speed == 0 || !cycle)
				continue;
			g->phase = fmodf(g->phase + g->speed * dt + cycle, cycle);
			flow_place(g, &all, &any);
		}
		if (any)
			lv_obj_invalidate_area(f->holder, &all);
	}
}

static void on_flow_delete(lv_event_t *ev)
{
	struct flow *f = lv_event_get_user_data(ev), **pp;

	for (pp = &flows; *pp; pp = &(*pp)->next)
		if (*pp == f) {
			*pp = f->next;
			break;
		}
	free(f);
	if (!flows && flow_timer) {
		lv_timer_delete(flow_timer);
		flow_timer = NULL;
	}
}

static struct flow *flow_of(lv_obj_t *o)
{
	for (struct flow *f = flows; f; f = f->next)
		if (f->holder == o)
			return f;
	return NULL;
}

static void flow_opts(JSContext *ctx, struct flow *f, JSValueConst opts)
{
	double d;
	JSValue v;

	if (opt_num(ctx, opts, "speed", &d))
		f->speed = S(d);
	v = JS_GetPropertyStr(ctx, opts, "color");
	if (JS_IsString(v)) {
		f->color = lv_color_hex(color_of(ctx, v, C_ACCENT));
		lv_obj_invalidate(f->holder);
	}
	JS_FreeValue(ctx, v);
	if (opt_num(ctx, opts, "opacity", &d))
		lv_obj_set_style_opa(f->holder, (lv_opa_t)LV_CLAMP(0, d * 255, 255), 0);
	flow_place_now(f);
}

static JSValue js_flow(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
{
	lv_obj_t *p = el_obj(ctx, this);
	JSValueConst opts = argc > 0 ? argv[0] : JS_UNDEFINED;
	double x1 = 0, y1 = 0, x2 = 0, y2 = 0, gap = 30, size = 8;
	struct flow *f;

	if (!p || !JS_IsObject(opts))
		return JS_UNDEFINED;
	opt_num(ctx, opts, "x1", &x1);
	opt_num(ctx, opts, "y1", &y1);
	opt_num(ctx, opts, "x2", &x2);
	opt_num(ctx, opts, "y2", &y2);
	opt_num(ctx, opts, "gap", &gap);
	opt_num(ctx, opts, "size", &size);
	f = calloc(1, sizeof(*f));
	f->holder = plain_box(p);
	lv_obj_set_size(f->holder, lv_pct(100), lv_pct(100));
	f->size = LV_MAX(2, S(size));
	f->x1 = S(x1);
	f->y1 = S(y1);
	f->len = sqrtf((S(x2) - f->x1) * (S(x2) - f->x1) + (S(y2) - f->y1) * (S(y2) - f->y1));
	f->dx = f->len ? (S(x2) - f->x1) / f->len : 0;
	f->dy = f->len ? (S(y2) - f->y1) / f->len : 0;
	f->gap = LV_MAX(f->size + 2, S(gap));
	f->ndots = LV_CLAMP(1, (int)(f->len / f->gap) + 1, FLOW_MAX_DOTS);
	f->color = lv_color_hex(C_ACCENT);
	for (int i = 0; i < FLOW_MAX_DOTS; i++)
		lv_area_set(&f->drawn[i], 1, 0, 0, 0);
	f->next = flows;
	flows = f;
	lv_obj_add_event_cb(f->holder, on_flow_draw, LV_EVENT_DRAW_MAIN, f);
	lv_obj_add_event_cb(f->holder, on_flow_delete, LV_EVENT_DELETE, f);
	if (!flow_timer)
		flow_timer = lv_timer_create(flow_tick, FLOW_FRAME_MS, NULL);
	flow_opts(ctx, f, opts);
	return el_new(ctx, f->holder);
}

/* ---- time picker: two scroll wheels, hours and minutes ---------------------- */

static bool call(struct app *a, JSValueConst fn, int argc, JSValueConst *argv, const char *what);

struct picker {
	struct app *a;
	JSValue fn;		/* fn("HH:MM") after the user turned a wheel */
	lv_obj_t *holder, *hours, *minutes;
	int step;		/* minutes between the minute wheel's entries */
	struct picker *next;
};

static struct picker *pickers;

static struct picker *picker_of(lv_obj_t *o)
{
	for (struct picker *p = pickers; p; p = p->next)
		if (p->holder == o)
			return p;
	return NULL;
}

/* "HH:MM" (or "H:MM") -> wheel positions; false when it is no time */
static bool picker_parse(const char *s, int step, int *hour, int *slot)
{
	int h, m;

	if (!s || sscanf(s, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59)
		return false;
	*hour = h;
	*slot = m / step;	/* a minute between two entries goes to the lower one */
	return true;
}

static void picker_value(JSContext *ctx, struct picker *p, JSValueConst v)
{
	const char *s;
	int h, slot;

	if (!JS_IsString(v))
		return;
	s = JS_ToCString(ctx, v);
	if (picker_parse(s, p->step, &h, &slot)) {
		lv_roller_set_selected(p->hours, h, LV_ANIM_OFF);
		lv_roller_set_selected(p->minutes, slot, LV_ANIM_OFF);
	}
	JS_FreeCString(ctx, s);
}

static void on_picker_change(lv_event_t *ev)
{
	struct picker *p = lv_event_get_user_data(ev);
	char buf[8];
	JSValue v;

	snprintf(buf, sizeof(buf), "%02d:%02d", (int)lv_roller_get_selected(p->hours),
		 (int)lv_roller_get_selected(p->minutes) * p->step);
	v = JS_NewString(p->a->ctx, buf);
	call(p->a, p->fn, 1, (JSValueConst *)&v, "time");
	JS_FreeValue(p->a->ctx, v);
}

static void on_picker_delete(lv_event_t *ev)
{
	struct picker *p = lv_event_get_user_data(ev), **pp;

	for (pp = &pickers; *pp; pp = &(*pp)->next)
		if (*pp == p) {
			*pp = p->next;
			break;
		}
	JS_FreeValue(p->a->ctx, p->fn);
	free(p);
}

static lv_obj_t *picker_wheel(lv_obj_t *parent, const char *options, int rows, const lv_font_t *font)
{
	lv_obj_t *r = lv_roller_create(parent);

	lv_roller_set_options(r, options, LV_ROLLER_MODE_INFINITE);
	lv_roller_set_visible_row_count(r, rows);
	lv_obj_set_style_text_font(r, font, 0);
	lv_obj_set_style_text_color(r, lv_color_hex(C_MUTED), 0);
	lv_obj_set_style_bg_color(r, lv_color_hex(C_CARD_HI), 0);
	lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(r, 0, 0);
	lv_obj_set_style_radius(r, S(8), 0);
	lv_obj_set_style_pad_hor(r, S(12), 0);
	/* the chosen row */
	lv_obj_set_style_text_color(r, lv_color_hex(C_TEXT), LV_PART_SELECTED);
	lv_obj_set_style_bg_color(r, lv_color_hex(C_ACCENT), LV_PART_SELECTED);
	lv_obj_set_style_bg_opa(r, LV_OPA_40, LV_PART_SELECTED);
	return r;
}

/* el.time(opts, fn): a time picker. opts: value "HH:MM", step (minutes, 1-30,
   a divisor of 60; default 1), rows (visible rows, 1-7, default 3), font and
   the usual place options. fn("HH:MM") after each turn of a wheel. */
static JSValue js_time(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
{
	lv_obj_t *parent = el_obj(ctx, this), *colon;
	JSValueConst opts = argc > 0 ? argv[0] : JS_UNDEFINED;
	double d;
	int step = 1, rows = 3;
	const char *fname = NULL;
	char *hopts, *mopts, *w;
	const lv_font_t *font;
	struct picker *p;
	JSValue v;

	if (!parent)
		return JS_UNDEFINED;
	if (JS_IsObject(opts)) {
		if (opt_num(ctx, opts, "step", &d) && d >= 1 && d <= 30 && 60 % (int)d == 0)
			step = (int)d;
		if (opt_num(ctx, opts, "rows", &d))
			rows = LV_CLAMP(1, (int)d, 7);
		v = JS_GetPropertyStr(ctx, opts, "font");
		if (JS_IsString(v))
			fname = JS_ToCString(ctx, v);
		JS_FreeValue(ctx, v);
	}
	font = font_of(fname);
	JS_FreeCString(ctx, fname);

	/* "00\n01\n...\n23" and "00\n<step>\n..." */
	w = hopts = malloc(24 * 3);
	for (int h = 0; h < 24; h++)
		w += sprintf(w, h ? "\n%02d" : "%02d", h);
	w = mopts = malloc(60 * 3);
	for (int m = 0; m < 60; m += step)
		w += sprintf(w, m ? "\n%02d" : "%02d", m);

	p = calloc(1, sizeof(*p));
	p->a = app_of(ctx);
	p->fn = argc > 1 && JS_IsFunction(ctx, argv[1]) ? JS_DupValue(ctx, argv[1]) : JS_UNDEFINED;
	p->step = step;
	p->holder = plain_box(parent);
	lv_obj_set_size(p->holder, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(p->holder, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(p->holder, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(p->holder, S(6), 0);
	/* the wheels take the drags and taps; they must not open the card's screen */
	lv_obj_remove_flag(p->holder, LV_OBJ_FLAG_EVENT_BUBBLE);
	p->hours = picker_wheel(p->holder, hopts, rows, font);
	colon = lv_label_create(p->holder);
	lv_label_set_text(colon, ":");
	lv_obj_set_style_text_font(colon, font, 0);
	lv_obj_set_style_text_color(colon, lv_color_hex(C_TEXT), 0);
	p->minutes = picker_wheel(p->holder, mopts, rows, font);
	free(hopts);
	free(mopts);
	lv_obj_add_event_cb(p->hours, on_picker_change, LV_EVENT_VALUE_CHANGED, p);
	lv_obj_add_event_cb(p->minutes, on_picker_change, LV_EVENT_VALUE_CHANGED, p);
	lv_obj_add_event_cb(p->holder, on_picker_delete, LV_EVENT_DELETE, p);
	p->next = pickers;
	pickers = p;
	if (JS_IsObject(opts)) {
		v = JS_GetPropertyStr(ctx, opts, "value");
		picker_value(ctx, p, v);
		JS_FreeValue(ctx, v);
	}
	apply_opts(ctx, p->holder, opts);
	return el_new(ctx, p->holder);
}

/* change an element in place: its options, a text's text, a flow's speed,
   a time picker's value */
static lv_obj_t *frame_of(lv_obj_t *o);

static JSValue js_set(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
{
	lv_obj_t *o = el_obj(ctx, this);
	JSValueConst opts = argc > 0 ? argv[0] : JS_UNDEFINED;
	struct flow *f;
	struct picker *pk;
	JSValue v;

	if (!o || !JS_IsObject(opts))
		return JS_UNDEFINED;
	if ((f = flow_of(o))) {
		flow_opts(ctx, f, opts);
		return JS_UNDEFINED;
	}
	if ((pk = picker_of(o))) {
		v = JS_GetPropertyStr(ctx, opts, "value");
		picker_value(ctx, pk, v);	/* no fn call: only a turn by the user */
		JS_FreeValue(ctx, v);
	}
	v = JS_GetPropertyStr(ctx, opts, "text");
	if (JS_IsString(v) && lv_obj_check_type(o, &lv_label_class)) {
		const char *s = JS_ToCString(ctx, v);

		if (s && strcmp(s, lv_label_get_text(o)))
			lv_label_set_text(o, s);
		JS_FreeCString(ctx, s);
	}
	JS_FreeValue(ctx, v);
	if (lv_obj_check_type(o, &lv_line_class)) {
		/* a line: new points (a hand, a needle), its looks */
		v = JS_GetPropertyStr(ctx, opts, "points");
		if (!JS_IsUndefined(v)) {
			lv_point_precise_t *pts, *old = lv_line_get_points_mutable(o);
			uint32_t n;

			if (!(pts = line_points(ctx, v, &n))) {
				JS_FreeValue(ctx, v);
				return JS_EXCEPTION;
			}
			lv_line_set_points_mutable(o, pts, n);
			free(old);
		}
		JS_FreeValue(ctx, v);
		line_style(ctx, o, opts);
		return JS_UNDEFINED;
	}
	if (lv_obj_check_type(o, &lv_arc_class)) {
		/* an arc: any of its options, the others stay */
		arc_opts(ctx, o, opts);
		return JS_UNDEFINED;
	}
	if (lv_obj_has_flag(o, LV_OBJ_FLAG_USER_3)) {
		/* a circle: x, y its centre and r its radius, as el.circle */
		double x, y, r;
		bool hx, hy, hr;
		int32_t cx, cy, d;

		lv_obj_update_layout(o);
		d = lv_obj_get_width(o);
		cx = lv_obj_get_x(o) + d / 2;
		cy = lv_obj_get_y(o) + d / 2;
		hx = opt_num(ctx, opts, "x", &x);
		hy = opt_num(ctx, opts, "y", &y);
		hr = opt_num(ctx, opts, "r", &r);
		if (hr) {
			d = S(2 * r);
			lv_obj_set_size(o, d, d);
		}
		if (hx)
			cx = S(x);
		if (hy)
			cy = S(y);
		if (hx || hy || hr)
			lv_obj_set_pos(o, cx - d / 2, cy - d / 2);
		apply_style(ctx, o, opts);
		return JS_UNDEFINED;
	}
	if (frame_of(o)) {
		/* the tile's or screen's own el: its looks go to the card or
		   screen around it, its place and size stay as they are */
		apply_style(ctx, frame_of(o), opts);
		return JS_UNDEFINED;
	}
	apply_opts(ctx, o, opts);
	return JS_UNDEFINED;
}

static JSValue js_image(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv);

static const JSCFunctionListEntry el_proto[] = {
	JS_CFUNC_DEF("box", 1, js_box),
	JS_CFUNC_DEF("text", 2, js_text),
	JS_CFUNC_DEF("button", 3, js_button),
	JS_CFUNC_DEF("icon", 2, js_icon),
	JS_CFUNC_DEF("circle", 1, js_circle),
	JS_CFUNC_DEF("arc", 1, js_arc),
	JS_CFUNC_DEF("line", 2, js_line),
	JS_CFUNC_DEF("flow", 1, js_flow),
	JS_CFUNC_DEF("time", 2, js_time),
	JS_CFUNC_DEF("image", 1, js_image),
	JS_CFUNC_DEF("set", 1, js_set),
	JS_CFUNC_DEF("onClick", 1, js_on_click),
	JS_CGETSET_DEF("width", js_width, NULL),
	JS_CGETSET_DEF("height", js_height, NULL),
};

/* ---- calling into an app --------------------------------------------------- */

static void report(struct app *a, const char *what)
{
	JSValue ex = JS_GetException(a->ctx), st;
	const char *msg = JS_ToCString(a->ctx, ex), *stack = NULL;

	st = JS_IsObject(ex) ? JS_GetPropertyStr(a->ctx, ex, "stack") : JS_UNDEFINED;
	if (JS_IsString(st))
		stack = JS_ToCString(a->ctx, st);
	fprintf(stderr, "app %s: %s: %s\n%s", a->id, what, msg ? msg : "?", stack ? stack : "");
	snprintf(a->error, sizeof(a->error), "%s: %s", what, msg ? msg : "error");
	if (!strcmp(what, "start"))
		a->failed = true;
	JS_FreeCString(a->ctx, stack);
	JS_FreeCString(a->ctx, msg);
	JS_FreeValue(a->ctx, st);
	JS_FreeValue(a->ctx, ex);
}

static void run_jobs(struct app *a)
{
	JSContext *c;

	a->deadline = now_ms() + CALL_LIMIT_MS;
	while (JS_ExecutePendingJob(a->rt, &c) > 0)
		;
	a->deadline = 0;
}

/* call fn(args) of app a; false when it threw */
static bool call(struct app *a, JSValueConst fn, int argc, JSValueConst *argv, const char *what)
{
	JSValue r;
	bool ok;

	if (!JS_IsFunction(a->ctx, fn))
		return false;
	a->deadline = now_ms() + CALL_LIMIT_MS;
	r = JS_Call(a->ctx, fn, JS_UNDEFINED, argc, argv);
	a->deadline = 0;
	ok = !JS_IsException(r);
	if (!ok)
		report(a, what);
	JS_FreeValue(a->ctx, r);
	run_jobs(a);
	return ok;
}

static int interrupt(JSRuntime *rt, void *opaque)
{
	struct app *a = opaque;

	(void)rt;
	return a->deadline && (int32_t)(now_ms() - a->deadline) > 0;
}

static void on_click(lv_event_t *ev)
{
	struct click *c = lv_event_get_user_data(ev);

	call(c->a, c->fn, 0, NULL, "click");
}

static void on_click_delete(lv_event_t *ev)
{
	struct click *c = lv_event_get_user_data(ev);

	JS_FreeValue(c->a->ctx, c->fn);
	free(c);
}

/* ---- cards and the full screen ---------------------------------------------- */

struct card {
	struct app *a;
	lv_obj_t *root, *area;
	cJSON *cfg;
	int h;			/* the height it was drawn for */
	bool drawing, resized;
	struct card *next;
};

static struct card *cards;
static lv_obj_t *screen_win, *screen_area;
static struct app *screen_app;

/* the card (or app screen) whose own el o is: the box that fresh() made
   in its area. NULL for anything an app drew. */
static lv_obj_t *frame_of(lv_obj_t *o)
{
	lv_obj_t *p = lv_obj_get_parent(o);

	if (!p)
		return NULL;
	if (p == screen_area && screen_area)
		return screen_win;
	for (struct card *c = cards; c; c = c->next)
		if (p == c->area)
			return c->root;
	return NULL;
}

/* the standard looks, before the app draws (it may set others) */
static void frame_reset(lv_obj_t *o, uint32_t bg, int radius)
{
	lv_obj_set_style_radius(o, radius, 0);
	lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(o, 0, 0);
	lv_obj_set_style_opa(o, LV_OPA_COVER, 0);
}

static void card_delete(lv_event_t *ev)
{
	struct card *c = lv_event_get_user_data(ev), **pp;

	for (pp = &cards; *pp; pp = &(*pp)->next)
		if (*pp == c) {
			*pp = c->next;
			break;
		}
	free(c);
}

static void show_error(lv_obj_t *area, struct app *a)
{
	lv_obj_t *l = lv_label_create(area);

	lv_obj_set_style_text_font(l, f_small, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(C_BAD), 0);
	lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(l, lv_pct(100));
	lv_label_set_text_fmt(l, "%s: %s", a->name, a->error);
}

/* a fresh drawing area: whatever the app drew before goes with the old one */
static lv_obj_t *fresh(lv_obj_t *holder)
{
	lv_obj_t *o;

	lv_obj_clean(holder);
	o = plain_box(holder);
	lv_obj_set_size(o, lv_pct(100), lv_pct(100));
	return o;
}

/* a card's JSON for the app, with "settings": the app's settings, the
   defaults of the manifest's cardSettings, then the card's own (level 10) */
static JSValue card_arg(struct app *a, cJSON *cfg)
{
	char *s = cfg ? cJSON_PrintUnformatted(cfg) : NULL;
	JSValue card = s ? JS_ParseJSON(a->ctx, s, strlen(s), "<card>") : JS_NewObject(a->ctx);
	JSValue merged = JS_NewObject(a->ctx), g, rt, app_set;
	JSPropertyEnum *props;
	uint32_t n;
	cJSON *it;

	free(s);
	if (!JS_IsObject(card)) {
		JS_FreeValue(a->ctx, card);
		card = JS_NewObject(a->ctx);
	}
	g = JS_GetGlobalObject(a->ctx);
	rt = JS_GetPropertyStr(a->ctx, g, "retoon");
	app_set = JS_GetPropertyStr(a->ctx, rt, "settings");
	if (JS_IsObject(app_set) &&
	    !JS_GetOwnPropertyNames(a->ctx, &props, &n, app_set, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)) {
		for (uint32_t i = 0; i < n; i++) {
			JS_SetProperty(a->ctx, merged, props[i].atom,
				       JS_GetProperty(a->ctx, app_set, props[i].atom));
			JS_FreeAtom(a->ctx, props[i].atom);
		}
		js_free(a->ctx, props);
	}
	JS_FreeValue(a->ctx, app_set);
	JS_FreeValue(a->ctx, rt);
	JS_FreeValue(a->ctx, g);
	cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(a->manifest, "cardSettings")) {
		cJSON *k = cJSON_GetObjectItemCaseSensitive(it, "key");
		cJSON *d = cJSON_GetObjectItemCaseSensitive(it, "default");
		char *j = d ? cJSON_PrintUnformatted(d) : NULL;

		if (cJSON_IsString(k) && j)
			JS_SetPropertyStr(a->ctx, merged, k->valuestring, JS_ParseJSON(a->ctx, j, strlen(j), "<default>"));
		free(j);
	}
	cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(cfg, "settings")) {
		char *j = cJSON_PrintUnformatted(it);

		if (it->string && j)
			JS_SetPropertyStr(a->ctx, merged, it->string, JS_ParseJSON(a->ctx, j, strlen(j), "<setting>"));
		free(j);
	}
	JS_SetPropertyStr(a->ctx, card, "settings", merged);
	return card;
}

static void draw_card(struct card *c)
{
	struct app *a = c->a;
	lv_obj_t *area;
	bool ok = false;
	JSValue args[2];

	if (c->drawing)
		return;
	c->drawing = true;
	lv_obj_update_layout(c->root);
	c->h = lv_obj_get_height(c->root);
	frame_reset(c->root, C_CARD, S(14));
	area = fresh(c->area);

	if (!a->failed && JS_IsFunction(a->ctx, a->tile_fn)) {
		args[0] = el_new(a->ctx, area);
		args[1] = card_arg(a, c->cfg);
		ok = call(a, a->tile_fn, 2, args, "tile");
		JS_FreeValue(a->ctx, args[0]);
		JS_FreeValue(a->ctx, args[1]);
	}
	if (!ok)
		show_error(fresh(c->area), a);
	c->drawing = false;
}

static cJSON *screen_cfg;	/* the card that opened the screen (a copy) */

static void screen_close(lv_event_t *ev)
{
	(void)ev;
	if (screen_win)
		lv_obj_delete(screen_win);
	screen_win = screen_area = NULL;
	screen_app = NULL;
	cJSON_Delete(screen_cfg);
	screen_cfg = NULL;
}

void apps_close(void)
{
	screen_close(NULL);
}

static void draw_screen(void)
{
	struct app *a = screen_app;
	lv_obj_t *area;
	bool ok = false;
	JSValue args[2];

	if (!screen_area)
		return;
	frame_reset(screen_win, C_BG, 0);
	area = fresh(screen_area);
	if (!a->failed) {
		args[0] = el_new(a->ctx, area);
		args[1] = card_arg(a, screen_cfg);
		ok = call(a, a->screen_fn, 2, args, "screen");
		JS_FreeValue(a->ctx, args[0]);
		JS_FreeValue(a->ctx, args[1]);
	}
	if (!ok)
		show_error(fresh(screen_area), a);
}

static void screen_open(struct app *a, cJSON *cfg)
{
	lv_obj_t *l, *b;

	if (screen_win || !JS_IsFunction(a->ctx, a->screen_fn))
		return;
	screen_app = a;
	screen_cfg = cfg ? cJSON_Duplicate(cfg, true) : NULL;
	screen_win = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(screen_win);
	lv_obj_set_size(screen_win, SCREEN_W, SCREEN_H);
	lv_obj_set_style_bg_color(screen_win, lv_color_hex(C_BG), 0);
	lv_obj_set_style_bg_opa(screen_win, LV_OPA_COVER, 0);
	lv_obj_add_flag(screen_win, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(screen_win, LV_OBJ_FLAG_SCROLLABLE);

	l = lv_label_create(screen_win);
	lv_obj_set_style_text_font(l, f_head, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
	lv_label_set_text(l, a->name);
	lv_obj_set_pos(l, S(24), S(20));

	b = lv_button_create(screen_win);
	lv_obj_set_size(b, S(140), S(48));
	lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -S(16), S(8));
	lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD_HI), 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_add_event_cb(b, screen_close, LV_EVENT_CLICKED, NULL);
	l = lv_label_create(b);
	lv_obj_set_style_text_font(l, f_body, 0);
	lv_label_set_text(l, tr("Close"));
	lv_obj_center(l);

	screen_area = plain_box(screen_win);
	lv_obj_set_size(screen_area, SCREEN_W - S(32), SCREEN_H - S(80));
	lv_obj_set_pos(screen_area, S(16), S(68));
	draw_screen();
}

/* drawn again from apps_poll, not here: this comes in the middle of the
   layout, where the card's new size is not known to its contents yet.
   Only when it really got another height: drawing it sends this too. */
static void card_resized(lv_event_t *ev)
{
	struct card *c = lv_event_get_user_data(ev);

	if (!c->drawing && lv_obj_get_height(c->root) != c->h)
		c->resized = true;
}

static void on_card_tap(lv_event_t *ev)
{
	struct card *c = lv_event_get_user_data(ev);

	screen_open(c->a, c->cfg);
}

static struct app *find_app(const char *id)
{
	for (int i = 0; id && i < napps; i++)
		if (!strcmp(apps[i].id, id))
			return &apps[i];
	return NULL;
}

lv_obj_t *apps_card(lv_obj_t *parent, cJSON *cfg, int w)
{
	cJSON *app = cJSON_GetObjectItemCaseSensitive(cfg, "app");
	cJSON *hj = cJSON_GetObjectItemCaseSensitive(cfg, "height");
	struct app *a = find_app(cJSON_IsString(app) ? app->valuestring : NULL);
	struct card *c;
	lv_obj_t *root;
	int h = S(150);

	root = lv_obj_create(parent);
	lv_obj_remove_style_all(root);
	lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_color(root, lv_color_hex(C_CARD), 0);
	lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(root, S(14), 0);
	lv_obj_set_style_pad_all(root, S(12), 0);
	if (a) {
		cJSON *t = cJSON_GetObjectItemCaseSensitive(a->manifest, "tile");
		cJSON *th = cJSON_GetObjectItemCaseSensitive(t, "height");

		if (cJSON_IsNumber(th))
			h = S(th->valueint);
	}
	if (cJSON_IsNumber(hj))
		h = S(hj->valueint);
	lv_obj_set_size(root, w, LV_CLAMP(S(60), h, S(520)));
	if (!a) {
		lv_obj_t *l = lv_label_create(root);

		lv_obj_set_style_text_font(l, f_small, 0);
		lv_obj_set_style_text_color(l, lv_color_hex(C_MUTED), 0);
		lv_label_set_text_fmt(l, tr("App \"%s\" is not installed"),
				      cJSON_IsString(app) ? app->valuestring : "?");
		return root;
	}
	c = calloc(1, sizeof(*c));
	c->a = a;
	c->root = root;
	c->cfg = cfg;
	c->area = plain_box(root);
	lv_obj_set_size(c->area, lv_pct(100), lv_pct(100));
	c->next = cards;
	cards = c;
	lv_obj_add_event_cb(root, card_delete, LV_EVENT_DELETE, c);
	/* the page can make the card taller to line up with the cards next to it */
	lv_obj_add_event_cb(root, card_resized, LV_EVENT_SIZE_CHANGED, c);
	if (JS_IsFunction(a->ctx, a->screen_fn)) {
		lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_style_bg_color(root, lv_color_hex(C_CARD_HI), LV_STATE_PRESSED);
		lv_obj_add_event_cb(root, on_card_tap, LV_EVENT_CLICKED, c);
	}
	draw_card(c);
	return root;
}

static void update_app(struct app *a)
{
	for (struct card *c = cards; c; c = c->next)
		if (c->a == a)
			draw_card(c);
	if (screen_app == a)
		draw_screen();
}

/* ---- timers ---------------------------------------------------------------- */

struct timer {
	struct app *a;
	int id;
	uint32_t due, every;
	JSValue fn;
	struct timer *next;
};

static struct timer *timers;
static int next_timer = 1;

static JSValue add_timer(JSContext *ctx, int argc, JSValueConst *argv, bool repeat)
{
	struct timer *t;
	int32_t ms = 0;

	if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
		return JS_ThrowTypeError(ctx, "a function is needed");
	if (argc > 1)
		JS_ToInt32(ctx, &ms, argv[1]);
	if (repeat && ms < 100)
		ms = 100;
	t = calloc(1, sizeof(*t));
	t->a = app_of(ctx);
	t->id = next_timer++;
	t->due = now_ms() + (ms > 0 ? ms : 0);
	t->every = repeat ? ms : 0;
	t->fn = JS_DupValue(ctx, argv[0]);
	t->next = timers;
	timers = t;
	return JS_NewInt32(ctx, t->id);
}

static JSValue js_set_timeout(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
{
	return add_timer(ctx, argc, argv, false);
}

static JSValue js_set_interval(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
{
	return add_timer(ctx, argc, argv, true);
}

static JSValue js_clear_timer(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
{
	int32_t id = 0;

	if (argc > 0)
		JS_ToInt32(ctx, &id, argv[0]);
	for (struct timer *t = timers; t; t = t->next)
		if (t->id == id && t->a == app_of(ctx))
			t->id = 0;		/* removed by timers_poll */
	return JS_UNDEFINED;
}

static void timers_poll(void)
{
	uint32_t now = now_ms();
	struct timer **pp = &timers, *t;

	/* first the due ones; a timer added meanwhile waits for the next round */
	for (t = timers; t; t = t->next)
		if (t->id && (int32_t)(now - t->due) >= 0) {
			if (t->every)
				t->due = now + t->every;
			else
				t->id = 0;
			call(t->a, t->fn, 0, NULL, "timer");
		}
	while ((t = *pp)) {
		if (!t->id) {
			*pp = t->next;
			JS_FreeValue(t->a->ctx, t->fn);
			free(t);
		} else {
			pp = &t->next;
		}
	}
}

/* ---- fetch ----------------------------------------------------------------- */

struct pic;

struct request {
	struct app *a;
	struct pic *pic;	/* a picture for el.image, not a fetch */
	CURL *h;
	struct curl_slist *headers;
	JSValue resolve, reject;
	char *buf, *body, *user, *password;
	size_t len;
	struct request *next;
};

static CURLM *multi;
static struct request *requests;

static size_t on_data(char *p, size_t sz, size_t n, void *user)
{
	struct request *r = user;
	size_t add = sz * n;

	if (r->len + add > MAX_BODY)
		return 0;
	r->buf = realloc(r->buf, r->len + add + 1);
	memcpy(r->buf + r->len, p, add);
	r->len += add;
	r->buf[r->len] = 0;
	return add;
}

static char *opt_str(JSContext *ctx, JSValueConst o, const char *key)
{
	JSValue v = JS_IsObject(o) ? JS_GetPropertyStr(ctx, o, key) : JS_UNDEFINED;
	char *out = NULL;

	if (JS_IsString(v)) {
		const char *s = JS_ToCString(ctx, v);

		out = s ? strdup(s) : NULL;
		JS_FreeCString(ctx, s);
	}
	JS_FreeValue(ctx, v);
	return out;
}

static JSValue js_fetch(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
{
	JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
	JSValue funcs[2], promise, hv;
	struct request *r;
	const char *url;
	char *method;
	double timeout = 30;

	if (argc < 1 || !JS_IsString(argv[0]))
		return JS_ThrowTypeError(ctx, "fetch needs a URL");
	promise = JS_NewPromiseCapability(ctx, funcs);
	r = calloc(1, sizeof(*r));
	r->a = app_of(ctx);
	r->resolve = funcs[0];
	r->reject = funcs[1];
	r->h = curl_easy_init();
	url = JS_ToCString(ctx, argv[0]);
	curl_easy_setopt(r->h, CURLOPT_URL, url);
	JS_FreeCString(ctx, url);
	curl_easy_setopt(r->h, CURLOPT_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(r->h, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(r->h, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(r->h, CURLOPT_CAINFO, CA_FILE);
	curl_easy_setopt(r->h, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(r->h, CURLOPT_USERAGENT, "ReToon-app");
	curl_easy_setopt(r->h, CURLOPT_WRITEFUNCTION, on_data);
	curl_easy_setopt(r->h, CURLOPT_WRITEDATA, r);
	curl_easy_setopt(r->h, CURLOPT_NOSIGNAL, 1L);
	opt_num(ctx, opts, "timeout", &timeout);
	curl_easy_setopt(r->h, CURLOPT_TIMEOUT_MS, (long)(LV_CLAMP(1, timeout, 120) * 1000));
	method = opt_str(ctx, opts, "method");
	if (method)
		curl_easy_setopt(r->h, CURLOPT_CUSTOMREQUEST, method);
	free(method);
	r->body = opt_str(ctx, opts, "body");
	if (r->body)
		curl_easy_setopt(r->h, CURLOPT_POSTFIELDS, r->body);
	/* a user and password for HTTP authentication; curl answers the
	   server's challenge itself */
	r->user = opt_str(ctx, opts, "user");
	r->password = opt_str(ctx, opts, "password");
	if (r->user) {
		char *auth = opt_str(ctx, opts, "auth");
		long how = CURLAUTH_ANY;

		if (auth && !strcmp(auth, "basic"))
			how = CURLAUTH_BASIC;
		else if (auth && !strcmp(auth, "digest"))
			how = CURLAUTH_DIGEST;
		free(auth);
		curl_easy_setopt(r->h, CURLOPT_HTTPAUTH, how);
		curl_easy_setopt(r->h, CURLOPT_USERNAME, r->user);
		curl_easy_setopt(r->h, CURLOPT_PASSWORD, r->password ? r->password : "");
	}
	hv = JS_IsObject(opts) ? JS_GetPropertyStr(ctx, opts, "headers") : JS_UNDEFINED;
	if (JS_IsObject(hv)) {
		JSPropertyEnum *props;
		uint32_t n;

		if (!JS_GetOwnPropertyNames(ctx, &props, &n, hv, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)) {
			for (uint32_t i = 0; i < n; i++) {
				const char *k = JS_AtomToCString(ctx, props[i].atom);
				char *val = opt_str(ctx, hv, k), line[512];

				snprintf(line, sizeof(line), "%s: %s", k, val ? val : "");
				r->headers = curl_slist_append(r->headers, line);
				free(val);
				JS_FreeCString(ctx, k);
				JS_FreeAtom(ctx, props[i].atom);
			}
			js_free(ctx, props);
		}
		curl_easy_setopt(r->h, CURLOPT_HTTPHEADER, r->headers);
	}
	JS_FreeValue(ctx, hv);
	curl_easy_setopt(r->h, CURLOPT_PRIVATE, r);
	r->next = requests;
	requests = r;
	curl_multi_add_handle(multi, r->h);
	return promise;
}

/* the headers of the last answer (after redirects and authentication), by
   name in lower case; a repeated header keeps its last value */
static JSValue answer_headers(JSContext *ctx, CURL *h)
{
	JSValue o = JS_NewObject(ctx);
	struct curl_header *hd, *prev = NULL;

	while ((hd = curl_easy_nextheader(h, CURLH_HEADER, -1, prev))) {
		char name[128];
		size_t i;

		for (i = 0; hd->name[i] && i < sizeof(name) - 1; i++)
			name[i] = tolower((unsigned char)hd->name[i]);
		name[i] = 0;
		JS_SetPropertyStr(ctx, o, name, JS_NewString(ctx, hd->value ? hd->value : ""));
		prev = hd;
	}
	return o;
}

static void pic_got(struct pic *p, struct request *r, CURLcode res);

static void request_free(struct request *r)
{
	struct request **pp;

	for (pp = &requests; *pp; pp = &(*pp)->next)
		if (*pp == r) {
			*pp = r->next;
			break;
		}
	curl_multi_remove_handle(multi, r->h);
	curl_easy_cleanup(r->h);
	curl_slist_free_all(r->headers);
	free(r->buf);
	free(r->body);
	free(r->user);
	free(r->password);
	free(r);
}

static void request_done(struct request *r, CURLcode res)
{
	struct app *a = r->a;
	JSContext *ctx;
	JSValue v;
	long status = 0;

	if (r->pic) {
		pic_got(r->pic, r, res);
		request_free(r);
		return;
	}
	ctx = a->ctx;
	if (res == CURLE_OK) {
		curl_easy_getinfo(r->h, CURLINFO_RESPONSE_CODE, &status);
		v = JS_NewObject(ctx);
		JS_SetPropertyStr(ctx, v, "status", JS_NewInt32(ctx, status));
		JS_SetPropertyStr(ctx, v, "text", JS_NewStringLen(ctx, r->buf ? r->buf : "", r->len));
		JS_SetPropertyStr(ctx, v, "headers", answer_headers(ctx, r->h));
		call(a, r->resolve, 1, (JSValueConst *)&v, "fetch");
	} else {
		v = JS_NewError(ctx);
		JS_SetPropertyStr(ctx, v, "message", JS_NewString(ctx, curl_easy_strerror(res)));
		call(a, r->reject, 1, (JSValueConst *)&v, "fetch");
	}
	JS_FreeValue(ctx, v);
	JS_FreeValue(ctx, r->resolve);
	JS_FreeValue(ctx, r->reject);
	request_free(r);
}

static void fetch_poll(void)
{
	CURLMsg *m;
	int running, left;

	if (!requests)
		return;
	curl_multi_perform(multi, &running);
	while ((m = curl_multi_info_read(multi, &left))) {
		struct request *r = NULL;

		if (m->msg != CURLMSG_DONE)
			continue;
		curl_easy_getinfo(m->easy_handle, CURLINFO_PRIVATE, (char **)&r);
		if (r)
			request_done(r, m->data.result);
	}
}

/* ---- pictures: el.image -------------------------------------------------------- */

/*
 * A JPEG or PNG from a URL (a camera snapshot, for example), decoded once at
 * the size it is shown and kept per URL and size, so drawing the card again
 * shows it at once. "every" fetches a new one that often while it is on the
 * screen. Decoding runs in a thread of its own: a big picture does not stop
 * the screen.
 */
#define PIC_KEEP_MS	60000		/* a picture no longer shown is kept this long */
#define PIC_MAX_PIXELS	(4096 * 4096)	/* larger ones are not decoded */
#define PIC_RETRY_MS	5000		/* a camera or stream again after a failure */
#define STREAM_MAX	(3 << 20)	/* a stream's frame larger than this is dropped */

struct pic_use {
	lv_obj_t *o;
	lv_obj_t *note;			/* a popup's "no picture", shown after a failure */
	struct pic *p;
	struct pic_use *next;
};

struct pic {
	char *url, *user, *password, *auth;
	struct curl_slist *headers;	/* the app's, copied */
	int w, h;			/* decoded to fit in this (screen pixels) */
	int every;			/* seconds between new ones; 0: once */
	char *camera;			/* a Home Assistant camera, not a URL */
	bool stream;			/* MJPEG: frames as they come */
	int fps;			/* a stream's frames a second, at most */
	struct request *req;		/* a stream's open request */
	uint32_t frame;			/* when the last frame went to decoding */
	uint32_t opened;		/* when the stream was opened */
	bool failed;			/* the last try failed: again after PIC_RETRY_MS */
	lv_draw_buf_t *buf;
	struct gif_frames anim;		/* an animated GIF's frames (n > 1) */
	int cur;			/* the frame shown */
	uint32_t next_at;		/* when the next one is due */
	uint32_t got, unused;
	bool loading, decoding;
	bool brief;			/* a popup's: freed as soon as it is not shown */
	struct pic_use *uses;
	/* from the decoding thread (under pic_lock) */
	uint8_t *px;
	int pw, ph;
	struct gif_frames anim_new;	/* or an animation */
	char err[80];
	struct pic *next;
};

static struct pic *pics;
static pthread_mutex_t pic_lock = PTHREAD_MUTEX_INITIALIZER;

struct pic_job {
	struct pic *p;
	uint8_t *data;
	size_t len;
	int sw, sh, dw, dh;
	uint8_t *out;			/* ARGB8888: B, G, R, A */
	struct gif_frames anim;		/* or this, for an animated GIF */
};

/* the size it is shown at: the whole picture fits, never larger than it is */
static void pic_fit(struct pic_job *j)
{
	double f = LV_MIN((double)j->p->w / j->sw, (double)j->p->h / j->sh);

	if (f > 1)
		f = 1;
	j->dw = LV_MAX(1, (int)(j->sw * f + 0.5));
	j->dh = LV_MAX(1, (int)(j->sh * f + 0.5));
}

/* libjpeg's errors: back to jpg_decode instead of exit() */
struct jpg_err {
	struct jpeg_error_mgr mgr;
	jmp_buf jb;
};

static void jpg_err_exit(j_common_ptr c)
{
	longjmp(((struct jpg_err *)c->err)->jb, 1);
}

static void jpg_err_quiet(j_common_ptr c)
{
	(void)c;
}

/*
 * A JPEG with libjpeg-turbo (NEON on the Toon 2), progressive ones too. It
 * decodes at 1/2, 1/4 or 1/8 of the size when the picture is shown that much
 * smaller (a large camera frame costs a fraction of the work), straight into
 * B, G, R, A; then each row is thinned to the size it is shown at.
 */
static const char *jpg_decode(struct pic_job *j)
{
	struct jpeg_decompress_struct ci;
	struct jpg_err je;
	uint8_t *volatile row = NULL;
	const char *volatile err = "not a JPEG it can read";
	unsigned denom = 1;

	ci.err = jpeg_std_error(&je.mgr);
	je.mgr.error_exit = jpg_err_exit;
	je.mgr.output_message = jpg_err_quiet;
	if (setjmp(je.jb)) {
		jpeg_destroy_decompress(&ci);
		free(row);
		free(j->out);
		j->out = NULL;
		return err;
	}
	jpeg_create_decompress(&ci);
	jpeg_mem_src(&ci, j->data, j->len);
	jpeg_read_header(&ci, TRUE);
	if ((size_t)ci.image_width * ci.image_height > PIC_MAX_PIXELS) {
		err = "too large";
		longjmp(je.jb, 1);
	}
	j->sw = ci.image_width;
	j->sh = ci.image_height;
	pic_fit(j);
	while (denom < 8 && ci.image_width / (denom * 2) >= (unsigned)j->dw &&
	       ci.image_height / (denom * 2) >= (unsigned)j->dh)
		denom *= 2;
	ci.scale_num = 1;
	ci.scale_denom = denom;
	ci.out_color_space = JCS_EXT_BGRA;	/* alpha comes out as 0xff */
	ci.dct_method = JDCT_IFAST;
	ci.do_fancy_upsampling = FALSE;
	err = "the JPEG is damaged";
	jpeg_start_decompress(&ci);
	j->sw = ci.output_width;
	j->sh = ci.output_height;
	pic_fit(j);
	j->out = calloc((size_t)j->dw * j->dh, 4);
	row = malloc((size_t)ci.output_width * 4);
	if (!j->out || !row) {
		err = "out of memory";
		longjmp(je.jb, 1);
	}
	while (ci.output_scanline < ci.output_height) {
		int sy = ci.output_scanline, y0, y1;
		JSAMPROW r = row;

		jpeg_read_scanlines(&ci, &r, 1);
		/* the rows of the smaller picture that take this one */
		y0 = (sy * j->dh + j->sh - 1) / j->sh;
		y1 = ((sy + 1) * j->dh - 1) / j->sh;
		for (int dy = y0; dy <= y1 && dy < j->dh; dy++) {
			uint32_t *d = (uint32_t *)(j->out + (size_t)dy * j->dw * 4);

			if (j->dw == j->sw)
				memcpy(d, row, (size_t)j->dw * 4);
			else
				for (int dx = 0; dx < j->dw; dx++)
					memcpy(d + dx, row + (size_t)(dx * j->sw / j->dw) * 4, 4);
		}
	}
	jpeg_finish_decompress(&ci);
	jpeg_destroy_decompress(&ci);
	free(row);
	return NULL;
}

static const char *pic_decode(struct pic_job *j)
{
	static const uint8_t png_sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };

	if (j->len > 3 && j->data[0] == 0xff && j->data[1] == 0xd8)
		return jpg_decode(j);
	if (j->len > 8 && !memcmp(j->data, png_sig, 8)) {
		/* LVGL's lodepng gives a draw buffer of R, G, B, A */
		lv_draw_buf_t *png = NULL;
		const uint8_t *rgba;
		unsigned w, h;
		uint32_t stride;

		if (lodepng_decode32((unsigned char **)&png, &w, &h, j->data, j->len) || !png ||
		    (size_t)w * h > PIC_MAX_PIXELS) {
			if (png)
				lv_draw_buf_destroy(png);
			return "not a PNG it can read";
		}
		rgba = png->data;
		stride = png->header.stride ? png->header.stride : w * 4;
		j->sw = w;
		j->sh = h;
		pic_fit(j);
		j->out = calloc((size_t)j->dw * j->dh, 4);
		for (int dy = 0; j->out && dy < j->dh; dy++)
			for (int dx = 0; dx < j->dw; dx++) {
				const uint8_t *q = rgba + (size_t)(dy * j->sh / j->dh) * stride +
						   (size_t)(dx * j->sw / j->dw) * 4;
				uint8_t *d = j->out + (dy * j->dw + dx) * 4;

				d[0] = q[2];
				d[1] = q[1];
				d[2] = q[0];
				d[3] = q[3];
			}
		lv_draw_buf_destroy(png);
		return j->out ? NULL : "out of memory";
	}
	if (j->len > 6 && !memcmp(j->data, "GIF8", 4)) {
		struct gif_frames g;
		const char *err = gif_decode(j->data, j->len, j->p->w, j->p->h, PIC_MAX_PIXELS, &g);

		if (err)
			return err;
		/* one frame (or a stream's): a still picture like the others */
		if (g.n == 1 || j->p->stream) {
			j->out = g.px[0];
			g.px[0] = NULL;
			j->dw = g.w;
			j->dh = g.h;
			gif_free(&g);
		} else {
			j->anim = g;
		}
		return NULL;
	}
	return "not a JPEG, PNG or GIF";
}

static void *pic_thread(void *arg)
{
	struct pic_job *j = arg;
	const char *err = pic_decode(j);

	pthread_mutex_lock(&pic_lock);
	if (err) {
		free(j->out);
		snprintf(j->p->err, sizeof(j->p->err), "%s", err);
	} else if (j->anim.n) {
		gif_free(&j->p->anim_new);	/* one not shown yet: this one instead */
		j->p->anim_new = j->anim;
	} else {
		j->p->px = j->out;
		j->p->pw = j->dw;
		j->p->ph = j->dh;
	}
	j->p->decoding = false;
	pthread_mutex_unlock(&pic_lock);
	free(j->data);
	free(j);
	return NULL;
}

/* decode data (taken over: freed) in a thread of its own */
static void pic_decode_start(struct pic *p, uint8_t *data, size_t len)
{
	struct pic_job *j = calloc(1, sizeof(*j));
	pthread_t t;

	j->p = p;
	j->data = data;
	j->len = len;
	p->decoding = true;
	if (pthread_create(&t, NULL, pic_thread, j)) {
		p->decoding = false;
		free(j->data);
		free(j);
		return;
	}
	pthread_detach(t);
}

/* "content-length:" in a stream part's headers, or -1 */
static long part_length(const char *h, size_t n)
{
	static const char key[] = "content-length:";

	for (size_t i = 0; i + sizeof(key) - 1 <= n; i++) {
		size_t k = 0;

		while (k < sizeof(key) - 1 && tolower((unsigned char)h[i + k]) == key[k])
			k++;
		if (k == sizeof(key) - 1) {
			long v = 0;

			for (i += k; i < n && h[i] == ' '; i++)
				;
			if (i >= n || !isdigit((unsigned char)h[i]))
				return -1;
			for (; i < n && isdigit((unsigned char)h[i]); i++)
				v = v * 10 + (h[i] - '0');
			return v;
		}
	}
	return -1;
}

static void *find2(const char *buf, size_t n, uint8_t a, uint8_t b)
{
	for (size_t i = 0; i + 1 < n; i++)
		if ((uint8_t)buf[i] == a && (uint8_t)buf[i + 1] == b)
			return (void *)(buf + i);
	return NULL;
}

/* a frame of a stream to decoding, when the one before it is done and the
   fps allows it; the others are dropped, so the picture never runs behind */
static void stream_frame(struct pic *p, const char *data, size_t len)
{
	bool busy;
	uint8_t *f;

	pthread_mutex_lock(&pic_lock);
	busy = p->decoding;
	pthread_mutex_unlock(&pic_lock);
	/* 3/4 of the interval: a camera at the same rate, a little late or
	   early, still gets each frame through */
	if (busy || now_ms() - p->frame < 750u / p->fps || !(f = malloc(len)))
		return;
	memcpy(f, data, len);
	p->frame = now_ms();
	p->failed = false;
	pic_decode_start(p, f, len);
}

/*
 * MJPEG (multipart/x-mixed-replace): parts of headers and a picture (JPEG,
 * or PNG as Home Assistant sends for some cameras). A part's Content-Length
 * says where it ends; without one, a JPEG ends at its end marker.
 */
static size_t on_stream(char *ptr, size_t sz, size_t n, void *user)
{
	struct request *r = user;
	size_t add = sz * n;

	if (r->len + add > STREAM_MAX)
		r->len = 0;		/* no frame in all that: start again */
	r->buf = realloc(r->buf, r->len + add + 1);
	if (!r->buf) {
		r->len = 0;
		return 0;
	}
	memcpy(r->buf + r->len, ptr, add);
	r->len += add;
	for (;;) {
		char *hend = memmem(r->buf, r->len, "\r\n\r\n", 4), *soi, *eoi;
		size_t body, start, end;
		long clen;

		if (!hend) {
			/* no whole headers yet; a stream of bare JPEGs has none */
			soi = r->len > 65536 ? find2(r->buf, r->len, 0xff, 0xd8) : NULL;
			if (r->len > 65536 && !soi) {
				memmove(r->buf, r->buf + r->len - 3, 3);
				r->len = 3;
			}
			if (!soi)
				break;
			body = soi - r->buf;
			clen = -1;
		} else {
			body = hend + 4 - r->buf;
			clen = part_length(r->buf, hend - r->buf);
		}
		if (clen >= 0) {
			if (r->len - body < (size_t)clen)
				break;
			start = body;
			end = body + clen;
		} else {
			if (!(soi = find2(r->buf + body, r->len - body, 0xff, 0xd8)))
				break;
			if (!(eoi = find2(soi + 2, r->len - (soi + 2 - r->buf), 0xff, 0xd9)))
				break;
			start = soi - r->buf;
			end = eoi + 2 - r->buf;
		}
		if (end > start)
			stream_frame(r->pic, r->buf + start, end - start);
		memmove(r->buf, r->buf + end, r->len - end);
		r->len -= end;
	}
	return add;
}

/* get a picture (or open a stream) at url; resolve and check for Home
   Assistant's own address (camera) */
static void pic_get(struct pic *p, const char *url, const char *resolve, bool check)
{
	struct request *r = calloc(1, sizeof(*r));

	r->pic = p;
	r->h = curl_easy_init();
	curl_easy_setopt(r->h, CURLOPT_URL, url);
	if (resolve) {
		r->headers = curl_slist_append(NULL, resolve);
		curl_easy_setopt(r->h, CURLOPT_RESOLVE, r->headers);
		curl_easy_setopt(r->h, CURLOPT_SSL_VERIFYPEER, check ? 1L : 0L);
		curl_easy_setopt(r->h, CURLOPT_SSL_VERIFYHOST, check ? 2L : 0L);
	}
	curl_easy_setopt(r->h, CURLOPT_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(r->h, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(r->h, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(r->h, CURLOPT_CAINFO, CA_FILE);
	curl_easy_setopt(r->h, CURLOPT_USERAGENT, "ReToon-app");
	curl_easy_setopt(r->h, CURLOPT_WRITEFUNCTION, p->stream ? on_stream : on_data);
	curl_easy_setopt(r->h, CURLOPT_WRITEDATA, r);
	curl_easy_setopt(r->h, CURLOPT_NOSIGNAL, 1L);
	if (p->stream) {
		/* open as long as it is shown. Home Assistant sends a frame only
		   when the picture changed, so a quiet stream is fine: keepalive
		   finds a dead connection, and one quiet for 5 minutes starts again */
		curl_easy_setopt(r->h, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
		curl_easy_setopt(r->h, CURLOPT_TCP_KEEPALIVE, 1L);
		curl_easy_setopt(r->h, CURLOPT_TCP_KEEPIDLE, 30L);
		curl_easy_setopt(r->h, CURLOPT_TCP_KEEPINTVL, 10L);
		curl_easy_setopt(r->h, CURLOPT_LOW_SPEED_LIMIT, 1L);
		curl_easy_setopt(r->h, CURLOPT_LOW_SPEED_TIME, 300L);
		/* more of it per round of the screen's loop than curl's 16 kB:
		   a frame of a camera is often larger */
		curl_easy_setopt(r->h, CURLOPT_BUFFERSIZE, 256L * 1024);
		p->req = r;
		p->opened = now_ms();
	} else {
		curl_easy_setopt(r->h, CURLOPT_TIMEOUT_MS, 20000L);
	}
	if (p->user) {
		long how = !p->auth ? CURLAUTH_ANY : !strcmp(p->auth, "basic") ? CURLAUTH_BASIC :
			   !strcmp(p->auth, "digest") ? CURLAUTH_DIGEST : CURLAUTH_ANY;

		curl_easy_setopt(r->h, CURLOPT_HTTPAUTH, how);
		curl_easy_setopt(r->h, CURLOPT_USERNAME, p->user);
		curl_easy_setopt(r->h, CURLOPT_PASSWORD, p->password ? p->password : "");
	}
	if (p->headers)
		curl_easy_setopt(r->h, CURLOPT_HTTPHEADER, p->headers);
	curl_easy_setopt(r->h, CURLOPT_PRIVATE, r);
	r->next = requests;
	requests = r;
	curl_multi_add_handle(multi, r->h);
	p->loading = true;
}

static void pic_fail(struct pic *p, const char *why)
{
	p->loading = false;
	p->failed = true;
	p->got = now_ms();
	snprintf(p->err, sizeof(p->err), "%s", why);
}

/* Home Assistant signed the camera's path: get it with that signature */
static void on_signed(cJSON *result, void *user)
{
	struct pic *p = user;
	cJSON *path = cJSON_GetObjectItemCaseSensitive(result, "path");
	char base[200], resolve[200], url[1024];
	bool check;

	p->loading = false;
	if (!cJSON_IsString(path)) {
		pic_fail(p, result ? "no signed path from Home Assistant" : ha_error());
		return;
	}
	if (!ha_http(base, sizeof(base), resolve, sizeof(resolve), &check)) {
		pic_fail(p, "not connected to Home Assistant");
		return;
	}
	snprintf(url, sizeof(url), "%s%s", base, path->valuestring);
	pic_get(p, url, resolve, check);
}

/* a camera: ask Home Assistant for a signed path first (valid 30 s, enough
   to start), so the app needs no token and never sees one */
static void pic_fetch(struct pic *p)
{
	char path[300];
	cJSON *o;

	if (!p->camera) {
		pic_get(p, p->url, NULL, true);
		return;
	}
	if (!ha_ready()) {
		pic_fail(p, "not connected to Home Assistant");
		return;
	}
	if (p->stream)
		snprintf(path, sizeof(path), "/api/camera_proxy_stream/%s", p->camera);
	else
		snprintf(path, sizeof(path), "/api/camera_proxy/%s?width=%d&height=%d", p->camera, p->w, p->h);
	o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "type", "auth/sign_path");
	cJSON_AddStringToObject(o, "path", path);
	cJSON_AddNumberToObject(o, "expires", 30);
	p->loading = true;
	if (!ha_request(o, on_signed, p))
		pic_fail(p, "not connected to Home Assistant");
}

static void pic_got(struct pic *p, struct request *r, CURLcode res)
{
	long status = 0;

	p->loading = false;
	p->got = now_ms();
	curl_easy_getinfo(r->h, CURLINFO_RESPONSE_CODE, &status);
	if (p->stream) {
		/* a stream that ended or broke. Home Assistant ends one every
		   30 seconds: one that ran that long opens again at once, one
		   that broke sooner after PIC_RETRY_MS */
		p->req = NULL;
		p->failed = true;
		if (status == 200 && p->got - p->opened >= 10000) {
			p->got = 0;
			return;	/* not worth a line in the log */
		}
		snprintf(p->err, sizeof(p->err), "stream ended: %s (curl %d, http %ld)",
			 res != CURLE_OK ? curl_easy_strerror(res) : "closed", (int)res, status);
		return;
	}
	if (res != CURLE_OK || status != 200 || !r->len) {
		snprintf(p->err, sizeof(p->err), "%s", res != CURLE_OK ? curl_easy_strerror(res) :
			 status != 200 ? "the server answered with an error" : "empty answer");
		LV_LOG_WARN("picture %s: %s (%ld)", p->url, p->err, status);
		p->failed = true;
		return;
	}
	p->failed = false;
	pic_decode_start(p, (uint8_t *)r->buf, r->len);	/* the thread frees it */
	r->buf = NULL;
}

static void pic_free(struct pic *p)
{
	struct pic **pp;

	for (pp = &pics; *pp; pp = &(*pp)->next)
		if (*pp == p) {
			*pp = p->next;
			break;
		}
	if (p->buf) {
		lv_image_cache_drop(p->buf);
		lv_draw_buf_destroy(p->buf);
	}
	curl_slist_free_all(p->headers);
	free(p->px);
	gif_free(&p->anim);
	gif_free(&p->anim_new);
	free(p->camera);
	free(p->url);
	free(p->user);
	free(p->password);
	free(p->auth);
	free(p);
}

/* is the picture on the screen: a page that is not shown (the pages next
   to it stay made, to slide to) does not count */
static bool pic_shown(const struct pic *p)
{
	for (struct pic_use *u = p->uses; u; u = u->next)
		if (lv_obj_is_visible(u->o))
			return true;
	return false;
}

/* a new picture (pixels B, G, R, A) into a draw buffer of its own */
static void pic_set(struct pic *p, const uint8_t *px, int w, int h)
{
	lv_draw_buf_t *old = p->buf, *b = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_ARGB8888, 0);

	if (!b)
		return;
	for (int y = 0; y < h; y++)
		memcpy(b->data + y * b->header.stride, px + (size_t)y * w * 4, (size_t)w * 4);
	p->buf = b;
	for (struct pic_use *u = p->uses; u; u = u->next)
		lv_image_set_src(u->o, b);
	if (old) {
		lv_image_cache_drop(old);
		lv_draw_buf_destroy(old);
	}
}

/* an animated GIF's next frame, into the same buffer, when it is due */
static void pic_step(struct pic *p, uint32_t now)
{
	const struct gif_frames *g = &p->anim;

	if (g->n < 2 || !p->buf || (int32_t)(now - p->next_at) < 0 ||
	    p->buf->header.w != (uint32_t)g->w || p->buf->header.h != (uint32_t)g->h)
		return;
	p->cur = (p->cur + 1) % g->n;
	for (int y = 0; y < g->h; y++)
		memcpy(p->buf->data + y * p->buf->header.stride, g->px[p->cur] + (size_t)y * g->w * 4,
		       (size_t)g->w * 4);
	lv_image_cache_drop(p->buf);
	for (struct pic_use *u = p->uses; u; u = u->next)
		lv_obj_invalidate(u->o);
	/* on time, but not a burst of frames after a page that was away */
	p->next_at += g->delay[p->cur];
	if ((int32_t)(now - p->next_at) > 1000)
		p->next_at = now + g->delay[p->cur];
}

static void pic_poll(void)
{
	uint32_t now = now_ms();

	for (struct pic *p = pics, *next; p; p = next) {
		uint8_t *px;
		struct gif_frames anim;
		bool decoding, shown;

		next = p->next;
		pthread_mutex_lock(&pic_lock);
		px = p->px;
		p->px = NULL;
		anim = p->anim_new;
		memset(&p->anim_new, 0, sizeof(p->anim_new));
		decoding = p->decoding;
		/* a stream's frame that fails: once, not for every frame */
		if (p->err[0] && !decoding && (!p->loading || p->stream)) {
			static char last[80];

			if (!p->stream || strcmp(last, p->err))
				fprintf(stderr, "picture %s: %s\n", p->url, p->err);
			snprintf(last, sizeof(last), "%s", p->err);
			p->err[0] = 0;
			/* a picture that could not be decoded failed too (a camera
			   that only streams may answer with something else) */
			if (!p->stream)
				p->failed = true;
		}
		pthread_mutex_unlock(&pic_lock);
		if (px) {
			gif_free(&p->anim);	/* a still picture now */
			pic_set(p, px, p->pw, p->ph);
			free(px);
		}
		if (anim.n) {
			gif_free(&p->anim);
			p->anim = anim;
			p->cur = 0;
			p->next_at = now + anim.delay[0];
			pic_set(p, anim.px[0], anim.w, anim.h);
		}
		for (struct pic_use *u = p->uses; u; u = u->next)
			if (u->note && lv_obj_has_flag(u->note, LV_OBJ_FLAG_HIDDEN) == (p->failed && !p->buf)) {
				if (p->failed && !p->buf)
					lv_obj_remove_flag(u->note, LV_OBJ_FLAG_HIDDEN);
				else
					lv_obj_add_flag(u->note, LV_OBJ_FLAG_HIDDEN);
			}
		/* a stream is open only while it is on the screen, and an
		   animation only moves then */
		shown = pic_shown(p);
		if (shown)
			pic_step(p, now);
		if (p->req && !shown) {
			request_free(p->req);
			p->req = NULL;
			p->loading = false;
			p->got = 0;
		}
		if (p->loading || decoding)
			continue;
		if (!p->uses) {
			if (p->brief || now - p->unused > PIC_KEEP_MS)
				pic_free(p);
		} else if (!shown) {
			continue;
		} else if (p->stream || (p->camera && p->failed)) {
			if (!p->got || now - p->got >= PIC_RETRY_MS)
				pic_fetch(p);
		} else if (p->every && now - p->got >= (uint32_t)p->every * 1000) {
			pic_fetch(p);
		}
	}
}

static void on_pic_delete(lv_event_t *ev)
{
	struct pic_use *u = lv_event_get_user_data(ev), **pp;
	struct pic *p = u->p;

	for (pp = &p->uses; *pp; pp = &(*pp)->next)
		if (*pp == u) {
			*pp = u->next;
			break;
		}
	if (!p->uses)
		p->unused = now_ms();
	free(u);
}

static bool same(const char *a, const char *b)
{
	return a == b || (a && b && !strcmp(a, b));
}

/* may the app show this camera: listed in its manifest's "cameras" as
   "camera.door", or "camera.*" for any */
static bool camera_allowed(struct app *a, const char *id)
{
	cJSON *v;

	cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(a->manifest, "cameras"))
		if (cJSON_IsString(v) && (!strcmp(v->valuestring, id) || !strcmp(v->valuestring, "camera.*")))
			return true;
	return false;
}

static bool opt_bool(JSContext *ctx, JSValueConst o, const char *key)
{
	JSValue v = JS_IsObject(o) ? JS_GetPropertyStr(ctx, o, key) : JS_UNDEFINED;
	bool b = JS_ToBool(ctx, v) > 0;

	JS_FreeValue(ctx, v);
	return b;
}

/* el.image({url | camera, stream, fps, every, user, password, auth, headers,
   x, y, w, h, align, radius, ...}) */
static JSValue js_image(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
{
	lv_obj_t *parent = el_obj(ctx, this), *o;
	JSValueConst opts = argc > 0 ? argv[0] : JS_UNDEFINED;
	char *url = opt_str(ctx, opts, "url"), *camera = opt_str(ctx, opts, "camera"), *user, *password, *auth;
	bool stream = opt_bool(ctx, opts, "stream");
	struct pic *p;
	struct pic_use *u;
	double every = 0, fps = 5;
	int w, h;
	JSValue hv;

	if (!parent) {
		free(url);
		free(camera);
		return JS_UNDEFINED;
	}
	if (camera) {
		bool ok = !strncmp(camera, "camera.", 7) && strlen(camera) < 128 && !strpbrk(camera, "/?&# ");

		if (!ok || !camera_allowed(app_of(ctx), camera)) {
			JSValue e = ok ? JS_ThrowTypeError(ctx, "%s is not in the manifest's \"cameras\"", camera) :
				    JS_ThrowTypeError(ctx, "camera needs a camera entity id");

			free(url);
			free(camera);
			return e;
		}
		/* kept by the camera's id (for the log too) */
		free(url);
		url = strdup(camera);
	} else if (!url || (strncmp(url, "http://", 7) && strncmp(url, "https://", 8))) {
		free(url);
		return JS_ThrowTypeError(ctx, "image needs an http or https url, or a camera");
	}
	o = lv_image_create(parent);
	lv_obj_set_size(o, S(320), S(240));
	lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_image_set_inner_align(o, LV_IMAGE_ALIGN_CENTER);
	lv_obj_set_style_clip_corner(o, true, 0);
	apply_opts(ctx, o, opts);
	w = lv_obj_get_style_width(o, 0);
	h = lv_obj_get_style_height(o, 0);
	opt_num(ctx, opts, "every", &every);
	opt_num(ctx, opts, "fps", &fps);
	user = opt_str(ctx, opts, "user");
	password = opt_str(ctx, opts, "password");
	auth = opt_str(ctx, opts, "auth");

	for (p = pics; p; p = p->next)
		if (!strcmp(p->url, url) && p->w == w && p->h == h && same(p->user, user) &&
		    same(p->password, password) && p->stream == stream && !p->camera == !camera)
			break;
	if (p) {
		free(url);
		free(camera);
		free(user);
		free(password);
		free(auth);
	} else {
		p = calloc(1, sizeof(*p));
		p->url = url;
		p->camera = camera;
		p->stream = stream;
		p->user = user;
		p->password = password;
		p->auth = auth;
		p->w = w;
		p->h = h;
		hv = JS_IsObject(opts) ? JS_GetPropertyStr(ctx, opts, "headers") : JS_UNDEFINED;
		if (JS_IsObject(hv)) {
			JSPropertyEnum *props;
			uint32_t n;

			if (!JS_GetOwnPropertyNames(ctx, &props, &n, hv, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)) {
				for (uint32_t i = 0; i < n; i++) {
					const char *k = JS_AtomToCString(ctx, props[i].atom);
					char *val = opt_str(ctx, hv, k), line[512];

					snprintf(line, sizeof(line), "%s: %s", k, val ? val : "");
					p->headers = curl_slist_append(p->headers, line);
					free(val);
					JS_FreeCString(ctx, k);
					JS_FreeAtom(ctx, props[i].atom);
				}
				js_free(ctx, props);
			}
		}
		JS_FreeValue(ctx, hv);
		p->next = pics;
		pics = p;
	}
	p->every = stream || every <= 0 ? 0 : every < 2 ? 2 : (int)every;
	p->fps = fps < 1 ? 1 : fps > 10 ? 10 : (int)fps;
	u = calloc(1, sizeof(*u));
	u->o = o;
	u->p = p;
	u->next = p->uses;
	p->uses = u;
	lv_obj_add_event_cb(o, on_pic_delete, LV_EVENT_DELETE, u);
	if (p->buf)
		lv_image_set_src(o, p->buf);
	else if (!p->stream && !p->loading && !p->decoding && (!p->got || now_ms() - p->got > 5000))
		pic_fetch(p);	/* a stream: pic_poll opens it once it is on the screen */
	return el_new(ctx, o);
}

/*
 * A Home Assistant camera's snapshot (a message's picture, notify.c), fitted
 * into w x h: fetched once, fresh, over the Toon's own connection (scaled
 * down by Home Assistant to that size), and freed as soon as the object is
 * gone. NULL for an id that is not a camera's.
 */
lv_obj_t *apps_camera_picture(lv_obj_t *parent, const char *camera, int w, int h)
{
	struct pic *p;
	struct pic_use *u;
	lv_obj_t *o;

	if (!camera || strncmp(camera, "camera.", 7) || strlen(camera) >= 128 ||
	    strpbrk(camera, "/?&# \"\\"))
		return NULL;
	o = lv_image_create(parent);
	lv_obj_set_size(o, w, h);
	lv_image_set_inner_align(o, LV_IMAGE_ALIGN_CENTER);
	for (p = pics; p; p = p->next)
		if (p->camera && !p->stream && !p->user && p->w == w && p->h == h && !strcmp(p->url, camera))
			break;
	if (!p) {
		p = calloc(1, sizeof(*p));
		p->url = strdup(camera);
		p->camera = strdup(camera);
		p->w = w;
		p->h = h;
		p->brief = true;
		p->fps = 1;
		p->next = pics;
		pics = p;
	}
	u = calloc(1, sizeof(*u));
	u->o = o;
	u->p = p;
	u->next = p->uses;
	p->uses = u;
	/* a camera without pictures (some only stream) says so */
	u->note = lv_label_create(o);
	lv_label_set_text(u->note, tr("No picture from this camera"));
	lv_obj_set_style_text_color(u->note, lv_color_hex(C_MUTED), 0);
	lv_obj_set_style_text_font(u->note, f_body, 0);
	lv_obj_center(u->note);
	lv_obj_add_flag(u->note, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb(o, on_pic_delete, LV_EVENT_DELETE, u);
	if (p->buf)
		lv_image_set_src(o, p->buf);	/* the one before, until the new one is in */
	if (!p->loading && !p->decoding)
		pic_fetch(p);
	return o;
}

/* a message from an app (retoon.notify, retoon/notify/set) may only show a
   camera its manifest lists, as el.image */
static bool app_note_ok(struct app *a, const char *json, size_t len)
{
	cJSON *o = cJSON_ParseWithLength(json, len), *c;
	bool ok = true;

	c = cJSON_IsObject(o) ? cJSON_GetObjectItemCaseSensitive(o, "camera") : NULL;
	if (c)
		ok = cJSON_IsString(c) && camera_allowed(a, c->valuestring);
	cJSON_Delete(o);
	return ok;
}

/* ---- the retoon object ------------------------------------------------------ */

static JSValue js_log(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
{
	fprintf(stderr, "app %s:", app_of(ctx)->id);
	for (int i = 0; i < argc; i++) {
		const char *s = JS_ToCString(ctx, argv[i]);

		fprintf(stderr, " %s", s ? s : "?");
		JS_FreeCString(ctx, s);
	}
	fputc('\n', stderr);
	return JS_UNDEFINED;
}

static JSValue set_fn(JSContext *ctx, JSValue *slot, int argc, JSValueConst *argv)
{
	if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
		return JS_ThrowTypeError(ctx, "a function is needed");
	JS_FreeValue(ctx, *slot);
	*slot = JS_DupValue(ctx, argv[0]);
	return JS_UNDEFINED;
}

static JSValue js_tile(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
{
	return set_fn(ctx, &app_of(ctx)->tile_fn, argc, argv);
}

static JSValue js_screen(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
{
	return set_fn(ctx, &app_of(ctx)->screen_fn, argc, argv);
}

static JSValue js_on_settings(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
{
	return set_fn(ctx, &app_of(ctx)->settings_fn, argc, argv);
}

/* updates are drawn from the main loop, not inside the app's own call */
static bool want_update[MAX_APPS];

static JSValue js_update(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
{
	want_update[app_of(ctx) - apps] = true;
	return JS_UNDEFINED;
}

/* ---- entities ---------------------------------------------------------------- */

/* all apps' entities to the Home Assistant and MQTT links */
static void want_entities(void)
{
	const char **ids = NULL;
	int n = 0;

	for (int i = 0; i < napps; i++)
		for (int j = 0; j < apps[i].nwatch; j++) {
			ids = realloc(ids, (n + 1) * sizeof(*ids));
			ids[n++] = apps[i].watch[j];
		}
	if (ent_want_apps(ids, n)) {
		ha_restart();		/* subscribes again, with the new ones */
		mqtt_restart();
	}
	free(ids);
}

static JSValue js_watch(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
{
	struct app *a = app_of(ctx);
	int64_t len = 0;

	if (argc < 1 || !JS_IsArray(ctx, argv[0]) || array_len(ctx, argv[0], &len))
		return JS_ThrowTypeError(ctx, "watch needs a list of entity ids");
	free(a->watch);
	free(a->seen);
	len = LV_CLAMP(0, len, 64);
	a->watch = len ? calloc(len, sizeof(*a->watch)) : NULL;
	a->seen = len ? malloc(len * sizeof(*a->seen)) : NULL;
	for (int64_t i = 0; i < len; i++)
		a->seen[i] = -1;
	a->nwatch = 0;
	for (int64_t i = 0; i < len; i++) {
		JSValue v = JS_GetPropertyUint32(ctx, argv[0], i);
		const char *id = JS_IsString(v) ? JS_ToCString(ctx, v) : NULL;

		if (id && strchr(id, '.'))
			snprintf(a->watch[a->nwatch++], sizeof(*a->watch), "%s", id);
		JS_FreeCString(ctx, id);
		JS_FreeValue(ctx, v);
	}
	JS_FreeValue(ctx, a->state_fn);
	a->state_fn = argc > 1 && JS_IsFunction(ctx, argv[1]) ? JS_DupValue(ctx, argv[1]) : JS_UNDEFINED;
	want_entities();
	return JS_UNDEFINED;
}

static JSValue js_state(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
{
	const char *id = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
	struct entity *e = id ? ent_find(id) : NULL;
	const char *unit, *name;
	char *attr, *end;
	double num;
	JSValue o;

	JS_FreeCString(ctx, id);
	if (!e || !e->state)
		return JS_NULL;
	o = JS_NewObject(ctx);
	JS_SetPropertyStr(ctx, o, "state", JS_NewString(ctx, e->state));
	num = strtod(e->state, &end);
	JS_SetPropertyStr(ctx, o, "value", JS_NewFloat64(ctx, end != e->state && !*end ? num : NAN));
	unit = ent_str(e, "unit_of_measurement");
	JS_SetPropertyStr(ctx, o, "unit", unit ? JS_NewString(ctx, unit) : JS_NULL);
	name = ent_str(e, "friendly_name");
	JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, name ? name : e->id));
	attr = cJSON_PrintUnformatted(e->attr);
	JS_SetPropertyStr(ctx, o, "attributes", attr ? JS_ParseJSON(ctx, attr, strlen(attr), "<attributes>") : JS_NewObject(ctx));
	free(attr);
	return o;
}

/* tell each app which of its entities changed: state or attributes */
static void watch_poll(void)
{
	for (int i = 0; i < napps; i++) {
		struct app *a = &apps[i];
		char (*list)[96];

		for (int j = 0; j < a->nwatch; j++) {
			struct entity *e = ent_find(a->watch[j]);
			long now = e && e->state ? (long)e->gen : -1;
			JSValue arg;

			/* a new state or attribute (volume, title, temperature of a
			   weather entity...), not only a new state */
			if (now == a->seen[j])
				continue;
			a->seen[j] = now;
			if (!JS_IsFunction(a->ctx, a->state_fn))
				continue;
			arg = JS_NewString(a->ctx, a->watch[j]);
			list = a->watch;
			call(a, a->state_fn, 1, &arg, "state");
			JS_FreeValue(a->ctx, arg);
			if (a->watch != list)
				break;		/* the app changed its list meanwhile */
		}
	}
}

/* ---- MQTT on the Toon's own broker ------------------------------------------------ */

#define MAX_SUBS	32

/* a topic to publish to: no wildcards, not the broker's own $SYS */
static bool topic_ok(const char *t)
{
	return t && t[0] && t[0] != '$' && strlen(t) < 256 && !strpbrk(t, "+#");
}

/* Under retoon/ an app may only send the documented commands: the states
   there are the ReToon programs' own (toon-boilerd heats on retoon/climate
   and retoon/control), and the screen takes only "view N" and "wake" from
   an app, as toon-mqttd allowed before 1.45. Everything else is free
   (app/<id>/... for an app's own data). */
static bool app_may_send(const char *topic, const char *text, size_t len, bool retain)
{
	int n;
	char end;

	if (strncmp(topic, "retoon/", 7))
		return true;
	if (retain)
		return false;	/* a kept command would come back at every start */
	if (!strcmp(topic, "retoon/boiler/set") || !strcmp(topic, "retoon/display/set") ||
	    !strcmp(topic, "retoon/notify/set"))
		return true;
	if (!strcmp(topic, "retoon/ui/set") && text && len < 32)
		return !strcmp(text, "wake") ||
		       (sscanf(text, "view %d%c", &n, &end) == 1 && n >= 0 && n < 100);
	return false;
}

/* retoon.publish(topic, payload, { retain }): payload a string, or anything
   else as JSON; null or undefined sends an empty message (with retain, that
   takes a kept one away). false if the broker cannot be reached. */
static JSValue js_publish(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
{
	const char *topic = argc > 0 && JS_IsString(argv[0]) ? JS_ToCString(ctx, argv[0]) : NULL;
	const char *text = NULL;
	JSValue json = JS_UNDEFINED;
	bool retain = false, ok;
	size_t len = 0;

	if (!topic_ok(topic)) {
		JS_FreeCString(ctx, topic);
		return JS_ThrowTypeError(ctx, "publish needs a topic without + or #");
	}
	if (argc > 2 && JS_IsObject(argv[2])) {
		JSValue r = JS_GetPropertyStr(ctx, argv[2], "retain");

		retain = JS_ToBool(ctx, r) > 0;
		JS_FreeValue(ctx, r);
	}
	if (argc > 1 && JS_IsString(argv[1])) {
		text = JS_ToCStringLen(ctx, &len, argv[1]);
	} else if (argc > 1 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1])) {
		json = JS_JSONStringify(ctx, argv[1], JS_UNDEFINED, JS_UNDEFINED);
		if (JS_IsException(json)) {
			JS_FreeCString(ctx, topic);
			return json;
		}
		if (JS_IsString(json))
			text = JS_ToCStringLen(ctx, &len, json);
	}
	if (!app_may_send(topic, text, len, retain) ||
	    (!strcmp(topic, "retoon/notify/set") && text && !app_note_ok(app_of(ctx), text, len))) {
		JS_FreeCString(ctx, text);
		JS_FreeValue(ctx, json);
		JS_FreeCString(ctx, topic);
		return JS_FALSE;	/* not sent, as without a broker */
	}
	ok = len <= (256 << 10) && appbus_publish(topic, text ? text : "", len, retain);
	JS_FreeCString(ctx, text);
	JS_FreeValue(ctx, json);
	JS_FreeCString(ctx, topic);
	return JS_NewBool(ctx, ok);
}

static int sub_find(struct app *a, const char *pattern)
{
	for (int i = 0; i < a->nsubs; i++)
		if (!strcmp(a->subs[i].pattern, pattern))
			return i;
	return -1;
}

/* retoon.subscribe(pattern, fn): fn(topic, payload) for each message on a
   topic that matches; + and # as in MQTT. Again with the same pattern:
   the new fn replaces the old one. */
static JSValue js_subscribe(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
{
	struct app *a = app_of(ctx);
	const char *pattern = argc > 0 && JS_IsString(argv[0]) ? JS_ToCString(ctx, argv[0]) : NULL;
	int i;

	if (!pattern || !pattern[0] || pattern[0] == '$' || strlen(pattern) >= 256 ||
	    mosquitto_sub_topic_check(pattern) != MOSQ_ERR_SUCCESS || argc < 2 ||
	    !JS_IsFunction(ctx, argv[1])) {
		JS_FreeCString(ctx, pattern);
		return JS_ThrowTypeError(ctx, "subscribe needs a topic pattern and a function");
	}
	i = sub_find(a, pattern);
	if (i >= 0) {
		JS_FreeValue(ctx, a->subs[i].fn);
	} else if (a->nsubs >= MAX_SUBS) {
		JS_FreeCString(ctx, pattern);
		return JS_ThrowRangeError(ctx, "at most %d subscriptions", MAX_SUBS);
	} else {
		a->subs = realloc(a->subs, (a->nsubs + 1) * sizeof(*a->subs));
		i = a->nsubs++;
		a->subs[i].pattern = strdup(pattern);
		appbus_subscribe(pattern);
	}
	a->subs[i].fn = JS_DupValue(ctx, argv[1]);
	JS_FreeCString(ctx, pattern);
	return JS_UNDEFINED;
}

static JSValue js_unsubscribe(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
{
	struct app *a = app_of(ctx);
	const char *pattern = argc > 0 && JS_IsString(argv[0]) ? JS_ToCString(ctx, argv[0]) : NULL;
	int i = pattern ? sub_find(a, pattern) : -1;

	if (i >= 0) {
		appbus_unsubscribe(a->subs[i].pattern);
		JS_FreeValue(ctx, a->subs[i].fn);
		free(a->subs[i].pattern);
		a->subs[i] = a->subs[--a->nsubs];
	}
	JS_FreeCString(ctx, pattern);
	return JS_UNDEFINED;
}

static void on_bus(const char *topic, const char *payload, size_t len, bool retained)
{
	for (int i = 0; i < napps; i++) {
		struct app *a = &apps[i];

		for (int j = 0; j < a->nsubs; j++) {
			bool match = false;
			JSValue args[2], fn;

			if (mosquitto_topic_matches_sub(a->subs[j].pattern, topic, &match) || !match)
				continue;
			args[0] = JS_NewString(a->ctx, topic);
			args[1] = JS_NewStringLen(a->ctx, payload, len);
			fn = JS_DupValue(a->ctx, a->subs[j].fn);	/* the call may unsubscribe */
			call(a, fn, 2, args, "subscribe");
			JS_FreeValue(a->ctx, fn);
			JS_FreeValue(a->ctx, args[0]);
			JS_FreeValue(a->ctx, args[1]);
			break;		/* one call per app and message */
		}
	}
}

/* ---- notifications ------------------------------------------------------------- */

struct note_cb {
	struct app *a;
	JSValue fn;
};

static void note_done(void *user, const char *event, const char *button)
{
	struct note_cb *c = user;

	if (strcmp(event, "gone") && JS_IsFunction(c->a->ctx, c->fn)) {
		JSValue arg = button ? JS_NewString(c->a->ctx, button) : JS_NULL;

		call(c->a, c->fn, 1, &arg, "notify");
		JS_FreeValue(c->a->ctx, arg);
	}
	JS_FreeValue(c->a->ctx, c->fn);
	free(c);
}

static JSValue js_notify(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
{
	JSValue j;
	const char *s;
	struct note_cb *c;
	bool ok;

	if (argc < 1 || !JS_IsObject(argv[0]))
		return JS_ThrowTypeError(ctx, "notify needs an object");
	j = JS_JSONStringify(ctx, argv[0], JS_UNDEFINED, JS_UNDEFINED);
	s = JS_IsString(j) ? JS_ToCString(ctx, j) : NULL;
	if (s && !app_note_ok(app_of(ctx), s, strlen(s))) {
		JS_FreeCString(ctx, s);
		JS_FreeValue(ctx, j);
		return JS_ThrowTypeError(ctx, "camera: not in the manifest's \"cameras\"");
	}
	c = calloc(1, sizeof(*c));
	c->a = app_of(ctx);
	c->fn = argc > 1 && JS_IsFunction(ctx, argv[1]) ? JS_DupValue(ctx, argv[1]) : JS_UNDEFINED;
	/* the callback frees c, also when the message is left out */
	ok = s && notify_show(s, note_done, c);
	if (!ok) {
		JS_FreeValue(ctx, c->fn);
		free(c);
	}
	JS_FreeCString(ctx, s);
	JS_FreeValue(ctx, j);
	return JS_NewBool(ctx, ok);
}

/* ---- Home Assistant services (level 7) ------------------------------------- */

#define CALL_HOLD_MS	60000	/* how long a call waits for the connection */

struct ha_wait {
	struct app *a;
	JSValue resolve, reject;
	cJSON *msg;		/* held: not sent yet, the connection is not up */
	uint32_t until;		/* held: give up then */
	struct ha_wait *next;
};

static struct ha_wait *held, **held_tail = &held;

/* may the app call domain.service: listed in its manifest's "services" as
   that, or as "domain.*" */
static bool service_allowed(struct app *a, const char *domain, const char *service)
{
	cJSON *v;
	size_t dl = strlen(domain);

	cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(a->manifest, "services")) {
		const char *s = cJSON_IsString(v) ? v->valuestring : "";

		if (strncmp(s, domain, dl) || s[dl] != '.')
			continue;
		if (!strcmp(s + dl + 1, "*") || !strcmp(s + dl + 1, service))
			return true;
	}
	return false;
}

/* settle the promise: from Home Assistant's answer through call() (runs
   the app's jobs), inside retoon.call itself directly (the app's own call
   is still running and keeps its time limit) */
static void settle(struct app *a, JSValue fn, JSValue v, bool inside)
{
	if (inside)
		JS_FreeValue(a->ctx, JS_Call(a->ctx, fn, JS_UNDEFINED, 1, (JSValueConst *)&v));
	else
		call(a, fn, 1, (JSValueConst *)&v, "call");
	JS_FreeValue(a->ctx, v);
}

static void call_reject(struct app *a, JSValue reject, const char *why, bool inside)
{
	JSValue e = JS_NewError(a->ctx);

	JS_SetPropertyStr(a->ctx, e, "message", JS_NewString(a->ctx, why));
	settle(a, reject, e, inside);
}

static void on_call_result(cJSON *result, void *user)
{
	struct ha_wait *w = user;
	struct app *a = w->a;

	if (result) {
		cJSON *resp = cJSON_GetObjectItemCaseSensitive(result, "response");
		char *s = resp && !cJSON_IsNull(resp) ? cJSON_PrintUnformatted(resp) : NULL;
		JSValue v = s ? JS_ParseJSON(a->ctx, s, strlen(s), "<response>") : JS_NULL;

		free(s);
		settle(a, w->resolve, v, false);
	} else {
		call_reject(a, w->reject, ha_error(), false);
	}
	JS_FreeValue(a->ctx, w->resolve);
	JS_FreeValue(a->ctx, w->reject);
	free(w);
}

/* send a call; false when the connection went down while sending */
static bool call_send(struct ha_wait *w)
{
	cJSON *o = w->msg;

	w->msg = NULL;
	if (ha_request(o, on_call_result, w))
		return true;
	call_reject(w->a, w->reject, "not connected to Home Assistant", false);
	JS_FreeValue(w->a->ctx, w->resolve);
	JS_FreeValue(w->a->ctx, w->reject);
	free(w);
	return false;
}

/* Calls made while the Toon was not connected to Home Assistant (apps start
   before the connection is up, and a connection can drop for a moment):
   sent in their order once it is up, refused after CALL_HOLD_MS with the
   connection's status as the reason. Taken off the list first, because a
   callback can make a new call. */
static void held_poll(void)
{
	struct ha_wait *list = held, *w;
	bool up = ha_ready();
	uint32_t now = now_ms();

	if (!held || (!up && (int32_t)(now - held->until) < 0))
		return;
	held = NULL;
	held_tail = &held;
	while ((w = list)) {
		list = w->next;
		w->next = NULL;
		if (up) {
			call_send(w);
		} else if ((int32_t)(now - w->until) >= 0) {
			char why[200];

			snprintf(why, sizeof(why), "not connected to Home Assistant (%s)", ha_status());
			cJSON_Delete(w->msg);
			call_reject(w->a, w->reject, why, false);
			JS_FreeValue(w->a->ctx, w->resolve);
			JS_FreeValue(w->a->ctx, w->reject);
			free(w);
		} else {
			*held_tail = w;
			held_tail = &w->next;
		}
	}
}

static JSValue js_call(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
{
	struct app *a = app_of(ctx);
	JSValue funcs[2], promise, js;
	const char *name = argc > 0 && JS_IsString(argv[0]) ? JS_ToCString(ctx, argv[0]) : NULL;
	char domain[64], service[64], why[200];
	cJSON *data = NULL, *ent, *o;
	bool response = false;
	const char *s;

	if (!name || sscanf(name, "%63[a-z0-9_].%63[a-z0-9_]", domain, service) != 2) {
		JS_FreeCString(ctx, name);
		return JS_ThrowTypeError(ctx, "call needs \"domain.service\"");
	}
	JS_FreeCString(ctx, name);
	if (argc > 1 && JS_IsObject(argv[1])) {
		js = JS_JSONStringify(ctx, argv[1], JS_UNDEFINED, JS_UNDEFINED);
		s = JS_IsString(js) ? JS_ToCString(ctx, js) : NULL;
		data = s ? cJSON_Parse(s) : NULL;
		JS_FreeCString(ctx, s);
		JS_FreeValue(ctx, js);
	}
	if (argc > 2 && JS_IsObject(argv[2])) {
		JSValue v = JS_GetPropertyStr(ctx, argv[2], "response");

		response = JS_ToBool(ctx, v) > 0;
		JS_FreeValue(ctx, v);
	}
	promise = JS_NewPromiseCapability(ctx, funcs);
	if (!service_allowed(a, domain, service)) {
		snprintf(why, sizeof(why), "%s.%s is not in the manifest's \"services\"", domain, service);
		call_reject(a, funcs[1], why, true);
		goto out;
	}
	/* the Toon's own entities: as a card's button, no answer */
	ent = cJSON_GetObjectItemCaseSensitive(data, "entity_id");
	if (cJSON_IsString(ent) && (is_local(ent->valuestring) || is_mqtt(ent->valuestring))) {
		char id[96];

		snprintf(id, sizeof(id), "%s", ent->valuestring);
		cJSON_DeleteItemFromObjectCaseSensitive(data, "entity_id");
		ha_call(domain, service, id, data);
		data = NULL;
		settle(a, funcs[0], JS_NULL, true);
		goto out;
	}
	o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "type", "call_service");
	cJSON_AddStringToObject(o, "domain", domain);
	cJSON_AddStringToObject(o, "service", service);
	cJSON_AddItemToObject(o, "service_data", data ? data : cJSON_CreateObject());
	data = NULL;
	if (response)
		cJSON_AddTrueToObject(o, "return_response");
	{
		struct ha_wait *w = calloc(1, sizeof(*w));

		w->a = a;
		w->resolve = JS_DupValue(ctx, funcs[0]);
		w->reject = JS_DupValue(ctx, funcs[1]);
		if (ha_ready() && !held) {
			if (!ha_request(o, on_call_result, w)) {
				JS_FreeValue(ctx, w->resolve);
				JS_FreeValue(ctx, w->reject);
				free(w);
				call_reject(a, funcs[1], "not connected to Home Assistant", true);
			}
		} else {
			/* not connected (yet): hold it, held_poll sends it */
			w->msg = o;
			w->until = now_ms() + CALL_HOLD_MS;
			*held_tail = w;
			held_tail = &w->next;
		}
	}
out:
	cJSON_Delete(data);
	JS_FreeValue(ctx, funcs[0]);
	JS_FreeValue(ctx, funcs[1]);
	return promise;
}

static JSValue js_sound(JSContext *ctx, JSValueConst this, int argc, JSValueConst *argv)
{
	const char *name = argc > 0 && JS_IsString(argv[0]) ? JS_ToCString(ctx, argv[0]) : NULL;
	double vol = -1;

	if (argc > 1 && JS_IsNumber(argv[1]))
		JS_ToFloat64(ctx, &vol, argv[1]);
	if (name)
		notify_sound(name, (int)vol);
	JS_FreeCString(ctx, name);
	return JS_UNDEFINED;
}

static const JSCFunctionListEntry retoon_funcs[] = {
	JS_CFUNC_DEF("notify", 2, js_notify),
	JS_CFUNC_DEF("sound", 2, js_sound),
	JS_CFUNC_DEF("call", 3, js_call),
	JS_CFUNC_DEF("watch", 2, js_watch),
	JS_CFUNC_DEF("state", 1, js_state),
	JS_CFUNC_DEF("fetch", 2, js_fetch),
	JS_CFUNC_DEF("tile", 1, js_tile),
	JS_CFUNC_DEF("screen", 1, js_screen),
	JS_CFUNC_DEF("update", 0, js_update),
	JS_CFUNC_DEF("onSettings", 1, js_on_settings),
	JS_CFUNC_DEF("log", 1, js_log),
	JS_CFUNC_DEF("publish", 3, js_publish),
	JS_CFUNC_DEF("subscribe", 2, js_subscribe),
	JS_CFUNC_DEF("unsubscribe", 1, js_unsubscribe),
};

/* the manifest's defaults with the saved settings over them */
static void load_settings(struct app *a)
{
	char path[256];
	cJSON *saved, *s;
	struct stat sb;
	JSValue obj = JS_NewObject(a->ctx), g;

	cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(a->manifest, "settings")) {
		cJSON *k = cJSON_GetObjectItemCaseSensitive(s, "key");
		cJSON *d = cJSON_GetObjectItemCaseSensitive(s, "default");
		char *j = d ? cJSON_PrintUnformatted(d) : NULL;

		if (cJSON_IsString(k) && j)
			JS_SetPropertyStr(a->ctx, obj, k->valuestring, JS_ParseJSON(a->ctx, j, strlen(j), "<default>"));
		free(j);
	}
	snprintf(path, sizeof(path), APPS_CONF "/%s.json", a->id);
	a->mt_settings = stat(path, &sb) ? 0 : sb.st_mtime;
	saved = NULL;
	{
		FILE *f = fopen(path, "r");

		if (f) {
			char buf[16384];
			size_t n = fread(buf, 1, sizeof(buf) - 1, f);

			buf[n] = 0;
			saved = cJSON_Parse(buf);
			fclose(f);
		}
	}
	cJSON_ArrayForEach(s, saved) {
		char *j = cJSON_PrintUnformatted(s);

		if (s->string && j)
			JS_SetPropertyStr(a->ctx, obj, s->string, JS_ParseJSON(a->ctx, j, strlen(j), "<setting>"));
		free(j);
	}
	cJSON_Delete(saved);
	g = JS_GetGlobalObject(a->ctx);
	{
		JSValue rt = JS_GetPropertyStr(a->ctx, g, "retoon");

		JS_SetPropertyStr(a->ctx, rt, "settings", obj);
		JS_FreeValue(a->ctx, rt);
	}
	JS_FreeValue(a->ctx, g);
}

static char *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	char *buf;
	long n;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	rewind(f);
	buf = malloc(n + 1);
	*len = fread(buf, 1, n, f);
	buf[*len] = 0;
	fclose(f);
	return buf;
}

static void load_app(const char *dir)
{
	struct app *a = &apps[napps];
	char path[512], *src, *main_js = "main.js";
	cJSON *m, *v;
	JSValue g, rt, proto, console, info, r;
	size_t len;

	snprintf(path, sizeof(path), APPS_DIR "/%s/manifest.json", dir);
	src = read_file(path, &len);
	m = src ? cJSON_Parse(src) : NULL;
	free(src);
	if (!m) {
		fprintf(stderr, "app %s: no readable manifest.json\n", dir);
		return;
	}
	memset(a, 0, sizeof(*a));
	a->manifest = m;
	v = cJSON_GetObjectItemCaseSensitive(m, "id");
	snprintf(a->id, sizeof(a->id), "%s", cJSON_IsString(v) ? v->valuestring : dir);
	v = cJSON_GetObjectItemCaseSensitive(m, "name");
	snprintf(a->name, sizeof(a->name), "%s", cJSON_IsString(v) ? v->valuestring : a->id);
	v = cJSON_GetObjectItemCaseSensitive(m, "version");
	snprintf(a->version, sizeof(a->version), "%s", cJSON_IsString(v) ? v->valuestring : "");
	v = cJSON_GetObjectItemCaseSensitive(m, "main");
	if (cJSON_IsString(v) && !strchr(v->valuestring, '/'))
		main_js = v->valuestring;
	a->tile_fn = a->screen_fn = a->settings_fn = a->state_fn = JS_UNDEFINED;

	a->rt = JS_NewRuntime();
	JS_SetMemoryLimit(a->rt, MEM_LIMIT);
	JS_SetMaxStackSize(a->rt, 256 << 10);
	JS_SetInterruptHandler(a->rt, interrupt, a);
	JS_NewClass(a->rt, el_class, &el_def);
	a->ctx = JS_NewContext(a->rt);
	JS_SetContextOpaque(a->ctx, a);
	proto = JS_NewObject(a->ctx);
	JS_SetPropertyFunctionList(a->ctx, proto, el_proto, sizeof(el_proto) / sizeof(el_proto[0]));
	JS_SetClassProto(a->ctx, el_class, proto);

	g = JS_GetGlobalObject(a->ctx);
	rt = JS_NewObject(a->ctx);
	JS_SetPropertyFunctionList(a->ctx, rt, retoon_funcs, sizeof(retoon_funcs) / sizeof(retoon_funcs[0]));
	info = JS_NewObject(a->ctx);
	JS_SetPropertyStr(a->ctx, info, "id", JS_NewString(a->ctx, a->id));
	JS_SetPropertyStr(a->ctx, info, "name", JS_NewString(a->ctx, a->name));
	JS_SetPropertyStr(a->ctx, info, "version", JS_NewString(a->ctx, a->version));
	JS_SetPropertyStr(a->ctx, rt, "app", info);
	JS_SetPropertyStr(a->ctx, g, "retoon", rt);
	console = JS_NewObject(a->ctx);
	JS_SetPropertyStr(a->ctx, console, "log", JS_NewCFunction(a->ctx, js_log, "log", 1));
	JS_SetPropertyStr(a->ctx, g, "console", console);
	JS_SetPropertyStr(a->ctx, g, "setTimeout", JS_NewCFunction(a->ctx, js_set_timeout, "setTimeout", 2));
	JS_SetPropertyStr(a->ctx, g, "setInterval", JS_NewCFunction(a->ctx, js_set_interval, "setInterval", 2));
	JS_SetPropertyStr(a->ctx, g, "clearTimeout", JS_NewCFunction(a->ctx, js_clear_timer, "clearTimeout", 1));
	JS_SetPropertyStr(a->ctx, g, "clearInterval", JS_NewCFunction(a->ctx, js_clear_timer, "clearInterval", 1));
	JS_FreeValue(a->ctx, g);
	napps++;
	load_settings(a);

	v = cJSON_GetObjectItemCaseSensitive(m, "api");
	if (cJSON_IsNumber(v) && v->valueint > APPS_API) {
		snprintf(a->error, sizeof(a->error), "%s", tr("needs a newer ReToon. Please update it "
			 "(System in the web interface)."));
		a->failed = true;
		fprintf(stderr, "app %s: needs api %d, this is %d\n", a->id, v->valueint, APPS_API);
		return;
	}
	snprintf(path, sizeof(path), APPS_DIR "/%s/%s", dir, main_js);
	src = read_file(path, &len);
	if (!src) {
		snprintf(a->error, sizeof(a->error), "%s is missing", main_js);
		a->failed = true;
		return;
	}
	a->deadline = now_ms() + CALL_LIMIT_MS;
	r = JS_Eval(a->ctx, src, len, main_js, JS_EVAL_TYPE_GLOBAL);
	a->deadline = 0;
	free(src);
	if (JS_IsException(r))
		report(a, "start");
	JS_FreeValue(a->ctx, r);
	run_jobs(a);
	fprintf(stderr, "app %s %s: started\n", a->id, a->version);
}

void apps_init(void)
{
	DIR *d = opendir(APPS_DIR);
	struct dirent *e;

	curl_global_init(CURL_GLOBAL_DEFAULT);
	multi = curl_multi_init();
	JS_NewClassID(&el_class);
	while (d && (e = readdir(d)) && napps < MAX_APPS)
		if (e->d_name[0] != '.')
			load_app(e->d_name);
	if (d)
		closedir(d);
}

void apps_poll(void)
{
	static uint32_t t_conf;

	fetch_poll();
	pic_poll();
	timers_poll();
	held_poll();
	watch_poll();
	appbus_poll(on_bus);
	for (int i = 0; i < napps; i++)
		if (want_update[i]) {
			want_update[i] = false;
			update_app(&apps[i]);
		}
	for (struct card *c = cards; c; c = c->next)
		if (c->resized) {
			c->resized = false;
			draw_card(c);
		}
	/* settings saved by the web interface */
	if (now_ms() - t_conf > 3000) {
		t_conf = now_ms();
		for (int i = 0; i < napps; i++) {
			struct app *a = &apps[i];
			char path[256];
			struct stat sb;

			snprintf(path, sizeof(path), APPS_CONF "/%s.json", a->id);
			if ((stat(path, &sb) ? 0 : sb.st_mtime) != a->mt_settings) {
				load_settings(a);
				call(a, a->settings_fn, 0, NULL, "settings");
			}
		}
	}
}
