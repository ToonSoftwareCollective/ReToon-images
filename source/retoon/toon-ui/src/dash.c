// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * toon-ui: dashboards. The file uses Home Assistant's dashboard format
 * (views -> sections -> cards), so a view can be copied over from Home
 * Assistant as it is. Cards this program does not know fall back to a tile
 * or a list of their entities.
 */
#include <ctype.h>
#include <math.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "ui.h"

/* pages on the screen (all built at the start); the web editor stops at the
   same number (MAX_VIEWS in app.js) */
#define MAX_VIEWS 32
#define GAP	S(10)
/* A tile, and a row of the sections' grid, is as tall as makes FIT_ROWS
   rows fill the page above the bar exactly (on a Toon 2 63 pixels); set
   in dash_init() from the screen. */
#define FIT_ROWS	7
#define DIM_ROWS	8	/* the dimmed page has no bar under it */
static int page_rows = FIT_ROWS;	/* of the page being built */
#define TILE_H	tile_h
#define ROW_H(n)	((n) * (TILE_H + GAP) - GAP)
static int tile_h = 66;
#define FEAT_H	S(50)

struct card {
	cJSON *cfg;
	char ent[96];
	bool row;			/* a line in an entities card */
	lv_obj_t *root, *icon_bg, *icon, *name, *state;
	lv_obj_t *slider, *btns, *big, *target, *action;
	lv_obj_t *arc, *dot, *act_icon, *info, *mode;	/* the thermostat dial */
	lv_obj_t *prog;			/* ReToon's thermostat: week program on/off */
	lv_obj_t *info_icon;		/* ... and who chose: hourglass, lock */
	int arc_from;			/* the dial's value when its knob was taken, -1: not */
	bool classic;			/* the thermostat's "classic" style: no ring */
	lv_obj_t *steps;		/* ... its - and + (the others: c->target's parent) */
	lv_point_precise_t tick[2];	/* dot: a bar where the room is now */
	const char *feature;		/* what slider/btns control */
	cJSON *feat;			/* its settings in the card (in cfg) */
	char sig[256];			/* the options the buttons were built for */
	lv_obj_t *gauge, *needle;	/* a gauge card: its ring, its needle (or NULL) */
	struct card *next;
};

static lv_obj_t *bar, *nav, *content, *clock_lbl, *wifi_icon, *ha_icon, *mqtt_icon;
static lv_obj_t *views[MAX_VIEWS], *navbtn[MAX_VIEWS];
static cJSON *doc;
static struct card *cards;
static int nviews, cur, columns = 2, std_columns = 2;
static bool page_anim = true;	/* swiping slides the pages (ui.conf page_anim) */
/* ReToon's own thermostat: the presets and the week program button for a
   card that does not choose (ui.conf thermostat_buttons, on unless 0). From
   before the choice per card: dash_load moves it into the cards of a saved
   dashboard once; only the standard pages still follow it. */
static bool th_buttons = true;

/* the row of buttons under a thermostat: a card's features list decides (an
   empty one: none, as in Home Assistant); without one, ReToon's own follows
   the old setting for every card */
static bool thermostat_buttons(cJSON *cfg, const char *entity)
{
	cJSON *fl = cJSON_GetObjectItemCaseSensitive(cfg, "features");

	if (cJSON_IsArray(fl))
		return cJSON_GetArraySize(fl) > 0;
	return strcmp(entity, "toon.thermostat") || th_buttons;
}

static bool is_toon1(void)
{
	static int t = -1;
	char model[64] = "";
	FILE *f;

	if (t < 0) {
		f = fopen("/proc/device-tree/model", "r");
		if (f) {
			if (!fgets(model, sizeof(model), f))
				model[0] = 0;
			fclose(f);
		}
		t = strstr(model, "Toon 1") != NULL;
	}
	return t;
}
static time_t mt_dash, mt_ui, mt_ha;
static bool std_pages, hw_seen;	/* the standard pages, and what they were built for */
static uint32_t t_tick;

int dash_view(void) { return cur; }
int dash_views(void) { return nviews; }

/* ---- small helpers ----------------------------------------------------- */

static const char *jstr(cJSON *o, const char *key)
{
	cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

	return cJSON_IsString(v) && v->valuestring[0] ? v->valuestring : NULL;
}

static lv_obj_t *box(lv_obj_t *parent, int w, int h)
{
	lv_obj_t *o = lv_obj_create(parent);

	lv_obj_remove_style_all(o);
	lv_obj_set_size(o, w, h);
	lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
	return o;
}

static lv_obj_t *panel(lv_obj_t *parent, int w, int h)
{
	lv_obj_t *o = box(parent, w, h);

	lv_obj_set_style_bg_color(o, lv_color_hex(C_CARD), 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(o, S(14), 0);
	return o;
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *s)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_label_set_text(l, s ? s : "");	/* first: the default text has letters icon fonts lack */
	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
	return l;
}

static void set_text(lv_obj_t *l, const char *s)
{
	if (l && strcmp(lv_label_get_text(l), s))
		lv_label_set_text(l, s);
}

static void domain_of(const char *id, char *out, size_t len)
{
	snprintf(out, len, "%s", is_mqtt(id) ? mqtt_domain(id) : id);
	out[strcspn(out, ".")] = 0;
}

static bool is_unknown(const char *s)
{
	return !s || !strcmp(s, "unavailable") || !strcmp(s, "unknown");
}

static bool is_active(const char *domain, const char *s)
{
	if (is_unknown(s))
		return false;
	if (!strcmp(domain, "climate") || !strcmp(domain, "water_heater"))
		return strcmp(s, "off");
	if (!strcmp(domain, "media_player"))
		return !strcmp(s, "playing") || !strcmp(s, "on") || !strcmp(s, "buffering");
	if (!strcmp(domain, "person") || !strcmp(domain, "device_tracker"))
		return !strcmp(s, "home");
	if (!strcmp(domain, "lock"))
		return !strcmp(s, "unlocked") || !strcmp(s, "open");
	if (!strcmp(domain, "vacuum"))
		return !strcmp(s, "cleaning");
	if (!strcmp(domain, "alarm_control_panel"))
		return strcmp(s, "disarmed");
	return !strcmp(s, "on") || !strcmp(s, "open") || !strcmp(s, "opening") ||
	       !strcmp(s, "heating") || !strcmp(s, "active");
}

static uint32_t named_color(const char *n)
{
	static const struct { const char *n; uint32_t c; } t[] = {
		{ "red", 0xf44336 }, { "pink", 0xe91e63 }, { "purple", 0x926bc7 },
		{ "deep-purple", 0x6e41ab }, { "indigo", 0x3f51b5 }, { "blue", 0x2196f3 },
		{ "light-blue", 0x03a9f4 }, { "cyan", 0x00bcd4 }, { "teal", 0x009688 },
		{ "green", 0x4caf50 }, { "light-green", 0x8bc34a }, { "lime", 0xcddc39 },
		{ "yellow", 0xffeb3b }, { "amber", 0xffc107 }, { "orange", 0xff9800 },
		{ "deep-orange", 0xff6f22 }, { "brown", 0x795548 }, { "grey", 0x9e9e9e },
		{ "blue-grey", 0x607d8b }, { "primary", C_ACCENT }, { "accent", 0xff9800 },
		{ "white", 0xffffff },
	};

	for (size_t i = 0; n && i < sizeof(t) / sizeof(t[0]); i++)
		if (!strcmp(t[i].n, n))
			return t[i].c;
	if (n && n[0] == '#' && strlen(n) == 7)
		return strtoul(n + 1, NULL, 16);
	return 0;
}

uint32_t dash_color(const char *name, uint32_t def)
{
	uint32_t c = named_color(name);

	return c ? c : def;
}

static uint32_t state_color(struct card *c, struct entity *e, const char *domain)
{
	uint32_t col = named_color(jstr(c->cfg, "color"));
	cJSON *rgb = cJSON_GetObjectItemCaseSensitive(e->attr, "rgb_color");
	const char *act;

	if (is_unknown(e->state))
		return 0x596273;
	if (!strcmp(domain, "sensor") || !strcmp(domain, "toon") || !strcmp(domain, "weather") ||
	    !strcmp(domain, "sun") || !strcmp(domain, "select") || !strcmp(domain, "input_select") ||
	    !strcmp(domain, "number") || !strcmp(domain, "input_number")) {
		if (strcmp(domain, "toon") || !(!strcmp(e->state, "on") || !strcmp(e->state, "off") ||
						!strcmp(e->state, "heat")))
			return col ? col : 0x5b9bd5;
	}
	if (!is_active(domain, e->state) && strcmp(e->state, "heat"))
		return 0x6b7486;
	if (col)
		return col;
	if (!strcmp(domain, "light")) {
		if (cJSON_GetArraySize(rgb) == 3)
			return cJSON_GetArrayItem(rgb, 0)->valueint << 16 |
			       cJSON_GetArrayItem(rgb, 1)->valueint << 8 |
			       cJSON_GetArrayItem(rgb, 2)->valueint;
		return 0xffc107;
	}
	if (!strcmp(domain, "climate") || !strcmp(e->id, "toon.thermostat")) {
		act = ent_str(e, "hvac_action");
		return !strcmp(e->state, "cool") ? 0x2196f3 :
		       act && !strcmp(act, "idle") ? 0xd98a4a : 0xff6f22;
	}
	if (!strcmp(domain, "fan"))
		return 0x00bcd4;
	if (!strcmp(domain, "cover"))
		return 0x926bc7;
	if (!strcmp(domain, "lock") || !strcmp(domain, "alarm_control_panel"))
		return 0xf44336;
	if (!strcmp(domain, "person") || !strcmp(domain, "device_tracker"))
		return 0x4caf50;
	if (!strcmp(domain, "media_player"))
		return 0x03a9f4;
	if (!strcmp(e->id, "toon.flame") || !strcmp(e->id, "toon.heat_request"))
		return 0xff6f22;
	if (!strcmp(e->id, "toon.boiler_fault"))
		return 0xf44336;
	return 0xffc107;
}

static const char *default_icon(struct entity *e, const char *domain)
{
	static const struct { const char *domain, *dclass, *icon; } t[] = {
		{ "light", NULL, "mdi:lightbulb" },
		{ "switch", "outlet", "mdi:power-socket-eu" },
		{ "switch", NULL, "mdi:toggle-switch-variant" },
		{ "input_boolean", NULL, "mdi:toggle-switch-variant" },
		{ "fan", NULL, "mdi:fan" },
		{ "climate", NULL, "mdi:thermostat" },
		{ "cover", "garage", "mdi:garage" },
		{ "cover", NULL, "mdi:window-shutter" },
		{ "lock", NULL, "mdi:lock" },
		{ "media_player", "tv", "mdi:television" },
		{ "media_player", "speaker", "mdi:speaker" },
		{ "media_player", NULL, "mdi:cast" },
		{ "person", NULL, "mdi:account" },
		{ "device_tracker", NULL, "mdi:account" },
		{ "sun", NULL, "mdi:white-balance-sunny" },
		{ "weather", NULL, "mdi:weather-partly-cloudy" },
		{ "scene", NULL, "mdi:palette" },
		{ "script", NULL, "mdi:script-text" },
		{ "automation", NULL, "mdi:robot" },
		{ "button", NULL, "mdi:gesture-tap-button" },
		{ "input_button", NULL, "mdi:gesture-tap-button" },
		{ "binary_sensor", "door", "mdi:door" },
		{ "binary_sensor", "garage_door", "mdi:garage" },
		{ "binary_sensor", "window", "mdi:window-closed" },
		{ "binary_sensor", "opening", "mdi:square-outline" },
		{ "binary_sensor", "motion", "mdi:motion-sensor" },
		{ "binary_sensor", "occupancy", "mdi:home" },
		{ "binary_sensor", "presence", "mdi:home" },
		{ "binary_sensor", "smoke", "mdi:smoke-detector" },
		{ "binary_sensor", "moisture", "mdi:water" },
		{ "binary_sensor", "battery", "mdi:battery" },
		{ "binary_sensor", "connectivity", "mdi:check-network-outline" },
		{ "binary_sensor", "problem", "mdi:alert-circle" },
		{ "binary_sensor", "plug", "mdi:power-plug" },
		{ "binary_sensor", "power", "mdi:power-plug" },
		{ "binary_sensor", "heat", "mdi:fire" },
		{ "binary_sensor", "running", "mdi:play" },
		{ "binary_sensor", "update", "mdi:package-up" },
		{ "binary_sensor", NULL, "mdi:checkbox-marked-circle" },
		{ "sensor", "temperature", "mdi:thermometer" },
		{ "sensor", "humidity", "mdi:water-percent" },
		{ "sensor", "power", "mdi:flash" },
		{ "sensor", "energy", "mdi:lightning-bolt" },
		{ "sensor", "battery", "mdi:battery" },
		{ "sensor", "illuminance", "mdi:brightness-5" },
		{ "sensor", "pressure", "mdi:gauge" },
		{ "sensor", "atmospheric_pressure", "mdi:gauge" },
		{ "sensor", "carbon_dioxide", "mdi:molecule-co2" },
		{ "sensor", "voltage", "mdi:sine-wave" },
		{ "sensor", "current", "mdi:current-ac" },
		{ "sensor", "gas", "mdi:meter-gas" },
		{ "sensor", "water", "mdi:water" },
		{ "sensor", "signal_strength", "mdi:wifi" },
		{ "sensor", "timestamp", "mdi:clock-outline" },
		{ "sensor", "duration", "mdi:timer-outline" },
		{ "sensor", "monetary", "mdi:cash" },
		{ "sensor", "aqi", "mdi:air-filter" },
		{ "sensor", "volatile_organic_compounds", "mdi:air-filter" },
		{ "sensor", "pm25", "mdi:air-filter" },
		{ "sensor", "frequency", "mdi:sine-wave" },
		{ "sensor", "wind_speed", "mdi:weather-windy" },
		{ "sensor", "precipitation", "mdi:weather-rainy" },
		{ "sensor", NULL, "mdi:eye" },
		{ "select", NULL, "mdi:format-list-bulleted" },
		{ "input_select", NULL, "mdi:format-list-bulleted" },
		{ "number", NULL, "mdi:ray-vertex" },
		{ "input_number", NULL, "mdi:ray-vertex" },
		{ "update", NULL, "mdi:package-up" },
		{ "vacuum", NULL, "mdi:robot-vacuum" },
		{ "camera", NULL, "mdi:video" },
		{ "zone", NULL, "mdi:map-marker" },
		{ "calendar", NULL, "mdi:calendar" },
		{ "alarm_control_panel", NULL, "mdi:shield" },
		{ "water_heater", NULL, "mdi:thermometer" },
		{ "humidifier", NULL, "mdi:air-humidifier" },
		{ "timer", NULL, "mdi:timer-outline" },
		{ "counter", NULL, "mdi:counter" },
		{ "input_datetime", NULL, "mdi:calendar-clock" },
		{ "input_text", NULL, "mdi:form-textbox" },
		{ "remote", NULL, "mdi:remote" },
		{ "siren", NULL, "mdi:bullhorn" },
		{ "todo", NULL, "mdi:clipboard-list" },
		{ "valve", NULL, "mdi:valve" },
	};
	const char *dc = ent_str(e, "device_class");

	if (!strcmp(domain, "lock") && e->state && !strcmp(e->state, "unlocked"))
		return "mdi:lock-open";
	if (!strcmp(domain, "binary_sensor") && e->state && !strcmp(e->state, "on") && dc) {
		if (!strcmp(dc, "door"))
			return "mdi:door-open";
		if (!strcmp(dc, "window"))
			return "mdi:window-open";
	}
	for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
		if (!strcmp(t[i].domain, domain) && (!t[i].dclass || (dc && !strcmp(dc, t[i].dclass))))
			return t[i].icon;
	return "mdi:eye";
}

/* "heat_cool" -> "Heat cool", or the Dutch word for it */
static const char *state_word(const char *s, char *buf, size_t len)
{
	if (state_text(s))
		return state_text(s);
	snprintf(buf, len, "%s", s);
	buf[0] = toupper(buf[0]);
	for (char *p = buf; *p; p++)
		if (*p == '_')
			*p = ' ';
	return buf;
}

/* A number as a state shows it (s is the text, v its value): with Home
   Assistant's display precision for the entity when it has one, else at
   most one decimal (none from 1000 up) */
const char *fmt_number(struct entity *e, const char *s, double v)
{
	static char num[48];
	const char *dot = strchr(s, '.');
	int dp = ent_precision(e);

	if (dp >= 0)
		snprintf(num, sizeof(num), "%.*f", dp, v);
	else if (dot && strlen(dot) > 2)
		snprintf(num, sizeof(num), v >= 1000 || v <= -1000 ? "%.0f" : "%.1f", v);
	else
		snprintf(num, sizeof(num), "%s", s);
	return num;
}

/* What a person reads for the state: number with unit, or a tidy word. */
static void fmt_state(struct entity *e, const char *domain, char *out, size_t len)
{
	char word[64];
	const char *s = e->state, *unit = ent_str(e, "unit_of_measurement");
	const char *dc = ent_str(e, "device_class");
	char *end;
	double v;

	if (!s) {
		snprintf(out, len, "…");
		return;
	}
	if (!strcmp(domain, "scene") || !strcmp(domain, "script") || !strcmp(domain, "button") ||
	    !strcmp(domain, "input_button")) {
		/* their state is only when they last ran */
		snprintf(out, len, "%s", tr(!strcmp(s, "unavailable") ? "Unavailable" :
			 !strcmp(domain, "scene") ? "Scene" : !strcmp(domain, "script") ? "Script" : "Press"));
		return;
	}
	if (is_unknown(s)) {
		snprintf(out, len, "%s", tr(!strcmp(s, "unknown") ? "Unknown" : "Unavailable"));
		return;
	}
	v = strtod(s, &end);
	if (end != s && !*end) {
		snprintf(out, len, unit ? "%s %s" : "%s", fmt_number(e, s, v), unit);
		return;
	}
	if (!strcmp(domain, "binary_sensor") && dc) {
		bool on = !strcmp(s, "on");

		if (!strcmp(dc, "door") || !strcmp(dc, "window") || !strcmp(dc, "opening") ||
		    !strcmp(dc, "garage_door")) {
			snprintf(out, len, "%s", tr(on ? "Open" : "Closed"));
			return;
		}
		if (!strcmp(dc, "motion") || !strcmp(dc, "occupancy") || !strcmp(dc, "presence") ||
		    !strcmp(dc, "smoke") || !strcmp(dc, "moisture")) {
			snprintf(out, len, "%s", tr(on ? "Detected" : "Clear"));
			return;
		}
		if (!strcmp(dc, "connectivity")) {
			snprintf(out, len, "%s", tr(on ? "Connected" : "Disconnected"));
			return;
		}
		if (!strcmp(dc, "problem")) {
			snprintf(out, len, "%s", tr(on ? "Problem" : "OK"));
			return;
		}
	}
	if (!strcmp(domain, "light") && !strcmp(s, "on") &&
	    cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(e->attr, "brightness"))) {
		snprintf(out, len, "%d %%", (int)(ent_num(e, "brightness", 0) * 100 / 255 + 0.5));
		return;
	}
	if ((!strcmp(domain, "climate") || !strcmp(e->id, "toon.thermostat")) &&
	    cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(e->attr, "current_temperature"))) {
		const char *a = ent_str(e, "hvac_action");

		if (a && !strcmp(a, "heating"))
			snprintf(out, len, tr("Heating · %.1f °C"), ent_num(e, "current_temperature", 0));
		else
			snprintf(out, len, "%s · %.1f °C", state_word(s, word, sizeof(word)),
				 ent_num(e, "current_temperature", 0));
		return;
	}
	if (!strcmp(e->id, "toon.hot_water") && ent_str(e, "status_text") &&
	    strcmp(s, "unavailable")) {
		snprintf(out, len, "%s", ent_str(e, "status_text"));
		return;
	}
	if (!strcmp(domain, "weather") &&
	    cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(e->attr, "temperature"))) {
		snprintf(out, len, "%s · %.0f °C", state_word(s, word, sizeof(word)),
			 ent_num(e, "temperature", 0));
		for (char *p = out; *p; p++)
			if (*p == '-' || *p == '_')
				*p = ' ';
		return;
	}
	if (!strcmp(domain, "media_player") && ent_str(e, "media_title")) {
		snprintf(out, len, "%s", ent_str(e, "media_title"));
		return;
	}
	/* 2026-10-11T18:00:00+00:00 -> 2026-10-11 18:00 */
	if (strlen(s) >= 16 && s[4] == '-' && s[7] == '-' && s[10] == 'T') {
		snprintf(out, len, "%.10s %.5s", s, s + 11);
		return;
	}
	if (state_text(s)) {
		snprintf(out, len, "%s", state_text(s));
		return;
	}
	snprintf(out, len, "%s", s);
	out[0] = toupper(out[0]);
	for (char *p = out; *p; p++)
		if (*p == '_')
			*p = ' ';
}

/* ---- actions ----------------------------------------------------------- */

/* an action waiting for "Yes" in a popup ("confirmation" in the tap action) */
struct ask {
	char domain[32], service[64], entity[96];
	cJSON *data;
};

static void ask_done(void *user, const char *event, const char *button)
{
	struct ask *a = user;

	if (!strcmp(event, "button") && button && !strcmp(button, tr("Yes")))
		ha_call(a->domain, a->service, a->entity[0] ? a->entity : NULL, a->data);
	else
		cJSON_Delete(a->data);
	free(a);
}

/* true: a popup asks first ("confirmation": true or { "text": ... }), and
   the action follows on "Yes" */
static bool ask_first(struct card *c, cJSON *ta, const char *domain, const char *service,
		      const char *entity, cJSON *data)
{
	cJSON *conf = cJSON_GetObjectItemCaseSensitive(ta, "confirmation"), *o, *b;
	const char *text = jstr(conf, "text"), *name = jstr(c->cfg, "name");
	struct entity *e = c->ent[0] ? ent_get(c->ent) : NULL;
	struct ask *a;
	char *json;
	bool ok;

	if (!cJSON_IsTrue(conf) && !cJSON_IsObject(conf))
		return false;
	if (!name && e)
		name = ent_str(e, "friendly_name");
	a = calloc(1, sizeof(*a));
	snprintf(a->domain, sizeof(a->domain), "%s", domain);
	snprintf(a->service, sizeof(a->service), "%s", service);
	snprintf(a->entity, sizeof(a->entity), "%s", entity ? entity : "");
	a->data = data ? cJSON_Duplicate(data, 1) : NULL;
	o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "title", name ? name : tr("Are you sure?"));
	cJSON_AddStringToObject(o, "message", text ? text : name ? tr("Are you sure?") : "");
	cJSON_AddStringToObject(o, "style", "popup");
	cJSON_AddStringToObject(o, "sound", "none");
	b = cJSON_AddArrayToObject(o, "buttons");
	cJSON_AddItemToArray(b, cJSON_CreateString(tr("Yes")));
	cJSON_AddItemToArray(b, cJSON_CreateString(tr("Cancel")));
	json = cJSON_PrintUnformatted(o);
	cJSON_Delete(o);
	ok = json && notify_show(json, ask_done, a);
	free(json);
	if (!ok) {
		cJSON_Delete(a->data);
		free(a);
	}
	return true;
}

static void do_tap(struct card *c)
{
	cJSON *ta = cJSON_GetObjectItemCaseSensitive(c->cfg, "tap_action");
	const char *action = jstr(ta, "action"), *s;
	struct entity *e = c->ent[0] ? ent_get(c->ent) : NULL;
	char domain[32] = "";

	if (c->ent[0])
		domain_of(c->ent, domain, sizeof(domain));
	if (action && !strcmp(action, "none"))
		return;
	if (action && !strcmp(action, "navigate")) {
		const char *path = jstr(ta, "navigation_path");
		int i = 0;
		cJSON *v;

		cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(doc, "views")) {
			const char *vp = jstr(v, "path");

			if (path && vp && strstr(path, vp) && !strcmp(strstr(path, vp), vp))
				dash_show(i);
			i++;
		}
		return;
	}
	if (action && (!strcmp(action, "perform-action") || !strcmp(action, "call-service"))) {
		const char *svc = jstr(ta, "perform_action");
		cJSON *data = cJSON_GetObjectItemCaseSensitive(ta, "data");
		cJSON *target = cJSON_GetObjectItemCaseSensitive(ta, "target");
		const char *ent = jstr(target, "entity_id");
		char d[64];

		if (!svc)
			svc = jstr(ta, "service");
		if (!svc || !strchr(svc, '.'))
			return;
		if (!data)
			data = cJSON_GetObjectItemCaseSensitive(ta, "service_data");
		if (!ent)
			ent = jstr(data, "entity_id");
		domain_of(svc, d, sizeof(d));
		if (ask_first(c, ta, d, strchr(svc, '.') + 1, ent, data))
			return;
		ha_call(d, strchr(svc, '.') + 1, ent, data ? cJSON_Duplicate(data, 1) : NULL);
		return;
	}
	if (!e || !e->state || !strcmp(e->state, "unavailable"))
		return;
	s = e->state;
	if (!strcmp(domain, "light") || !strcmp(domain, "switch") || !strcmp(domain, "fan") ||
	    !strcmp(domain, "input_boolean") || !strcmp(domain, "humidifier") ||
	    !strcmp(domain, "siren") || !strcmp(c->ent, "toon.display") ||
	    (action && !strcmp(action, "toggle"))) {
		ha_call("homeassistant", "toggle", c->ent, NULL);
	} else if (!strcmp(domain, "scene") || !strcmp(domain, "script")) {
		ha_call(domain, "turn_on", c->ent, NULL);
	} else if (!strcmp(domain, "button") || !strcmp(domain, "input_button")) {
		ha_call(domain, "press", c->ent, NULL);
	} else if (!strcmp(domain, "media_player")) {
		ha_call(domain, !strcmp(s, "off") ? "turn_on" : "media_play_pause", c->ent, NULL);
	} else if (!strcmp(c->ent, "toon.thermostat")) {
		ha_call("climate", !strcmp(s, "off") ? "turn_on" : "turn_off", c->ent, NULL);
	}
	/* locks, covers and alarms are not switched by a plain tap */
}

static void on_tap(lv_event_t *ev)
{
	do_tap(lv_event_get_user_data(ev));
}

static void on_slider(lv_event_t *ev)
{
	struct card *c = lv_event_get_user_data(ev);
	int v = lv_slider_get_value(c->slider);
	cJSON *d = cJSON_CreateObject();
	char domain[32];

	domain_of(c->ent, domain, sizeof(domain));
	if (!strcmp(c->feature, "target-temperature")) {
		struct entity *e = ent_get(c->ent);
		double step = ent_num(e, "target_temp_step", 0.5), t;
		char buf[24];

		if (step <= 0)
			step = 0.5;
		t = round(v / 10.0 / step) * step;
		cJSON_AddNumberToObject(d, "temperature", t);
		ha_call("climate", "set_temperature", c->ent, d);
		snprintf(buf, sizeof(buf), "%.1f°", t);
		set_text(c->target, buf);
		cJSON_DeleteItemFromObjectCaseSensitive(e->attr, "temperature");
		cJSON_AddNumberToObject(e->attr, "temperature", t);
		lv_slider_set_value(c->slider, (int)(t * 10 + 0.5), LV_ANIM_OFF);
	} else if (!strcmp(c->feature, "media-player-volume-slider")) {
		cJSON_AddNumberToObject(d, "volume_level", v / 100.0);
		ha_call("media_player", "volume_set", c->ent, d);
	} else if (!strcmp(c->feature, "cover-position")) {
		cJSON_AddNumberToObject(d, "position", v);
		ha_call("cover", "set_cover_position", c->ent, d);
	} else if (!strcmp(c->feature, "fan-speed")) {
		/* 0 % is off, as in Home Assistant */
		cJSON_AddNumberToObject(d, "percentage", v);
		ha_call("fan", "set_percentage", c->ent, d);
	} else if (!v) {
		cJSON_Delete(d);
		ha_call(is_local(c->ent) ? "light" : domain, "turn_off", c->ent, NULL);
	} else {
		cJSON_AddNumberToObject(d, "brightness_pct", v);
		ha_call(is_local(c->ent) ? "light" : domain, "turn_on", c->ent, d);
	}
}

static void on_option(lv_event_t *ev)
{
	struct card *c = lv_event_get_user_data(ev);
	lv_obj_t *btn = lv_event_get_target(ev);
	const char *opt = lv_obj_get_user_data(btn);
	cJSON *d = cJSON_CreateObject();
	char domain[32];

	domain_of(c->ent, domain, sizeof(domain));
	if (!strcmp(c->feature, "climate-hvac-modes")) {
		cJSON_AddStringToObject(d, "hvac_mode", opt);
		ha_call("climate", "set_hvac_mode", c->ent, d);
	} else if (!strcmp(c->feature, "climate-fan-modes")) {
		cJSON_AddStringToObject(d, "fan_mode", opt);
		ha_call("climate", "set_fan_mode", c->ent, d);
	} else if (!strcmp(c->feature, "climate-swing-modes")) {
		cJSON_AddStringToObject(d, "swing_mode", opt);
		ha_call("climate", "set_swing_mode", c->ent, d);
	} else if (!strcmp(c->feature, "cover-open-close")) {
		cJSON_Delete(d);
		ha_call("cover", !strcmp(opt, "open") ? "open_cover" :
			!strcmp(opt, "close") ? "close_cover" : "stop_cover", c->ent, NULL);
	} else if (!strcmp(c->feature, "fan-preset-modes") ||
		   (!strcmp(c->feature, "climate-preset-modes") && !strcmp(domain, "fan"))) {
		cJSON_AddStringToObject(d, "preset_mode", opt);
		ha_call("fan", "set_preset_mode", c->ent, d);
	} else if (!strcmp(c->feature, "climate-preset-modes")) {
		struct entity *e = ent_get(c->ent);
		const char *cur = ent_str(e, "preset_mode");
		int hold = ent_num(e, "hold", 0);
		bool same = cur && !strcmp(cur, opt);

		cJSON_AddStringToObject(d, "preset_mode", opt);
		/* ReToon's own with the week program running: the preset chosen
		   by hand (orange) tapped again is locked (red), a locked one
		   tapped again follows the program again */
		if (!strcmp(c->ent, "toon.thermostat") && ent_num(e, "program", 0) && same && hold) {
			ha_call("climate", hold == 2 ? "retoon_resume_program" : "retoon_lock_preset",
				c->ent, d);
			return;
		}
		ha_call("climate", "set_preset_mode", c->ent, d);
	} else {
		cJSON_AddStringToObject(d, "option", opt);
		ha_call(is_local(c->ent) ? "select" : domain, "select_option", c->ent, d);
	}
}

static void on_step(lv_event_t *ev)
{
	struct card *c = lv_event_get_user_data(ev);
	struct entity *e = ent_get(c->ent);
	int dir = (intptr_t)lv_obj_get_user_data(lv_event_get_target(ev));
	double step = ent_num(e, "target_temp_step", 0.5);
	double t = ent_num(e, "temperature", 20) + dir * step;
	cJSON *d = cJSON_CreateObject();
	char buf[24];

	t = LV_CLAMP(ent_num(e, "min_temp", 5), t, ent_num(e, "max_temp", 35));
	cJSON_AddNumberToObject(d, "temperature", t);
	ha_call("climate", "set_temperature", c->ent, d);
	/* show it at once; the real value follows from the entity */
	snprintf(buf, sizeof(buf), "%.1f°", t);
	set_text(c->target, buf);
	cJSON_DeleteItemFromObjectCaseSensitive(e->attr, "temperature");
	cJSON_AddNumberToObject(e->attr, "temperature", t);
}

/*
 * Only the knob turns the dial. A touch elsewhere on the ring (a brush of
 * the hand, cleaning the screen) would otherwise move the target there at
 * once, up to 30 degrees at the bottom right.
 */
static void on_arc_press(lv_event_t *ev)
{
	struct card *c = lv_event_get_user_data(ev);
	lv_obj_t *a = c->arc;
	lv_indev_t *in = lv_indev_active();
	lv_point_t p;
	lv_area_t co;
	int32_t pl = lv_obj_get_style_pad_left(a, LV_PART_MAIN);
	int32_t pt = lv_obj_get_style_pad_top(a, LV_PART_MAIN);
	int32_t r = (LV_MIN(lv_obj_get_width(a) - pl - lv_obj_get_style_pad_right(a, LV_PART_MAIN),
			    lv_obj_get_height(a) - pt - lv_obj_get_style_pad_bottom(a, LV_PART_MAIN))) / 2;
	int32_t aw = lv_obj_get_style_arc_width(a, LV_PART_INDICATOR);
	double ang = lv_arc_get_angle_end(a) * M_PI / 180, kx, ky, reach = LV_MAX(aw * 2, S(34));

	c->arc_from = -1;
	if (!in)
		return;
	lv_indev_get_point(in, &p);
	lv_obj_get_coords(a, &co);
	kx = co.x1 + pl + r + (r - aw / 2) * cos(ang);
	ky = co.y1 + pt + r + (r - aw / 2) * sin(ang);
	if ((p.x - kx) * (p.x - kx) + (p.y - ky) * (p.y - ky) > reach * reach) {
		lv_indev_wait_release(in);	/* not the knob: nothing turns */
		return;
	}
	c->arc_from = lv_arc_get_value(a);
}

static void on_arc(lv_event_t *ev);

static void arc_events(struct card *c)
{
	c->arc_from = -1;
	lv_obj_add_flag(c->arc, LV_OBJ_FLAG_ADV_HITTEST);	/* only the ring takes touches */
	lv_obj_remove_flag(c->arc, LV_OBJ_FLAG_GESTURE_BUBBLE);	/* dragging it is no page swipe */
	lv_obj_add_event_cb(c->arc, on_arc_press, LV_EVENT_PRESSED, c);
	lv_obj_add_event_cb(c->arc, on_arc, LV_EVENT_VALUE_CHANGED, c);
	lv_obj_add_event_cb(c->arc, on_arc, LV_EVENT_RELEASED, c);
}

/* the dial is turned: show the target while it moves, set it on release */
static void on_arc(lv_event_t *ev)
{
	struct card *c = lv_event_get_user_data(ev);
	struct entity *e = ent_get(c->ent);
	double step = ent_num(e, "target_temp_step", 0.5);
	double t = lv_arc_get_value(c->arc) / 10.0;
	char buf[24];

	if (step <= 0)
		step = 0.5;
	t = round(t / step) * step;
	snprintf(buf, sizeof(buf), "%.1f°", t);
	set_text(c->target, buf);
	if (lv_event_get_code(ev) == LV_EVENT_RELEASED) {
		cJSON *d;
		int from = c->arc_from;

		c->arc_from = -1;
		/* the knob only tapped, or let go where it was: nothing to set
		   (a target, even the same one, would leave the week program) */
		if (from < 0 || (int)(t * 10 + 0.5) == (int)(round(from / 10.0 / step) * step * 10 + 0.5))
			return;
		d = cJSON_CreateObject();

		cJSON_AddNumberToObject(d, "temperature", t);
		ha_call("climate", "set_temperature", c->ent, d);
		cJSON_DeleteItemFromObjectCaseSensitive(e->attr, "temperature");
		cJSON_AddNumberToObject(e->attr, "temperature", t);
		lv_arc_set_value(c->arc, (int)(t * 10 + 0.5));
	}
}

/* ---- card parts -------------------------------------------------------- */

static lv_obj_t *small_button(lv_obj_t *parent, const char *label, const lv_font_t *font)
{
	lv_obj_t *b = lv_button_create(parent);
	lv_obj_t *l = text(b, font, C_TEXT, label);

	lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD_HI), 0);
	lv_obj_set_style_bg_color(b, lv_color_hex(C_ACCENT), LV_STATE_CHECKED);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_set_style_radius(b, S(10), 0);
	lv_obj_set_style_pad_all(b, 0, 0);
	lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
	lv_obj_center(l);
	return b;
}

/* A row of buttons, one per option; rebuilt only when the options change. */
/* Home Assistant's "none" preset (no preset) gets no button of its own:
   changing the temperature leaves a preset anyway */
static bool skip_option(struct card *c, cJSON *o)
{
	return !cJSON_IsString(o) ||
	       (!strcmp(c->feature, "climate-preset-modes") && !strcmp(o->valuestring, "none"));
}

/* the key in the feature's settings that lists which options get a button,
   and in which order, as in Home Assistant */
static const char *feature_list_key(const char *feature)
{
	return !strcmp(feature, "climate-hvac-modes") ? "hvac_modes" :
	       !strcmp(feature, "climate-preset-modes") ? "preset_modes" :
	       !strcmp(feature, "fan-preset-modes") ? "preset_modes" :
	       !strcmp(feature, "climate-fan-modes") ? "fan_modes" :
	       !strcmp(feature, "climate-swing-modes") ? "swing_modes" : "options";
}

#define MAX_OPTIONS	5

/* the options to make buttons for: the ones the card lists that the entity
   has (all it lists while the entity does not say), else all the entity's */
static int pick_options(struct card *c, cJSON *options, const char **out)
{
	cJSON *want = c->feat ? cJSON_GetObjectItemCaseSensitive(c->feat, feature_list_key(c->feature)) : NULL;
	cJSON *o, *w;
	int n = 0;

	if (cJSON_IsArray(want) && cJSON_GetArraySize(want)) {
		cJSON_ArrayForEach(w, want) {
			bool have = !cJSON_GetArraySize(options);

			if (!cJSON_IsString(w) || n >= MAX_OPTIONS)
				continue;
			cJSON_ArrayForEach(o, options)
				if (cJSON_IsString(o) && !strcmp(o->valuestring, w->valuestring))
					have = true;
			if (have)
				out[n++] = w->valuestring;
		}
		return n;
	}
	/* more modes than fit: "off" stays, in place of the last one that fits */
	int total = 0;
	bool off = false;

	cJSON_ArrayForEach(o, options)
		if (!skip_option(c, o)) {
			total++;
			off |= !strcmp(o->valuestring, "off");
		}
	off = off && total > MAX_OPTIONS && !strcmp(c->feature, "climate-hvac-modes");
	cJSON_ArrayForEach(o, options)
		if (!skip_option(c, o) && n < MAX_OPTIONS - off && !(off && !strcmp(o->valuestring, "off")))
			out[n++] = o->valuestring;
	if (off)
		out[n++] = "off";
	return n;
}

/* cover-open-close: open, stop, close (as Home Assistant's feature) */
static cJSON *cover_buttons(void)
{
	static cJSON *l;

	if (!l)
		l = cJSON_CreateStringArray((const char *[]){ "open", "stop", "close" }, 3);
	return l;
}

static void free_user_data(lv_event_t *ev)
{
	free(lv_obj_get_user_data(lv_event_get_target(ev)));
}

static void set_options(struct card *c, cJSON *options, const char *current)
{
	const char *list[MAX_OPTIONS];
	char sig[sizeof(c->sig)] = "";
	int n = pick_options(c, options, list), w;

	for (int i = 0; i < n; i++)
		if (strlen(sig) + strlen(list[i]) + 2 < sizeof(sig)) {
			strcat(sig, list[i]);
			strcat(sig, "|");
		}
	if (strcmp(sig, c->sig)) {
		snprintf(c->sig, sizeof(c->sig), "%s", sig);
		lv_obj_clean(c->btns);
		w = n ? (lv_obj_get_width(c->btns) - (n - 1) * 8) / n : 0;
		if (c->classic && n) {	/* two a row (three of 3 or 5), rows from the bottom */
			int cols = n == 4 ? 2 : LV_MIN(n, 3);

			w = (lv_obj_get_width(c->btns) - (cols - 1) * S(8)) / cols;
		}
		for (int i = 0; i < n; i++) {
			lv_obj_t *b;
			char label[48];

			snprintf(label, sizeof(label), "%s", list[i]);
			if (!strncmp(c->feature, "climate-", 8) && state_text(list[i])) {
				snprintf(label, sizeof(label), "%s", state_text(list[i]));
			} else if (!strncmp(c->feature, "climate-", 8)) {
				label[0] = toupper(label[0]);
				for (char *p = label; *p; p++)
					if (*p == '_')
						*p = ' ';
			}
			if (!strcmp(c->feature, "cover-open-close"))
				b = small_button(c->btns, mdi(!strcmp(list[i], "open") ? "mdi:arrow-up" :
							      !strcmp(list[i], "close") ? "mdi:arrow-down" :
							      "mdi:stop", list[i]), i_small);
			else
				b = small_button(c->btns, label, f_small);
			lv_obj_set_size(b, w, c->classic ? (lv_obj_get_height(c->btns) - S(8)) / 2 : lv_obj_get_height(c->btns));
			if (c->arc)
				lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
			lv_obj_set_width(lv_obj_get_child(b, 0), w - S(8));
			lv_label_set_long_mode(lv_obj_get_child(b, 0), LV_LABEL_LONG_CLIP);
			lv_obj_set_style_text_align(lv_obj_get_child(b, 0), LV_TEXT_ALIGN_CENTER, 0);
			/* a copy: the entity's attributes are replaced with every change */
			lv_obj_set_user_data(b, strdup(list[i]));
			lv_obj_add_event_cb(b, free_user_data, LV_EVENT_DELETE, NULL);
			lv_obj_add_event_cb(b, on_option, LV_EVENT_CLICKED, c);
		}
	}
	for (uint32_t k = 0; k < lv_obj_get_child_count(c->btns); k++) {
		lv_obj_t *b = lv_obj_get_child(c->btns, k);
		const char *v = lv_obj_get_user_data(b);

		bool on = current && v && !strcmp(v, current);

		lv_obj_set_state(b, LV_STATE_CHECKED, on);
		/* a thermostat's chosen preset or mode: dark blue with light blue
		   text, calmer than the full accent of other buttons */
		if (!strncmp(c->feature, "climate-", 8)) {
			lv_obj_set_style_bg_color(b, lv_color_hex(C_SEL), LV_STATE_CHECKED);
			lv_obj_set_style_text_color(lv_obj_get_child(b, 0),
						    lv_color_hex(on ? C_SEL_TEXT : C_TEXT), 0);
		}
	}
	/* ReToon's own thermostat with the week program running: a border on
	   the active preset when it was chosen by hand, orange until the
	   program's next step, red when locked */
	if ((c->arc || c->classic) && !strcmp(c->ent, "toon.thermostat")) {
		struct entity *e = ent_get(c->ent);
		int hold = ent_num(e, "program", 0) ? ent_num(e, "hold", 0) : 0;

		for (uint32_t k = 0; k < lv_obj_get_child_count(c->btns); k++) {
			lv_obj_t *b = lv_obj_get_child(c->btns, k);

			lv_obj_set_style_border_color(b, lv_color_hex(hold == 2 ? C_BAD : 0xff9800),
						      LV_STATE_CHECKED);
			lv_obj_set_style_border_width(b, hold ? S(3) : 0, LV_STATE_CHECKED);
		}
	}
}

static void gauge_update(struct card *c, struct entity *e);

static void card_update(void *user)
{
	struct card *c = user;
	struct entity *e = ent_get(c->ent);
	const char *name = jstr(c->cfg, "name"), *icon = jstr(c->cfg, "icon");
	char domain[32], buf[160];
	uint32_t col;

	if (c->gauge) {		/* no icon, no buttons: its own */
		gauge_update(c, e);
		return;
	}
	domain_of(c->ent, domain, sizeof(domain));
	if (!name)
		name = ent_str(e, "friendly_name");
	set_text(c->name, name ? name : c->ent);
	if (!icon)
		icon = ent_str(e, "icon");
	if (!icon)
		icon = default_icon(e, domain);
	set_text(c->icon, mdi(icon, "mdi:eye"));
	col = state_color(c, e, domain);
	if (c->icon)		/* a small dial without its name has none */
		lv_obj_set_style_text_color(c->icon, lv_color_hex(col), 0);
	/* a tile's own background while it is on ("Colour when on"; our option) */
	if (!c->row && c->root && jstr(c->cfg, "on_color"))
		lv_obj_set_style_bg_color(c->root, lv_color_hex(is_active(domain, e->state) ?
				dash_color(jstr(c->cfg, "on_color"), C_CARD) : C_CARD), 0);
	if (c->icon_bg)
		lv_obj_set_style_bg_color(c->icon_bg, lv_color_hex(col), 0);
	if (c->state) {
		fmt_state(e, domain, buf, sizeof(buf));
		set_text(c->state, buf);
	}

	if (c->slider && !lv_slider_is_dragged(c->slider) && !strcmp(c->feature, "target-temperature")) {
		double lo = ent_num(e, "min_temp", 5), hi = ent_num(e, "max_temp", 35);

		if (hi > lo) {
			lv_slider_set_range(c->slider, (int)(lo * 10), (int)(hi * 10));
			lv_slider_set_value(c->slider, (int)(ent_num(e, "temperature", lo) * 10 + 0.5), LV_ANIM_OFF);
		}
	} else if (c->slider && !lv_slider_is_dragged(c->slider)) {
		int v = 0;

		if (!strcmp(c->feature, "media-player-volume-slider"))
			v = ent_num(e, "volume_level", 0) * 100 + 0.5;
		else if (!strcmp(c->feature, "cover-position"))
			v = ent_num(e, "current_position", 0) + 0.5;
		else if (!strcmp(c->feature, "fan-speed"))
			v = e->state && !strcmp(e->state, "on") ? ent_num(e, "percentage", 0) + 0.5 : 0;
		else if (e->state && !strcmp(e->state, "on"))
			v = ent_num(e, "brightness", 255) * 100 / 255 + 0.5;
		lv_slider_set_value(c->slider, v, LV_ANIM_OFF);
	}
	if (c->arc || c->classic) {
		/* a thermostat with presets shows those; the others their modes */
		cJSON *pm = cJSON_GetObjectItemCaseSensitive(e->attr, "preset_modes");

		if (!c->feat)
			c->feature = cJSON_GetArraySize(pm) ? "climate-preset-modes" : "climate-hvac-modes";
		set_text(c->info, ent_str(e, "status_text") ? ent_str(e, "status_text") : "");
		/* ReToon's own with the week program running: chosen by hand until
		   the program's next step (hourglass) or locked (lock) */
		if (c->info_icon) {
			int hold = ent_num(e, "program", 0) ? ent_num(e, "hold", 0) : 0;

			set_text(c->info_icon, hold == 2 ? mdi("mdi:lock", "") :
				 hold == 1 ? mdi("mdi:timer-sand", "") : "");
			if (hold)
				lv_obj_remove_flag(c->info_icon, LV_OBJ_FLAG_HIDDEN);
			else
				lv_obj_add_flag(c->info_icon, LV_OBJ_FLAG_HIDDEN);
			/* locked: the ring and − / + are greyed out and take no touch */
			if (c->arc)
				lv_obj_set_state(c->arc, LV_STATE_DISABLED, hold == 2);
			if (c->target) {
				lv_obj_t *st = c->steps ? c->steps : lv_obj_get_parent(c->target);

				for (int k = 0; k < 2; k++)
					lv_obj_set_state(lv_obj_get_child(st, k), LV_STATE_DISABLED, hold == 2);
				lv_obj_set_style_text_opa(c->target, hold == 2 ? LV_OPA_50 : LV_OPA_COVER, 0);
			}
		}
		if (c->prog) {
			bool on = ent_num(e, "program_enabled", 0);

			/* one icon; blue when the program is on */
			lv_obj_set_state(c->prog, LV_STATE_CHECKED, on);
		}
	}
	if (c->btns && c->feature) {
		if (!strcmp(c->feature, "climate-preset-modes") || !strcmp(c->feature, "fan-preset-modes"))
			set_options(c, cJSON_GetObjectItemCaseSensitive(e->attr, "preset_modes"),
				    ent_str(e, "preset_mode"));
		else if (!strcmp(c->feature, "climate-hvac-modes"))
			set_options(c, cJSON_GetObjectItemCaseSensitive(e->attr, "hvac_modes"), e->state);
		else if (!strcmp(c->feature, "climate-fan-modes"))
			set_options(c, cJSON_GetObjectItemCaseSensitive(e->attr, "fan_modes"),
				    ent_str(e, "fan_mode"));
		else if (!strcmp(c->feature, "climate-swing-modes"))
			set_options(c, cJSON_GetObjectItemCaseSensitive(e->attr, "swing_modes"),
				    ent_str(e, "swing_mode"));
		else if (!strcmp(c->feature, "cover-open-close"))
			set_options(c, cover_buttons(), NULL);
		else
			set_options(c, cJSON_GetObjectItemCaseSensitive(e->attr, "options"), e->state);
	}
	if (c->big) {
		if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(e->attr, "current_temperature")))
			snprintf(buf, sizeof(buf), "%.1f°", ent_num(e, "current_temperature", 0));
		else
			snprintf(buf, sizeof(buf), "–");
		set_text(c->big, buf);
	}
	if (c->target) {
		if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(e->attr, "temperature")))
			snprintf(buf, sizeof(buf), "%.1f°", ent_num(e, "temperature", 0));
		else
			snprintf(buf, sizeof(buf), "–");
		set_text(c->target, buf);
	}
	if (c->action) {
		const char *a = ent_str(e, "hvac_action"), *s = e->state ? e->state : "";
		bool heating = a && !strcmp(a, "heating") && !is_unknown(e->state);
		bool cooling = a && !strcmp(a, "cooling") && !is_unknown(e->state);
		bool off = is_unknown(e->state) || !strcmp(s, "off");
		const char *side = ent_str(e, "season");
		bool sides = side && side[0];
		uint32_t tint = heating ? 0xff7a2f : cooling ? 0x35c6e8 : off ? 0x4a5262 : C_ACCENT;

		if (is_unknown(e->state))
			snprintf(buf, sizeof(buf), "%s", tr(!strcmp(c->ent, "toon.thermostat") ?
				 "No boiler module" : "Unavailable"));
		else if (heating)
			snprintf(buf, sizeof(buf), "%s", tr("Heating"));
		else if (!strcmp(s, "off"))
			snprintf(buf, sizeof(buf), "%s", tr("Off"));
		else if (cooling)
			snprintf(buf, sizeof(buf), "%s", tr("Cooling"));
		else
			snprintf(buf, sizeof(buf), "%s", tr("Idle"));
		/* the classic style has no corner for it: no boiler module (or
		   the external device) in place of the word, unless it runs */
		if (c->classic && ent_num(e, "no_boiler", 0) && !heating && !cooling && !off)
			snprintf(buf, sizeof(buf), "%s",
				 tr(ent_num(e, "external", 0) ? "External device" : "No boiler module"));
		set_text(c->action, buf);
		lv_obj_set_style_text_color(c->action, lv_color_hex(heating || cooling ? tint : C_MUTED), 0);
		/* top right: no boiler module (or the external device in its
		   place), or where cooling is in use the mode */
		if (c->mode)
			set_text(c->mode, ent_num(e, "no_boiler", 0) ?
				 tr(ent_num(e, "external", 0) ? "External device" : "No boiler module") :
				 !sides ? "" : tr(strcmp(side, "cool") ? "Mode: heating" : "Mode: cooling"));
		if (c->act_icon && c->classic) {
			/* by the target: a flame (snowflake) that lights up while it runs */
			set_text(c->act_icon, mdi(cooling ? "mdi:snowflake" : "mdi:fire", ""));
			lv_obj_set_style_text_color(c->act_icon, lv_color_hex(heating || cooling ? tint : 0x4a5262), 0);
		} else if (c->act_icon) {
			set_text(c->act_icon, mdi(heating ? "mdi:fire" : cooling ? "mdi:snowflake" :
						  is_unknown(e->state) ? "mdi:alert-circle-outline" :
						  off ? "mdi:power" : "mdi:check-circle-outline", ""));
			lv_obj_set_style_text_color(c->act_icon, lv_color_hex(heating || cooling ? tint : C_MUTED), 0);
		}
		if (c->slider && !strcmp(c->feature, "target-temperature"))
			lv_obj_set_style_bg_color(c->slider, lv_color_hex(off ? 0x4a5262 : tint), LV_PART_INDICATOR);
		if (c->arc) {
			double lo = ent_num(e, "min_temp", 5), hi = ent_num(e, "max_temp", 35);
			cJSON *cur_t = cJSON_GetObjectItemCaseSensitive(e->attr, "current_temperature");

			lv_obj_set_style_arc_color(c->arc, lv_color_hex(tint), LV_PART_INDICATOR);
			lv_obj_set_style_bg_color(c->arc, lv_color_hex(off ? 0x8c96a8 : tint), LV_PART_KNOB);
			if (hi > lo && !lv_obj_has_state(c->arc, LV_STATE_PRESSED)) {
				lv_arc_set_range(c->arc, (int)(lo * 10), (int)(hi * 10));
				lv_arc_set_value(c->arc, (int)(ent_num(e, "temperature", lo) * 10 + 0.5));
			}
			/* a short bar outside the ring where the room is now */
			if (cJSON_IsNumber(cur_t) && hi > lo) {
				double f = LV_CLAMP(0, (cur_t->valuedouble - lo) / (hi - lo), 1);
				double ang = (135 + f * 270) * M_PI / 180;
				lv_area_t a, d;
				int r;

				lv_obj_update_layout(c->root);
				lv_obj_get_coords(c->arc, &a);
				lv_obj_get_coords(c->dot, &d);
				r = lv_area_get_width(&a) / 2;
				for (int k = 0; k < 2; k++) {
					int rk = r + (k ? S(7) : -S(6));

					c->tick[k].x = (a.x1 + a.x2) / 2 - d.x1 + cos(ang) * rk;
					c->tick[k].y = (a.y1 + a.y2) / 2 - d.y1 + sin(ang) * rk;
				}
				lv_line_set_points(c->dot, c->tick, 2);
				lv_obj_remove_flag(c->dot, LV_OBJ_FLAG_HIDDEN);
			} else {
				lv_obj_add_flag(c->dot, LV_OBJ_FLAG_HIDDEN);
			}
		}
	}
}

static struct card *new_card(cJSON *cfg, const char *entity)
{
	struct card *c = calloc(1, sizeof(*c));

	c->cfg = cfg;
	if (entity)
		snprintf(c->ent, sizeof(c->ent), "%s", entity);
	c->next = cards;
	cards = c;
	return c;
}

static void finish(struct card *c)
{
	if (!c->ent[0])
		return;
	ent_bind(c->ent, card_update, c);
	card_update(c);
}

static lv_obj_t *add_slider(struct card *c, lv_obj_t *parent, int w)
{
	lv_obj_t *s = lv_slider_create(parent);

	lv_obj_set_size(s, w - S(28), S(30));
	lv_obj_set_style_radius(s, S(10), 0);
	lv_obj_set_style_radius(s, S(10), LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(s, lv_color_hex(C_CARD_HI), 0);
	lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(s, lv_color_hex(!strcmp(c->feature, "light-brightness") ?
						  0xffc107 : C_ACCENT), LV_PART_INDICATOR);
	lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, LV_PART_KNOB);
	lv_obj_set_style_pad_all(s, 0, LV_PART_KNOB);
	lv_obj_set_ext_click_area(s, S(10));
	/* dragging it must not count as a page swipe */
	lv_obj_remove_flag(s, LV_OBJ_FLAG_GESTURE_BUBBLE);
	lv_slider_set_range(s, 0, 100);
	lv_obj_add_event_cb(s, on_slider, LV_EVENT_RELEASED, c);
	c->slider = s;
	return s;
}

static lv_obj_t *add_stepper(struct card *c, lv_obj_t *parent, int w, int h, const lv_font_t *font)
{
	lv_obj_t *r = box(parent, w, h), *b;

	for (int dir = -1; dir <= 1; dir += 2) {
		b = small_button(r, dir < 0 ? "−" : "+", f_mid);
		lv_obj_set_size(b, h + S(8), h);
		lv_obj_align(b, dir < 0 ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID, 0, 0);
		lv_obj_set_user_data(b, (void *)(intptr_t)dir);
		lv_obj_add_event_cb(b, on_step, LV_EVENT_CLICKED, c);
	}
	c->target = text(r, font, C_TEXT, "–");
	lv_obj_center(c->target);
	return r;
}

/* ---- cards ------------------------------------------------------------- */

static bool known_feature(const char *t)
{
	return t && (!strcmp(t, "light-brightness") || !strcmp(t, "media-player-volume-slider") ||
		     !strcmp(t, "cover-position") || !strcmp(t, "cover-open-close") ||
		     !strcmp(t, "target-temperature") || !strcmp(t, "select-options") ||
		     !strcmp(t, "climate-hvac-modes") || !strcmp(t, "climate-preset-modes") ||
		     !strcmp(t, "climate-fan-modes") || !strcmp(t, "climate-swing-modes") ||
		     !strcmp(t, "fan-preset-modes") || !strcmp(t, "fan-speed"));
}

/* a tile's own text size ("small", "normal", "large", "x-large"; our own option) */
static lv_font_t *size_font(const char *size, lv_font_t *def)
{
	if (!size)
		return def;
	if (!strcmp(size, "small"))
		return f_small;
	if (!strcmp(size, "large"))
		return f_head;
	if (!strcmp(size, "x-large"))
		return f_mid;
	return def;
}

static lv_obj_t *tile(lv_obj_t *parent, cJSON *cfg, const char *entity, int w)
{
	struct card *c = new_card(cfg, entity);
	cJSON *f, *hide = cJSON_GetObjectItemCaseSensitive(cfg, "hide_state");
	int h = TILE_H, y = TILE_H - 4;
	int dy = (TILE_H - S(66)) / 2;	/* laid out for 66: the middle of the real height */
	lv_obj_t *o;

	cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(cfg, "features"))
		if (known_feature(jstr(f, "type")) && !c->feature) {
			c->feature = jstr(f, "type");
			c->feat = f;
			h += FEAT_H - 4;
		}
	c->root = panel(parent, w, h);
	lv_obj_add_flag(c->root, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_bg_color(c->root, lv_color_hex(C_CARD_HI), LV_STATE_PRESSED);
	lv_obj_add_event_cb(c->root, on_tap, LV_EVENT_CLICKED, c);

	c->icon_bg = box(c->root, S(44), S(44));
	lv_obj_set_pos(c->icon_bg, S(11), S(11) + dy);
	lv_obj_set_style_radius(c->icon_bg, S(22), 0);
	lv_obj_set_style_bg_opa(c->icon_bg, 50, 0);
	/* a plain object takes taps itself: let them reach the card (a 1x1
	   tile is almost all icon) */
	lv_obj_remove_flag(c->icon_bg, LV_OBJ_FLAG_CLICKABLE);
	c->icon = text(c->icon_bg, i_small, C_MUTED, "");
	lv_obj_center(c->icon);

	/* the name and the state: their own size and colour when chosen */
	lv_font_t *fn = size_font(jstr(cfg, "name_size"), f_body);
	lv_font_t *fs = size_font(jstr(cfg, "state_size"), f_small);
	uint32_t cn = dash_color(jstr(cfg, "name_color"), C_TEXT);
	uint32_t cs = dash_color(jstr(cfg, "state_color"), C_MUTED);

	c->name = text(c->root, fn, cn, "");
	lv_label_set_long_mode(c->name, LV_LABEL_LONG_DOT);
	if (!cJSON_IsTrue(hide)) {
		c->state = text(c->root, fs, cs, "");
		lv_label_set_long_mode(c->state, LV_LABEL_LONG_DOT);
	}
	if (fn == f_body && fs == f_small) {
		lv_obj_set_size(c->name, w - S(76), S(24));
		lv_obj_set_pos(c->name, S(66), (cJSON_IsTrue(hide) ? S(21) : S(10)) + dy);
		if (c->state) {
			lv_obj_set_size(c->state, w - S(76), S(20));
			lv_obj_set_pos(c->state, S(66), S(36) + dy);
		}
	} else {
		/* other sizes: the two lines together in the middle of the tile */
		int hn = lv_font_get_line_height(fn), hs = c->state ? lv_font_get_line_height(fs) : 0;
		int top = LV_MAX(0, (TILE_H - hn - hs) / 2);

		lv_obj_set_size(c->name, w - S(76), hn);
		lv_obj_set_pos(c->name, S(66), top);
		if (c->state) {
			lv_obj_set_size(c->state, w - S(76), hs);
			lv_obj_set_pos(c->state, S(66), top + hn);
		}
	}

	if (c->feature) {
		if (!strcmp(c->feature, "light-brightness") ||
		    !strcmp(c->feature, "media-player-volume-slider") ||
		    !strcmp(c->feature, "cover-position") || !strcmp(c->feature, "fan-speed")) {
			o = add_slider(c, c->root, w);
			lv_obj_set_pos(o, S(14), y + S(4));
		} else if (!strcmp(c->feature, "target-temperature")) {
			o = add_stepper(c, c->root, w - S(24), S(38), f_head);
			lv_obj_set_pos(o, S(12), y);
		} else {
			c->btns = box(c->root, w - S(24), S(38));
			lv_obj_set_pos(c->btns, S(12), y);
			lv_obj_set_flex_flow(c->btns, LV_FLEX_FLOW_ROW);
			lv_obj_set_style_pad_column(c->btns, S(8), 0);
			lv_obj_update_layout(c->btns);
		}
	}
	finish(c);
	return c->root;
}

static lv_obj_t *heading(lv_obj_t *parent, cJSON *cfg, int w)
{
	const char *style = jstr(cfg, "heading_style"), *icon = jstr(cfg, "icon");
	const char *title = jstr(cfg, "heading");
	bool sub = style && !strcmp(style, "subtitle");
	lv_obj_t *o = box(parent, w, sub ? S(28) : S(40)), *l;
	int x = 4;

	if (icon) {
		l = text(o, i_small, C_MUTED, mdi(icon, ""));
		lv_obj_align(l, LV_ALIGN_LEFT_MID, x, 2);
		x += S(34);
	}
	l = text(o, sub ? f_small : f_head, sub ? C_MUTED : C_TEXT, title ? title : jstr(cfg, "title"));
	lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
	lv_obj_set_width(l, w - x - S(4));
	lv_obj_align(l, LV_ALIGN_LEFT_MID, x, 2);
	return o;
}

/*
 * The thermostat: a dial. The ring and its knob are the temperature asked
 * for (turn it, or use the round - and + under the ends of the ring), the
 * large number is the room now, with a yellow bar outside the ring at that
 * temperature. The ring and knob are orange while heating, blue when not,
 * grey when off.
 */
/* the height a thermostat card should get to end level with the cards
   next to it; 0: its own height */
static int fit_h;

/* the height of the thermostat card with a ring of this size, with or
   without the row of buttons under it */
static int dial_height(int dia, bool row)
{
	int bh = dia < S(260) ? S(48) : S(54);

	return S(46) + dia / 2 + (int)(dia / 2 * 0.707) + S(14) + bh + S(16) +
	       (row ? S(44) + S(16) : 0);
}

/* a thermostat card's own height: a whole number of tile rows, so it lines
   up with tiles next to it, and every thermostat is the same size; counted
   with the button row also when it is hidden, so the dial gets that room */
static int thermostat_height(int w)
{
	int dia = LV_CLAMP(S(190), w - S(150), S(330));
	int rows = (dial_height(dia, true) + GAP) / (TILE_H + GAP);

	if (rows > 0 && rows * (TILE_H + GAP) - GAP >= dial_height(S(190), true))
		return rows * (TILE_H + GAP) - GAP;
	return dial_height(dia, true);
}

/* the week program on or off, on ReToon's own thermostat */
static void on_prog(lv_event_t *ev)
{
	struct card *c = lv_event_get_user_data(ev);

	ha_call("climate", ent_num(ent_get(c->ent), "program_enabled", 0) ?
		"retoon_program_off" : "retoon_program_on", c->ent, cJSON_CreateObject());
}

static lv_obj_t *thermostat(lv_obj_t *parent, cJSON *cfg, const char *entity, int w)
{
	struct card *c = new_card(cfg, entity);
	cJSON *f;
	bool with_row = thermostat_buttons(cfg, entity);
	int dia = LV_CLAMP(S(190), w - S(150), S(330)), h, top = S(46);
	int want = fit_h ? fit_h : thermostat_height(w);
	/* smaller to fit, larger is not needed */
	while (want && dia > S(190) && dial_height(dia, with_row) > want)
		dia -= 2;
	h = LV_MAX(dial_height(dia, with_row), want);
	top += (h - dial_height(dia, with_row)) / 2;	/* what is left: around the dial */

	int cy = top + dia / 2;
	int gap = (int)(dia / 2 * 0.707);	/* where the open part of the ring starts */
	bool small = dia < S(260);
	int bh = small ? S(48) : S(54);		/* the round - and + */
	int sy = cy + gap + S(14);		/* their top: just under the ring's ends */
	int py = h - S(44) - S(16);		/* the presets, at the bottom */
	lv_obj_t *o, *l, *row, *b;

	c->root = panel(parent, w, h);
	c->icon = text(c->root, i_small, C_MUTED, "");
	lv_obj_set_pos(c->icon, S(16), S(14));
	c->name = text(c->root, f_body, C_TEXT, "");
	lv_label_set_long_mode(c->name, LV_LABEL_LONG_DOT);
	lv_obj_set_size(c->name, w / 2 - S(60), S(24));
	lv_obj_set_pos(c->name, S(52), S(15));
	c->mode = text(c->root, f_small, C_MUTED, "");
	lv_obj_set_style_text_align(c->mode, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_set_size(c->mode, w / 2 - S(8), S(22));
	lv_obj_align(c->mode, LV_ALIGN_TOP_RIGHT, -S(16), S(18));

	c->arc = lv_arc_create(c->root);
	lv_obj_set_size(c->arc, dia, dia);
	lv_obj_align(c->arc, LV_ALIGN_TOP_MID, 0, top);
	lv_arc_set_bg_angles(c->arc, 135, 45);
	lv_obj_set_style_arc_width(c->arc, S(12), 0);
	lv_obj_set_style_arc_width(c->arc, S(12), LV_PART_INDICATOR);
	lv_obj_set_style_arc_color(c->arc, lv_color_hex(C_CARD_HI), 0);
	lv_obj_set_style_bg_color(c->arc, lv_color_hex(C_ACCENT), LV_PART_KNOB);
	lv_obj_set_style_border_width(c->arc, S(3), LV_PART_KNOB);
	lv_obj_set_style_border_color(c->arc, lv_color_hex(0xffffff), LV_PART_KNOB);
	lv_obj_set_style_pad_all(c->arc, S(5), LV_PART_KNOB);
	lv_obj_set_style_shadow_width(c->arc, S(10), LV_PART_KNOB);
	lv_obj_set_style_shadow_color(c->arc, lv_color_hex(0x000000), LV_PART_KNOB);
	lv_obj_set_style_shadow_opa(c->arc, 110, LV_PART_KNOB);
	arc_events(c);

	/* covers the whole card, so the bar can be anywhere around the ring */
	c->dot = lv_line_create(c->root);
	lv_obj_set_pos(c->dot, 0, 0);
	lv_obj_set_size(c->dot, LV_PCT(100), LV_PCT(100));
	lv_obj_remove_flag(c->dot, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_line_width(c->dot, S(5), 0);
	lv_obj_set_style_line_rounded(c->dot, true, 0);
	lv_obj_set_style_line_color(c->dot, lv_color_hex(0xffd54f), 0);
	lv_obj_add_flag(c->dot, LV_OBJ_FLAG_HIDDEN);

	l = text(c->root, f_small, C_MUTED, tr("Room"));
	lv_obj_align(l, LV_ALIGN_TOP_MID, 0, cy - (small ? S(62) : S(96)));
	c->big = text(c->root, small ? f_mid : f_big, C_TEXT, "–");
	lv_obj_align(c->big, LV_ALIGN_TOP_MID, 0, cy - (small ? S(42) : S(76)));

	/* what it is doing: icon and word, centred as one */
	row = box(c->root, LV_SIZE_CONTENT, S(30));
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, S(6), 0);
	lv_obj_align(row, LV_ALIGN_TOP_MID, 0, cy + (small ? S(4) : S(26)));
	c->act_icon = text(row, i_small, C_MUTED, "");
	c->action = text(row, f_body, C_MUTED, "");
	/* the line under it, inside the ring, above − and +: what comes next
	   ("Then Sleep at 22:30", "Temporary until 17:00"), with an icon in
	   front for a choice by hand; icon and words centred as one. A long
	   one ("Warming up for Comfort at 18:00") takes two lines: the row
	   grows down, there is room for that above - and + */
	row = box(c->root, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, S(4), 0);
	lv_obj_align(row, LV_ALIGN_TOP_MID, 0, cy + (small ? S(34) : S(58)));
	if (!strcmp(entity, "toon.thermostat")) {
		c->info_icon = text(row, i_tiny, C_MUTED, "");
		lv_obj_add_flag(c->info_icon, LV_OBJ_FLAG_HIDDEN);
	}
	c->info = text(row, f_small, C_MUTED, "");
	lv_obj_set_style_max_width(c->info, (int)(dia * 0.72), 0);
	lv_obj_set_style_text_align(c->info, LV_TEXT_ALIGN_CENTER, 0);

	/* - target + : round buttons under the ends of the ring */
	o = add_stepper(c, c->root, 2 * gap + bh, bh, small ? f_head : f_mid);
	lv_obj_align(o, LV_ALIGN_TOP_MID, 0, sy);
	for (int k = 0; k < 2; k++) {
		b = lv_obj_get_child(o, k);
		lv_obj_set_size(b, bh, bh);
		lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);	/* locked */
	}
	lv_obj_set_style_opa(c->arc, LV_OPA_50, LV_STATE_DISABLED);
	if (!with_row) {	/* no presets, no week program button (c->btns stays NULL) */
		finish(c);
		return c->root;
	}

	/* the buttons under the dial: the card's own feature (as in Home
	   Assistant, with its list of which ones), else the entity's presets
	   or modes (card_update) */
	c->feature = "climate-hvac-modes";
	cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(cfg, "features")) {
		const char *t = jstr(f, "type");

		if (t && (!strcmp(t, "climate-hvac-modes") || !strcmp(t, "climate-preset-modes") ||
			  !strcmp(t, "climate-fan-modes") || !strcmp(t, "climate-swing-modes"))) {
			c->feature = t;
			c->feat = f;
			break;
		}
	}
	/* ReToon's own: the week program on/off at the end of the preset row,
	   a margin around it counts as the button too */
	bh = !strcmp(entity, "toon.thermostat") ? S(56) : 0;
	if (bh) {
		c->prog = small_button(c->root, mdi("mdi:calendar-clock", "P"), i_mid);
		lv_obj_set_size(c->prog, bh, S(44));
		lv_obj_set_style_radius(c->prog, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_pos(c->prog, w - S(16) - bh, py);
		lv_obj_set_ext_click_area(c->prog, S(6));
		lv_obj_add_event_cb(c->prog, on_prog, LV_EVENT_CLICKED, c);
		bh += S(14);	/* a wider gap: it is no preset */
	}
	c->btns = box(c->root, w - S(32) - bh, S(44));
	lv_obj_set_pos(c->btns, S(16), py);
	lv_obj_set_flex_flow(c->btns, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(c->btns, S(8), 0);
	lv_obj_update_layout(c->btns);
	finish(c);
	return c->root;
}

/* the ring, its knob and the bar where the room is: shared by the dials */
static void add_arc(struct card *c, int dia)
{
	c->arc = lv_arc_create(c->root);
	lv_obj_set_size(c->arc, dia, dia);
	lv_arc_set_bg_angles(c->arc, 135, 45);
	lv_obj_set_style_arc_width(c->arc, dia < S(170) ? S(9) : S(12), 0);
	lv_obj_set_style_arc_width(c->arc, dia < S(170) ? S(9) : S(12), LV_PART_INDICATOR);
	lv_obj_set_style_arc_color(c->arc, lv_color_hex(C_CARD_HI), 0);
	lv_obj_set_style_bg_color(c->arc, lv_color_hex(C_ACCENT), LV_PART_KNOB);
	lv_obj_set_style_border_width(c->arc, S(3), LV_PART_KNOB);
	lv_obj_set_style_border_color(c->arc, lv_color_hex(0xffffff), LV_PART_KNOB);
	lv_obj_set_style_pad_all(c->arc, dia < S(170) ? S(3) : S(5), LV_PART_KNOB);
	lv_obj_set_style_shadow_width(c->arc, S(10), LV_PART_KNOB);
	lv_obj_set_style_shadow_color(c->arc, lv_color_hex(0x000000), LV_PART_KNOB);
	lv_obj_set_style_shadow_opa(c->arc, 110, LV_PART_KNOB);
	arc_events(c);

	/* covers the whole card, so the bar can be anywhere around the ring */
	c->dot = lv_line_create(c->root);
	lv_obj_set_pos(c->dot, 0, 0);
	lv_obj_set_size(c->dot, LV_PCT(100), LV_PCT(100));
	lv_obj_remove_flag(c->dot, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_line_width(c->dot, S(5), 0);
	lv_obj_set_style_line_rounded(c->dot, true, 0);
	lv_obj_set_style_line_color(c->dot, lv_color_hex(0xffd54f), 0);
	lv_obj_add_flag(c->dot, LV_OBJ_FLAG_HIDDEN);
}

/* the icon and name in the top left corner of a card */
static void add_title(struct card *c, int w)
{
	c->icon = text(c->root, i_small, C_MUTED, "");
	lv_obj_set_pos(c->icon, S(16), S(14));
	c->name = text(c->root, f_body, C_TEXT, "");
	lv_label_set_long_mode(c->name, LV_LABEL_LONG_DOT);
	lv_obj_set_size(c->name, w - S(68), S(24));
	lv_obj_set_pos(c->name, S(52), S(15));
}

/* the action word with its icon, centred as one */
static lv_obj_t *add_action(struct card *c, const lv_font_t *font)
{
	lv_obj_t *row = box(c->root, LV_SIZE_CONTENT, S(26));

	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, S(5), 0);
	c->act_icon = text(row, i_small, C_MUTED, "");
	c->action = text(row, font, C_MUTED, "");
	return row;
}

/*
 * Smaller thermostats, for pages with several of them ("style" in the card):
 *   compact  a small dial, three tile rows high: ring and knob, the room
 *            in the middle with the target under it, - and + beside the
 *            ring, the action word in the ring's opening
 *   bar      two tile rows: icon, name and action on the left, the room
 *            large on the right, and a slider for the target
 *   row      one tile row: icon, name, "Heating - 20.0 -> 21.0", with - and +
 * A tile's width options apply (grid_options.columns, 12 = the section).
 */
static lv_obj_t *thermostat_compact(lv_obj_t *parent, cJSON *cfg, const char *entity, int w)
{
	struct card *c = new_card(cfg, entity);
	/* three rows, or the rows it was given: then the ring grows with them;
	   "show_name": false (as Home Assistant's cards) leaves out icon and name */
	bool named = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(cfg, "show_name"));
	int h = fit_h ? fit_h : 3 * (TILE_H + GAP) - GAP, top = named ? S(42) : S(14);
	int dia = LV_CLAMP(S(120), LV_MIN(w - S(124), h - top - S(10)), S(330));
	bool large;
	int r, cy, bh;
	lv_obj_t *o, *b;

	/* in the rows it was given: room for - and + under the ring's ends */
	if (fit_h)
		dia = LV_MAX(S(120), LV_MIN(dia, (h - S(10) - top - S(54) + S(6)) * 2000 / 1707));
	large = dia >= S(260);		/* as the big dial: the large digits from here */
	r = dia / 2;
	bh = large ? S(54) : S(44);

	if (h - top - S(10) > dia)	/* narrower than high: the ring in the middle */
		top += (h - top - S(10) - dia) / 2;
	cy = top + r;
	c->root = panel(parent, w, h);
	if (named)
		add_title(c, w);
	add_arc(c, dia);
	lv_obj_align(c->arc, LV_ALIGN_TOP_MID, 0, top);

	c->big = text(c->root, large ? f_big : f_mid, C_TEXT, "–");
	lv_obj_align(c->big, LV_ALIGN_TOP_MID, 0, cy - (large ? S(76) : S(30)));
	/* the target, between a small - and + under the ring's ends */
	o = add_stepper(c, c->root, dia + 2 * (bh + S(6)), bh, large ? f_mid : f_body);
	lv_obj_align(o, LV_ALIGN_TOP_MID, 0, cy + (int)(r * 0.707) - S(6));
	for (int k = 0; k < 2; k++) {
		b = lv_obj_get_child(o, k);
		lv_obj_set_size(b, bh, bh);
		lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_text_font(lv_obj_get_child(b, 0), f_head, 0);
	}
	o = add_action(c, large ? f_body : f_small);
	lv_obj_align(o, LV_ALIGN_TOP_MID, 0, cy + (large ? S(26) : S(4)));
	finish(c);
	return c->root;
}

static lv_obj_t *thermostat_bar(lv_obj_t *parent, cJSON *cfg, const char *entity, int w)
{
	struct card *c = new_card(cfg, entity);
	int h = 2 * (TILE_H + GAP) - GAP;
	lv_obj_t *o, *l;

	c->root = panel(parent, w, h);
	c->icon_bg = box(c->root, S(44), S(44));
	lv_obj_set_pos(c->icon_bg, S(11), S(11));
	lv_obj_set_style_radius(c->icon_bg, S(22), 0);
	lv_obj_set_style_bg_opa(c->icon_bg, 50, 0);
	/* a plain object takes taps itself: let them reach the card (a 1x1
	   tile is almost all icon) */
	lv_obj_remove_flag(c->icon_bg, LV_OBJ_FLAG_CLICKABLE);
	c->icon = text(c->icon_bg, i_small, C_MUTED, "");
	lv_obj_center(c->icon);
	c->name = text(c->root, f_body, C_TEXT, "");
	lv_label_set_long_mode(c->name, LV_LABEL_LONG_DOT);
	lv_obj_set_size(c->name, w - S(160), S(24));
	lv_obj_set_pos(c->name, S(66), S(10));
	o = add_action(c, f_small);
	lv_obj_set_pos(o, S(66), S(34));

	/* the room, large, with the target under it ("Set to" when there is
	   room for the words) */
	c->big = text(c->root, f_head, C_TEXT, "–");
	lv_obj_align(c->big, LV_ALIGN_TOP_RIGHT, -S(16), S(8));
	c->target = text(c->root, f_small, C_MUTED, "–");
	lv_obj_set_width(c->target, S(60));
	lv_obj_set_style_text_align(c->target, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_align(c->target, LV_ALIGN_TOP_RIGHT, -S(16), S(40));
	if (w >= S(360)) {
		l = text(c->root, f_small, C_MUTED, tr("Set to"));
		lv_obj_align(l, LV_ALIGN_TOP_RIGHT, -S(16) - S(60) - S(5), S(40));
	}

	c->feature = "target-temperature";
	o = add_slider(c, c->root, w);
	lv_obj_set_size(o, w - S(28), S(28));
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_KNOB);
	lv_obj_set_style_bg_color(o, lv_color_hex(0xffffff), LV_PART_KNOB);
	lv_obj_set_style_pad_all(o, S(2), LV_PART_KNOB);
	lv_obj_set_pos(o, S(14), h - S(28) - S(14));
	finish(c);
	return c->root;
}

static lv_obj_t *thermostat_row(lv_obj_t *parent, cJSON *cfg, const char *entity, int w)
{
	struct card *c = new_card(cfg, entity);
	int sw = LV_MIN(S(170), w / 2);
	lv_obj_t *o;

	c->root = panel(parent, w, TILE_H);
	c->icon_bg = box(c->root, S(44), S(44));
	lv_obj_set_pos(c->icon_bg, S(11), S(11));
	lv_obj_set_style_radius(c->icon_bg, S(22), 0);
	lv_obj_set_style_bg_opa(c->icon_bg, 50, 0);
	/* a plain object takes taps itself: let them reach the card (a 1x1
	   tile is almost all icon) */
	lv_obj_remove_flag(c->icon_bg, LV_OBJ_FLAG_CLICKABLE);
	c->icon = text(c->icon_bg, i_small, C_MUTED, "");
	lv_obj_center(c->icon);
	c->name = text(c->root, f_body, C_TEXT, "");
	lv_label_set_long_mode(c->name, LV_LABEL_LONG_DOT);
	lv_obj_set_size(c->name, w - sw - S(80), S(24));
	lv_obj_set_pos(c->name, S(66), S(10));
	/* "Heating · 20.5 °C" from the state text */
	c->state = text(c->root, f_small, C_MUTED, "");
	lv_label_set_long_mode(c->state, LV_LABEL_LONG_DOT);
	lv_obj_set_size(c->state, w - sw - S(80), S(20));
	lv_obj_set_pos(c->state, S(66), S(36));

	o = add_stepper(c, c->root, sw, S(44), f_body);
	lv_obj_align(o, LV_ALIGN_RIGHT_MID, -S(11), 0);
	finish(c);
	return c->root;
}

/*
 *   classic  no ring, after the original Toon's thermostat tile (the layout
 *            of tne7laa's "Toon Thermostaat" app): what it is doing on top,
 *            the room large, the target with a flame that lights up while
 *            heating, what comes next, wide - and +, and the presets (or
 *            modes) two a row. Five tile rows; given fewer, smaller digits.
 */
#define CL_TOP		S(10)
#define CL_ACTION	S(24)
#define CL_TARGET	S(30)
#define CL_INFO		S(20)
#define CL_STEP		S(48)
#define CL_BTN		S(40)
#define CL_GAP		S(8)
#define CL_BOTTOM	S(12)

static int classic_height(bool row, bool large)
{
	return CL_TOP + CL_ACTION + lv_font_get_line_height(large ? f_big : f_mid) + CL_TARGET +
	       CL_INFO + CL_GAP + CL_STEP + (row ? CL_GAP + 2 * CL_BTN + CL_GAP : 0) + CL_BOTTOM;
}

static lv_obj_t *thermostat_classic(lv_obj_t *parent, cJSON *cfg, const char *entity, int w)
{
	struct card *c = new_card(cfg, entity);
	cJSON *f;
	bool own = !strcmp(entity, "toon.thermostat");
	bool with_row = thermostat_buttons(cfg, entity);
	int h = fit_h ? fit_h : ROW_H(5), bot = h - CL_BOTTOM, y, spare;
	bool large = h >= classic_height(with_row, true);
	const lv_font_t *fb = large ? f_big : f_mid;
	lv_obj_t *row, *b, *l;

	c->classic = true;
	c->root = panel(parent, w, h);

	/* from the bottom up: the buttons, then - and + */
	if (with_row) {
		c->feature = "climate-hvac-modes";	/* card_update picks presets when there are */
		cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(cfg, "features")) {
			const char *t = jstr(f, "type");

			if (t && (!strcmp(t, "climate-hvac-modes") || !strcmp(t, "climate-preset-modes") ||
				  !strcmp(t, "climate-fan-modes") || !strcmp(t, "climate-swing-modes"))) {
				c->feature = t;
				c->feat = f;
				break;
			}
		}
		bot -= 2 * CL_BTN + CL_GAP;
		c->btns = box(c->root, w - S(28), 2 * CL_BTN + CL_GAP);
		lv_obj_set_pos(c->btns, S(14), bot);
		lv_obj_set_flex_flow(c->btns, LV_FLEX_FLOW_ROW_WRAP);
		/* one row of buttons sits at the bottom */
		lv_obj_set_flex_align(c->btns, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
		lv_obj_set_style_pad_column(c->btns, CL_GAP, 0);
		lv_obj_set_style_pad_row(c->btns, CL_GAP, 0);
		lv_obj_update_layout(c->btns);
		bot -= CL_GAP;
	}
	bot -= CL_STEP;
	c->steps = box(c->root, w - S(28), CL_STEP);
	lv_obj_set_pos(c->steps, S(14), bot);
	for (int dir = -1; dir <= 1; dir += 2) {
		b = small_button(c->steps, dir < 0 ? "−" : "+", f_mid);
		lv_obj_set_size(b, (w - S(28) - CL_GAP) / 2, CL_STEP);
		lv_obj_align(b, dir < 0 ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID, 0, 0);
		lv_obj_set_user_data(b, (void *)(intptr_t)dir);
		lv_obj_add_event_cb(b, on_step, LV_EVENT_CLICKED, c);
		lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);	/* locked */
	}
	bot -= CL_GAP;

	/* on top: what it is doing; ReToon's own has its week program button
	   top right */
	c->action = text(c->root, f_body, C_MUTED, "");
	lv_label_set_long_mode(c->action, LV_LABEL_LONG_DOT);
	lv_obj_set_size(c->action, w - 2 * S(62), lv_font_get_line_height(f_body));	/* clear of the button */
	lv_obj_set_style_text_align(c->action, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(c->action, LV_ALIGN_TOP_MID, 0, CL_TOP);
	if (own && with_row) {
		c->prog = small_button(c->root, mdi("mdi:calendar-clock", "P"), i_small);
		lv_obj_set_size(c->prog, S(44), S(34));
		lv_obj_set_style_radius(c->prog, LV_RADIUS_CIRCLE, 0);
		lv_obj_align(c->prog, LV_ALIGN_TOP_RIGHT, -S(10), S(8));
		lv_obj_set_ext_click_area(c->prog, S(6));
		lv_obj_add_event_cb(c->prog, on_prog, LV_EVENT_CLICKED, c);
	}

	/* the room, the target and what comes next, in the middle of what is left */
	y = CL_TOP + CL_ACTION;
	spare = bot - y - (lv_font_get_line_height(fb) + CL_TARGET + CL_INFO);
	y += LV_MAX(0, spare) / 2;
	c->big = text(c->root, fb, C_TEXT, "–");
	lv_obj_align(c->big, LV_ALIGN_TOP_MID, 0, y);
	y += lv_font_get_line_height(fb);

	row = box(c->root, LV_SIZE_CONTENT, CL_TARGET);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, S(6), 0);
	lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
	l = text(row, f_head, C_TEXT, tr("Target"));
	c->target = text(row, f_head, C_TEXT, "–");
	c->act_icon = text(row, i_small, C_MUTED, "");
	(void)l;
	y += CL_TARGET;

	row = box(c->root, LV_SIZE_CONTENT, CL_INFO);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, S(4), 0);
	lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
	if (own) {
		c->info_icon = text(row, i_tiny, C_MUTED, "");
		lv_obj_add_flag(c->info_icon, LV_OBJ_FLAG_HIDDEN);
	}
	c->info = text(row, f_small, C_MUTED, "");
	lv_obj_set_style_max_width(c->info, w - S(60), 0);
	lv_obj_set_style_text_align(c->info, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_height(c->info, CL_INFO);	/* one line: a longer text is cut */
	finish(c);
	return c->root;
}

static lv_obj_t *entities(lv_obj_t *parent, cJSON *cfg, int w)
{
	cJSON *list = cJSON_GetObjectItemCaseSensitive(cfg, "entities"), *it;
	const char *title = jstr(cfg, "title");
	lv_obj_t *root = panel(parent, w, LV_SIZE_CONTENT), *l;
	int y = 8;

	if (title) {
		l = text(root, f_head, C_TEXT, title);
		lv_obj_set_pos(l, S(16), S(12));
		y = S(48);
	}
	cJSON_ArrayForEach(it, list) {
		const char *id = cJSON_IsString(it) ? it->valuestring : jstr(it, "entity");
		struct card *c;

		if (!id)
			continue;
		c = new_card(cJSON_IsObject(it) ? it : NULL, id);
		c->row = true;
		c->root = box(root, w, S(44));
		lv_obj_set_pos(c->root, 0, y);
		lv_obj_add_flag(c->root, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_style_bg_color(c->root, lv_color_hex(C_CARD_HI), LV_STATE_PRESSED);
		lv_obj_set_style_bg_opa(c->root, LV_OPA_COVER, LV_STATE_PRESSED);
		lv_obj_add_event_cb(c->root, on_tap, LV_EVENT_CLICKED, c);
		c->icon = text(c->root, i_small, C_MUTED, "");
		lv_obj_align(c->icon, LV_ALIGN_LEFT_MID, S(16), 0);
		c->name = text(c->root, f_body, C_TEXT, "");
		lv_label_set_long_mode(c->name, LV_LABEL_LONG_DOT);
		lv_obj_set_width(c->name, w * 55 / 100 - S(56));
		lv_obj_align(c->name, LV_ALIGN_LEFT_MID, S(54), 0);
		c->state = text(c->root, f_body, C_MUTED, "");
		lv_label_set_long_mode(c->state, LV_LABEL_LONG_DOT);
		lv_obj_set_width(c->state, w * 45 / 100 - S(16));
		lv_obj_set_style_text_align(c->state, LV_TEXT_ALIGN_RIGHT, 0);
		lv_obj_align(c->state, LV_ALIGN_RIGHT_MID, -S(16), 0);
		finish(c);
		y += S(44);
	}
	l = box(root, 1, S(8));
	lv_obj_set_pos(l, 0, y);
	return root;
}

static lv_obj_t *markdown(lv_obj_t *parent, cJSON *cfg, int w)
{
	const char *src = jstr(cfg, "content");
	lv_obj_t *root = panel(parent, w, LV_SIZE_CONTENT), *l;
	char *plain = calloc(1, (src ? strlen(src) : 0) + 1), *p = plain;

	lv_obj_set_style_pad_all(root, S(14), 0);
	for (const char *s = src ? src : ""; *s; s++) {
		bool bol = s == src || s[-1] == '\n';

		if (*s == '*' || *s == '`' || (bol && *s == '#'))
			continue;
		if (*s == ' ' && p > plain && p[-1] == '\n')
			continue;
		*p++ = *s;
	}
	l = text(root, f_body, C_TEXT, plain);
	lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(l, w - S(28));
	free(plain);
	return root;
}

/*
 * The analogue clock ("style": "analog"), the design of the AnalogClock app:
 * a round dial with an edge, 60 marks (longer and thicker each hour), two
 * hands with round ends from the middle, a dot in the middle; on black
 * unless "background" is "card". One object that draws it all itself,
 * drawn again when the minute (with "seconds": a thin seconds hand, the
 * second) changes. Colours: "dial_color" (the edge, and the standard of the
 * others; standard the text colour), "hands_color", "marks_color" (hours),
 * "minute_marks_color".
 */
struct aclock {
	uint32_t edge, hands, marks, minor;
	bool seconds;
	int shown;		/* the minute or second it was drawn for */
};

static void aclock_line(lv_layer_t *layer, int cx, int cy, double turns, double from, double to,
			double width, uint32_t color, bool round)
{
	lv_draw_line_dsc_t d;
	double a = turns * 2 * M_PI - M_PI / 2;

	lv_draw_line_dsc_init(&d);
	d.p1.x = cx + cos(a) * from;
	d.p1.y = cy + sin(a) * from;
	d.p2.x = cx + cos(a) * to;
	d.p2.y = cy + sin(a) * to;
	d.color = lv_color_hex(color);
	d.width = width >= 1 ? lround(width) : 1;
	d.round_start = d.round_end = round;
	lv_draw_line(layer, &d);
}

static void aclock_dot(lv_layer_t *layer, int cx, int cy, double r, uint32_t color)
{
	lv_draw_rect_dsc_t rd;
	lv_area_t a = { cx - lround(r), cy - lround(r), cx + lround(r), cy + lround(r) };

	lv_draw_rect_dsc_init(&rd);
	rd.bg_color = lv_color_hex(color);
	rd.radius = LV_RADIUS_CIRCLE;
	lv_draw_rect(layer, &rd, &a);
}

static void aclock_draw(lv_event_t *ev)
{
	lv_obj_t *o = lv_event_get_target(ev);
	struct aclock *k = lv_event_get_user_data(ev);
	lv_layer_t *layer = lv_event_get_layer(ev);
	lv_area_t a;
	lv_draw_rect_dsc_t rd;
	time_t t = time(NULL);
	struct tm tm;
	int cx, cy;
	double r, m, h;

	lv_obj_get_coords(o, &a);
	cx = (a.x1 + a.x2 + 1) / 2;
	cy = (a.y1 + a.y2 + 1) / 2;
	r = LV_MIN(lv_area_get_width(&a), lv_area_get_height(&a)) * 0.46;
	localtime_r(&t, &tm);
	k->shown = k->seconds ? tm.tm_sec : tm.tm_min;

	lv_draw_rect_dsc_init(&rd);		/* the dial's edge */
	rd.bg_opa = LV_OPA_TRANSP;
	rd.border_color = lv_color_hex(k->edge);
	rd.border_width = LV_MAX(1, S(1));
	rd.radius = LV_RADIUS_CIRCLE;
	a.x1 = cx - lround(r), a.x2 = cx + lround(r);
	a.y1 = cy - lround(r), a.y2 = cy + lround(r);
	lv_draw_rect(layer, &rd, &a);
	for (int i = 0; i < 60; i++) {
		bool hour = i % 5 == 0;

		aclock_line(layer, cx, cy, i / 60.0, r * (hour ? 0.80 : 0.94), r * 0.98,
			    r * (hour ? 0.022 : 0.007), hour ? k->marks : k->minor, false);
	}
	if (tm.tm_year < 120)		/* no time yet: no hands */
		return;
	m = tm.tm_min + tm.tm_sec / 60.0;
	h = tm.tm_hour % 12 + m / 60;
	aclock_line(layer, cx, cy, h / 12, 0, r * 0.50, LV_MAX(2, r * 0.08), k->hands, true);
	aclock_line(layer, cx, cy, m / 60, 0, r * 0.73, LV_MAX(2, r * 0.055), k->hands, true);
	aclock_dot(layer, cx, cy, r * 0.045, k->hands);
	if (k->seconds) {
		aclock_line(layer, cx, cy, tm.tm_sec / 60.0, -r * 0.15, r * 0.85, LV_MAX(1, r * 0.012),
			    C_ACCENT, true);
		aclock_dot(layer, cx, cy, r * 0.025, C_ACCENT);
	}
}

static void aclock_free(lv_event_t *ev)
{
	free(lv_event_get_user_data(ev));
}

/* from the clock tick: draw again when the hands moved */
static void aclock_tick(lv_obj_t *o)
{
	struct aclock *k = lv_obj_get_user_data(o);
	time_t t = time(NULL);
	struct tm tm;

	localtime_r(&t, &tm);
	if (k && (k->seconds ? tm.tm_sec : tm.tm_min) != k->shown)
		lv_obj_invalidate(o);
}

static lv_obj_t *aclock_card(lv_obj_t *parent, cJSON *cfg, int w, int h)
{
	lv_obj_t *root = panel(parent, w, h), *o;
	struct aclock *k = calloc(1, sizeof(*k));
	uint32_t base = dash_color(jstr(cfg, "dial_color"), C_TEXT);
	const char *bg = jstr(cfg, "background");
	int d = LV_MIN(w, h) - 2 * S(12);

	k->edge = base;
	k->hands = dash_color(jstr(cfg, "hands_color"), base);
	k->marks = dash_color(jstr(cfg, "marks_color"), base);
	k->minor = dash_color(jstr(cfg, "minute_marks_color"), base);
	k->seconds = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cfg, "seconds"));
	k->shown = -1;
	if (!bg || strcmp(bg, "card"))
		lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
	o = box(root, d, d);
	lv_obj_center(o);
	lv_obj_set_user_data(o, k);
	lv_obj_add_flag(o, LV_OBJ_FLAG_USER_2);	/* found by the clock tick */
	lv_obj_add_event_cb(o, aclock_draw, LV_EVENT_DRAW_MAIN, k);
	lv_obj_add_event_cb(o, aclock_free, LV_EVENT_DELETE, k);
	return root;
}

static lv_obj_t *dclock_card(lv_obj_t *parent, int w, int h);

static lv_obj_t *clock_card(lv_obj_t *parent, cJSON *cfg, int w)
{
	int h = fit_h > ROW_H(2) ? fit_h : ROW_H(2);
	const char *style = jstr(cfg, "style"), *bg = jstr(cfg, "background");
	lv_obj_t *root;

	if (style && !strcmp(style, "analog"))
		return aclock_card(parent, cfg, w, h);
	root = dclock_card(parent, w, h);
	if (bg && !strcmp(bg, "black"))
		lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
	return root;
}

/*
 * The digital clock: two rows high of itself. Made taller (up to the whole
 * page) the digits grow with it, as far as the width allows; the time and
 * the date then stay together in the middle.
 */
static lv_obj_t *dclock_card(lv_obj_t *parent, int w, int h)
{
	int date_h = lv_font_get_line_height(f_body);
	/* "00:00" in this font is about 2.7 times its size wide; its line
	   about 1.2 times its size high */
	int size = LV_MIN((h - S(40) - date_h) * 10 / 12, (w - S(32)) * 10 / 27);
	lv_obj_t *root = panel(parent, w, h), *l;
	lv_font_t *big = f_big;

	size -= size % S(8);		/* fewer sizes to keep */
	if (size > S(78) + S(8) && (big = clock_font(size)) == NULL)
		big = f_big;
	l = text(root, big, C_TEXT, "");
	if (big == f_big) {
		lv_obj_align(l, LV_ALIGN_TOP_MID, 0, S(14));
		lv_obj_set_user_data(root, l);
		l = text(root, f_body, C_MUTED, "");
		lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -S(16));
	} else {
		int line = lv_font_get_line_height(big);

		lv_obj_align(l, LV_ALIGN_CENTER, 0, -(date_h + S(8)) / 2);
		lv_obj_set_user_data(root, l);
		l = text(root, f_body, C_MUTED, "");
		lv_obj_align(l, LV_ALIGN_CENTER, 0, (line + S(8)) / 2);
	}
	lv_obj_add_flag(root, LV_OBJ_FLAG_USER_1);	/* found by the clock tick */
	return root;
}

static lv_obj_t *unsupported(lv_obj_t *parent, const char *type, int w)
{
	lv_obj_t *root = panel(parent, w, S(44)), *l;
	char buf[96];

	if (type)
		snprintf(buf, sizeof(buf), tr("%s card: not shown on the ReToon"), type);
	else
		snprintf(buf, sizeof(buf), "%s", tr("This card: not shown on the ReToon"));
	l = text(root, f_small, C_MUTED, buf);
	lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
	lv_obj_set_width(l, w - S(28));
	lv_obj_align(l, LV_ALIGN_LEFT_MID, S(14), 0);
	return root;
}

static lv_obj_t *make_card(lv_obj_t *parent, cJSON *cfg, int w);

/* cards next to or under each other */
static lv_obj_t *stack(lv_obj_t *parent, cJSON *cfg, int w, int per_row)
{
	cJSON *list = cJSON_GetObjectItemCaseSensitive(cfg, "cards"), *it;
	lv_obj_t *root = box(parent, w, LV_SIZE_CONTENT);
	int cw = (w - (per_row - 1) * GAP) / per_row;

	lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_row(root, GAP, 0);
	lv_obj_set_style_pad_column(root, GAP, 0);
	cJSON_ArrayForEach(it, list)
		make_card(root, it, cw);
	return root;
}

/*
 * Home Assistant's gauge card: a half ring from "min" to "max" (0 and 100
 * when not set), the value in it and the name under it; "unit" instead of
 * the entity's own. Colours from "severity" (green, yellow, red: the value
 * from which each one starts) or "segments" (a list of from and color).
 * Without "needle" the ring fills up to the value, in the colour for that
 * value (blue below the first, or without colours); with "needle" the ring
 * shows all the colours and a needle points at the value.
 */
#define MAX_SEGS	8
struct seg {
	double from;
	uint32_t color;
};

static double cfg_num(cJSON *cfg, const char *key, double def)
{
	cJSON *v = cJSON_GetObjectItemCaseSensitive(cfg, key);

	return cJSON_IsNumber(v) ? v->valuedouble : def;
}

/* the colours of a gauge, in the order of where they start */
static int gauge_segs(cJSON *cfg, struct seg *s)
{
	static const struct {
		const char *key;
		uint32_t color;
	} sev[] = { { "green", 0x43a047 }, { "yellow", 0xffa600 }, { "red", 0xdb4437 } };
	cJSON *o = cJSON_GetObjectItemCaseSensitive(cfg, "segments"), *it;
	int n = 0;

	if (cJSON_IsArray(o)) {
		cJSON_ArrayForEach(it, o) {
			cJSON *f = cJSON_GetObjectItemCaseSensitive(it, "from");

			if (n < MAX_SEGS && cJSON_IsNumber(f))
				s[n++] = (struct seg){ f->valuedouble, dash_color(jstr(it, "color"), C_ACCENT) };
		}
	} else if (cJSON_IsObject(o = cJSON_GetObjectItemCaseSensitive(cfg, "severity"))) {
		for (int i = 0; i < 3; i++) {
			cJSON *f = cJSON_GetObjectItemCaseSensitive(o, sev[i].key);

			if (cJSON_IsNumber(f))
				s[n++] = (struct seg){ f->valuedouble, sev[i].color };
		}
	}
	for (int i = 1; i < n; i++)
		for (int j = i; j > 0 && s[j].from < s[j - 1].from; j--) {
			struct seg t = s[j];

			s[j] = s[j - 1];
			s[j - 1] = t;
		}
	return n;
}

/* where a value is on the half ring: 180 (left) to 360 (right), as LVGL
   counts (0 is to the right, clockwise) */
static int gauge_angle(double v, double lo, double hi)
{
	double f = hi > lo ? (v - lo) / (hi - lo) : 0;

	return 180 + (int)(LV_CLAMP(0.0, f, 1.0) * 180 + 0.5);
}

/* LVGL takes 0 for the end at the right */
static int arc_end(int a)
{
	return a >= 360 ? a - 360 : a;
}

static lv_obj_t *gauge_ring(lv_obj_t *parent, int x, int y, int d, int band, int a0, int a1, uint32_t color)
{
	lv_obj_t *a = lv_arc_create(parent);

	lv_obj_set_size(a, d, d);
	lv_obj_set_pos(a, x, y);
	lv_obj_remove_style(a, NULL, LV_PART_KNOB);
	lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_pad_all(a, 0, 0);
	lv_arc_set_bg_angles(a, a0, arc_end(a1));
	lv_arc_set_angles(a, a0, a0);		/* nothing filled */
	lv_obj_set_style_arc_width(a, band, 0);
	lv_obj_set_style_arc_width(a, band, LV_PART_INDICATOR);
	lv_obj_set_style_arc_rounded(a, false, 0);
	lv_obj_set_style_arc_rounded(a, false, LV_PART_INDICATOR);
	lv_obj_set_style_arc_color(a, lv_color_hex(color), 0);
	return a;
}

static lv_obj_t *gauge(lv_obj_t *parent, cJSON *cfg, const char *entity, int w)
{
	struct card *c = new_card(cfg, entity);
	struct seg s[MAX_SEGS];
	int n = gauge_segs(cfg, s);
	double lo = cfg_num(cfg, "min", 0), hi = cfg_num(cfg, "max", 100);
	cJSON *nd = cJSON_GetObjectItemCaseSensitive(cfg, "needle");
	/* true, also written as "true" (as people type it in the JSON) */
	bool needle = cJSON_IsTrue(nd) || (cJSON_IsString(nd) && !strcasecmp(nd->valuestring, "true"));
	int top = S(16), name_h = S(26), pad = S(12), d = LV_MIN(w - S(40), S(280)), band, h, x, used;
	lv_font_t *font;

	/* as many rows as the card was given, else what it needs rounded up to
	   whole tile rows, so it lines up with the tiles next to it */
	used = top + d / 2 + S(6) + name_h + pad;
	h = fit_h ? fit_h : ROW_H((used + GAP + TILE_H + GAP - 1) / (TILE_H + GAP));
	/* too low for the name as well (a single row): the ring and the value */
	if (h < top + S(30) + S(6) + name_h + pad) {
		top = S(8);
		name_h = 0;
		pad = S(6);
	}
	d = LV_MAX(S(40), LV_MIN(d, (h - top - S(6) - name_h - pad) * 2));
	band = LV_MAX(S(6), d * 17 / 100);
	used = top + d / 2 + S(6) + name_h + pad;
	if (h > used)
		top += (h - used) / 2;	/* what is left: around it */
	x = (w - d) / 2;
	/* the value fits inside the ring */
	font = d >= S(240) ? f_mid : d >= S(150) ? f_head : d >= S(90) ? f_body : f_small;

	c->root = panel(parent, w, h);
	/* grey under it all; with a needle the colours on top, where they are */
	c->gauge = gauge_ring(c->root, x, top, d, band, 180, 360, C_CARD_HI);
	if (needle) {
		for (int i = 0; i < n; i++) {
			int a0 = gauge_angle(LV_MAX(s[i].from, lo), lo, hi);
			int a1 = gauge_angle(i + 1 < n ? s[i + 1].from : hi, lo, hi);

			if (a1 > a0)
				gauge_ring(c->root, x, top, d, band, a0, a1, s[i].color);
		}
		/* covers the whole card, as the thermostat's bar */
		c->needle = lv_line_create(c->root);
		lv_obj_set_pos(c->needle, 0, 0);
		lv_obj_set_size(c->needle, LV_PCT(100), LV_PCT(100));
		lv_obj_remove_flag(c->needle, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_style_line_width(c->needle, LV_MAX(S(4), band / 3), 0);
		lv_obj_set_style_line_rounded(c->needle, true, 0);
		lv_obj_set_style_line_color(c->needle, lv_color_hex(C_TEXT), 0);
		lv_obj_add_flag(c->needle, LV_OBJ_FLAG_HIDDEN);
	}
	c->big = text(c->root, font, C_TEXT, "–");
	lv_obj_align(c->big, LV_ALIGN_TOP_MID, 0, top + d / 2 - lv_font_get_line_height(font) + S(4));
	c->name = text(c->root, f_body, C_TEXT, "");
	lv_label_set_long_mode(c->name, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_align(c->name, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_size(c->name, w - S(24), LV_MAX(name_h, 1));
	lv_obj_align(c->name, LV_ALIGN_TOP_MID, 0, top + d / 2 + S(6));
	if (!name_h)
		lv_obj_add_flag(c->name, LV_OBJ_FLAG_HIDDEN);
	lv_obj_update_layout(c->root);
	finish(c);
	return c->root;
}

static void gauge_update(struct card *c, struct entity *e)
{
	const char *name = jstr(c->cfg, "name"), *unit = jstr(c->cfg, "unit"), *st = e->state;
	double lo = cfg_num(c->cfg, "min", 0), hi = cfg_num(c->cfg, "max", 100), v = 0;
	struct seg s[MAX_SEGS];
	int n = gauge_segs(c->cfg, s), ang;
	char domain[32], buf[96], *end = NULL;
	bool num;

	if (!name)
		name = ent_str(e, "friendly_name");
	set_text(c->name, name ? name : c->ent);
	if (st)
		v = strtod(st, &end);
	num = st && end != st && !*end;
	if (num && unit) {
		snprintf(buf, sizeof(buf), "%s %s", fmt_number(e, st, v), unit);
	} else {
		domain_of(c->ent, domain, sizeof(domain));
		fmt_state(e, domain, buf, sizeof(buf));
	}
	set_text(c->big, buf);
	if (!num) {
		lv_arc_set_angles(c->gauge, 180, 180);
		if (c->needle)
			lv_obj_add_flag(c->needle, LV_OBJ_FLAG_HIDDEN);
		return;
	}
	ang = gauge_angle(v, lo, hi);
	if (c->needle) {
		lv_area_t a, l;
		double rad = ang * M_PI / 180;
		int r, band = lv_obj_get_style_arc_width(c->gauge, 0);

		lv_obj_get_coords(c->gauge, &a);
		lv_obj_get_coords(c->needle, &l);
		r = lv_area_get_width(&a) / 2;
		for (int k = 0; k < 2; k++) {
			/* short, from just inside the ring to halfway into it */
			int rk = k ? r - band / 2 : r - band - S(8);

			c->tick[k].x = (a.x1 + a.x2) / 2 - l.x1 + cos(rad) * rk;
			c->tick[k].y = (a.y1 + a.y2) / 2 - l.y1 + sin(rad) * rk;
		}
		lv_line_set_points(c->needle, c->tick, 2);
		lv_obj_remove_flag(c->needle, LV_OBJ_FLAG_HIDDEN);
	} else {
		uint32_t col = C_ACCENT;

		for (int i = 0; i < n; i++)
			if (v >= s[i].from)
				col = s[i].color;
		lv_arc_set_angles(c->gauge, 180, arc_end(ang));
		lv_obj_set_style_arc_color(c->gauge, lv_color_hex(col), LV_PART_INDICATOR);
	}
}

static lv_obj_t *make_card(lv_obj_t *parent, cJSON *cfg, int w)
{
	const char *type = jstr(cfg, "type"), *ent = jstr(cfg, "entity");
	lv_obj_t *o;
	cJSON *n;

	if (!type)
		type = "";
	if ((o = energy_card(parent, cfg, type, w)))
		return o;
	if (!strcmp(type, "heading"))
		return heading(parent, cfg, w);
	if (!strcmp(type, "custom:toon-clock"))
		return clock_card(parent, cfg, w);
	if (!strcmp(type, "custom:retoon-app"))
		return apps_card(parent, cfg, w);
	if (!strcmp(type, "thermostat") && ent) {
		const char *style = jstr(cfg, "style");

		if (style && !strcmp(style, "compact"))
			return thermostat_compact(parent, cfg, ent, w);
		if (style && !strcmp(style, "bar"))
			return thermostat_bar(parent, cfg, ent, w);
		if (style && !strcmp(style, "row"))
			return thermostat_row(parent, cfg, ent, w);
		if (style && !strcmp(style, "classic"))
			return thermostat_classic(parent, cfg, ent, w);
		return thermostat(parent, cfg, ent, w);
	}
	if (!strcmp(type, "gauge") && ent)
		return gauge(parent, cfg, ent, w);
	if (!strcmp(type, "markdown"))
		return markdown(parent, cfg, w);
	if (!strcmp(type, "vertical-stack"))
		return stack(parent, cfg, w, 1);
	if (!strcmp(type, "horizontal-stack"))
		return stack(parent, cfg, w, LV_CLAMP(1, cJSON_GetArraySize(
			cJSON_GetObjectItemCaseSensitive(cfg, "cards")), 4));
	if (!strcmp(type, "grid")) {
		n = cJSON_GetObjectItemCaseSensitive(cfg, "columns");
		return stack(parent, cfg, w, cJSON_IsNumber(n) ? LV_CLAMP(1, n->valueint, 6) : 3);
	}
	if (!strcmp(type, "conditional") && cJSON_GetObjectItemCaseSensitive(cfg, "card"))
		return make_card(parent, cJSON_GetObjectItemCaseSensitive(cfg, "card"), w);
	if (!strcmp(type, "history-graph") || !strcmp(type, "statistics-graph") ||
	    (!strcmp(type, "sensor") && ent && jstr(cfg, "graph") && !strcmp(jstr(cfg, "graph"), "line")))
		return graph_card(parent, cfg, w);
	if (cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(cfg, "entities")))
		return entities(parent, cfg, w);
	if (ent)
		return tile(parent, cfg, ent, w);
	return unsupported(parent, type, w);
}

/*
 * Sections as in Home Assistant: a grid of 12 columns and rows of one tile
 * (a row on the Toon is a tile high; in Home Assistant 56 pixels). A card's
 * size is its grid_options (columns 1-12 or "full", rows a number or
 * "auto"; the older layout_options grid_columns/grid_rows too). The cards
 * are placed in order, each in the first place where it fits ("dense", as
 * Home Assistant does), so a small card can sit next to a tall one.
 * A card with "auto" rows keeps its own height; a row is as tall as what
 * is in it.
 */
#define MAX_ROWS	64
#define MAX_ITEMS	64

/*
 * Where the cards of a page in the older layouts went (sections, or a list
 * over columns), to turn the page into a grid once (convert_view).
 */
#define MAX_RECS	256
static struct rec {
	lv_obj_t *o;
	cJSON *cfg;		/* NULL: a section's title, o is its label */
} recs[MAX_RECS];
static int nrecs;
static bool recording;

static void record(lv_obj_t *o, cJSON *cfg)
{
	if (recording && o && nrecs < MAX_RECS)
		recs[nrecs++] = (struct rec){ o, cfg };
}

static cJSON *grid_value(cJSON *cfg, const char *key, const char *old)
{
	cJSON *v = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cfg, "grid_options"), key);

	if (!v)
		v = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cfg, "layout_options"), old);
	return v;
}

/* the columns of the section's 12 a card takes */
static int card_columns(cJSON *cfg)
{
	cJSON *c = grid_value(cfg, "columns", "grid_columns");
	const char *type = jstr(cfg, "type");

	if (cJSON_IsNumber(c))
		return LV_CLAMP(1, c->valueint, 12);
	if (cJSON_IsString(c))		/* "full" */
		return 12;
	if (type && (!strcmp(type, "tile") || !strcmp(type, "button") || !strcmp(type, "entity") ||
		     !strcmp(type, "sensor") || !strcmp(type, "light") || !strcmp(type, "gauge")))
		return 6;
	return 12;
}

/* its rows; 0: "auto", its own height */
static int card_rows(cJSON *cfg)
{
	cJSON *r = grid_value(cfg, "rows", "grid_rows");

	return cJSON_IsNumber(r) ? LV_CLAMP(1, r->valueint, 16) : 0;
}

static bool is_type(cJSON *cfg, const char *t)
{
	const char *type = jstr(cfg, "type");

	return type && !strcmp(type, t);
}

static bool is_graph(cJSON *cfg)
{
	return is_type(cfg, "history-graph") || is_type(cfg, "statistics-graph") ||
	       energy_is_graph(jstr(cfg, "type")) ||
	       (is_type(cfg, "sensor") && jstr(cfg, "graph") && !strcmp(jstr(cfg, "graph"), "line"));
}

/* the big thermostat, which can be made last to end level with its neighbours */
static bool is_dial(cJSON *cfg)
{
	return is_type(cfg, "thermostat") && (!jstr(cfg, "style") || !strcmp(jstr(cfg, "style"), "dial"));
}

/* the cards that grow to the rows they are given (fit_h) */
static bool fills_rows(cJSON *cfg)
{
	return is_dial(cfg) || is_type(cfg, "custom:toon-clock") || is_type(cfg, "gauge") ||
	       (is_type(cfg, "thermostat") && jstr(cfg, "style") &&
		(!strcmp(jstr(cfg, "style"), "compact") || !strcmp(jstr(cfg, "style"), "classic")));
}

/* where column col of a section sw wide starts */
static int col_x(int col, int sw)
{
	return (sw + GAP) * col / 12;
}

static int section_card_width(cJSON *cfg, int sw)
{
	return col_x(card_columns(cfg), sw) - GAP;
}

/* one section: its cards (and title) with what the grid needs of them */
struct gsec {
	int n, sw;
	struct gitem {
		lv_obj_t *o;
		cJSON *cfg;		/* NULL: the section's title */
		int cols, rows;		/* rows 0: auto */
		int h;			/* its own height */
		int col, row, span;	/* where it went */
	} it[MAX_ITEMS];
};

static void gsec_free(lv_event_t *ev)
{
	free(lv_event_get_user_data(ev));
}

static lv_obj_t *section_box(lv_obj_t *parent, int w)
{
	lv_obj_t *o = box(parent, w, 0);
	struct gsec *g = calloc(1, sizeof(*g));

	g->sw = w;
	lv_obj_set_user_data(o, g);
	lv_obj_add_event_cb(o, gsec_free, LV_EVENT_DELETE, g);
	return o;
}

/* the size the grid gives a card: its rows, but never smaller than what it
   needs to show itself */
static void size_card(lv_obj_t *o, cJSON *cfg, int rows)
{
	int want = ROW_H(rows), have;

	if (is_graph(cfg)) {
		graph_set_height(o, LV_MAX(want, ROW_H(2)));
		return;
	}
	if (is_type(cfg, "custom:retoon-app")) {
		lv_obj_set_height(o, want);
		return;
	}
	lv_obj_update_layout(o);
	have = lv_obj_get_height(o);
	if (want <= have)
		return;
	lv_obj_set_height(o, want);
	/* a tile: what it shows in the middle of the taller card */
	if (is_type(cfg, "tile") || (is_type(cfg, "thermostat") && !is_dial(cfg)))
		lv_obj_set_style_pad_top(o, (want - have) / 2, 0);
}

static void section_add(lv_obj_t *sec, lv_obj_t *o, cJSON *cfg)
{
	struct gsec *g = lv_obj_get_user_data(sec);
	struct gitem *it;

	if (!o || g->n >= MAX_ITEMS)
		return;
	record(o, cfg);
	it = &g->it[g->n++];
	it->o = o;
	it->cfg = cfg;
	it->cols = cfg ? card_columns(cfg) : 12;
	it->rows = cfg ? card_rows(cfg) : 0;
	lv_obj_update_layout(o);
	it->h = lv_obj_get_height(o);
}

/* a card made for a section, at its size */
static void section_card(lv_obj_t *sec, cJSON *cfg)
{
	struct gsec *g = lv_obj_get_user_data(sec);
	int rows = card_rows(cfg);
	lv_obj_t *o;

	if (rows && fills_rows(cfg))
		fit_h = ROW_H(rows);
	o = make_card(sec, cfg, section_card_width(cfg, g->sw));
	fit_h = 0;
	if (o && rows)
		size_card(o, cfg, rows);
	section_add(sec, o, cfg);
}

/* put the cards in their places; the section gets the height they need */
static void section_place(lv_obj_t *sec)
{
	struct gsec *g = lv_obj_get_user_data(sec);
	uint16_t used[MAX_ROWS] = { 0 };
	int th[MAX_ROWS] = { 0 }, ty[MAX_ROWS], nrows = 0, total;

	for (int i = 0; i < g->n; i++) {
		struct gitem *it = &g->it[i];
		int c = LV_MIN(it->cols, 12), r;
		uint16_t mask = (uint16_t)((1u << c) - 1);
		bool done = false;

		/* auto: as many rows as its height is nearest to */
		r = it->rows ? it->rows : LV_MAX(1, (it->h + GAP + (TILE_H + GAP) / 2) / (TILE_H + GAP));
		r = LV_MIN(r, MAX_ROWS);
		for (int row = 0; row + r <= MAX_ROWS && !done; row++)
			for (int col = 0; col + c <= 12 && !done; col++) {
				bool fits = true;

				for (int k = row; k < row + r && fits; k++)
					fits = !(used[k] & (mask << col));
				if (!fits)
					continue;
				for (int k = row; k < row + r; k++)
					used[k] |= mask << col;
				it->col = col;
				it->row = row;
				it->span = r;
				nrows = LV_MAX(nrows, row + r);
				done = true;
			}
		if (!done)		/* the grid is full: under everything */
			it->span = 0;
	}
	/* how tall each row is: what is in it; a card over more rows takes
	   what it needs more than its rows have from its last row */
	for (int i = 0; i < g->n; i++)
		if (g->it[i].span == 1)
			th[g->it[i].row] = LV_MAX(th[g->it[i].row], g->it[i].h);
	for (int span = 2; span <= MAX_ROWS; span++)
		for (int i = 0; i < g->n; i++) {
			struct gitem *it = &g->it[i];
			int have = (span - 1) * GAP;

			if (it->span != span)
				continue;
			for (int k = it->row; k < it->row + span; k++)
				have += th[k];
			if (it->h > have)
				th[it->row + span - 1] += it->h - have;
		}
	for (int k = 0, y = 0; k < nrows; k++) {
		ty[k] = y;
		y += th[k] + GAP;
	}
	total = nrows ? ty[nrows - 1] + th[nrows - 1] : 0;

	for (int i = 0; i < g->n; i++) {
		struct gitem *it = &g->it[i];
		int y;

		if (!it->span) {	/* a row of its own, at the bottom */
			y = total ? total + GAP : 0;
			total = y + it->h;
		} else {
			y = ty[it->row];
		}
		lv_obj_set_pos(it->o, col_x(it->col, g->sw), y);
	}
	lv_obj_set_height(sec, total);
}

static lv_obj_t *flow(lv_obj_t *parent, int w)
{
	lv_obj_t *o = box(parent, w, LV_SIZE_CONTENT);

	lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_row(o, GAP, 0);
	lv_obj_set_style_pad_column(o, GAP, 0);
	return o;
}

static void build_grid(lv_obj_t *page, cJSON *view);
static bool is_grid_view(cJSON *view);

static void build_view(lv_obj_t *page, cJSON *view)
{
	cJSON *sections = cJSON_GetObjectItemCaseSensitive(view, "sections"), *s, *c;

	if (is_grid_view(view)) {
		build_grid(page, view);
		return;
	}
	cJSON *list = cJSON_GetObjectItemCaseSensitive(view, "cards");
	cJSON *vc = cJSON_GetObjectItemCaseSensitive(view, "columns");
	int cw = SCREEN_W, sw;

	/* each page has its own number of columns */
	columns = cJSON_IsNumber(vc) ? LV_CLAMP(1, vc->valueint, 4) : std_columns;
	sw = (cw - S(14) * (columns + 1)) / columns;
	if (cJSON_IsArray(sections)) {
		lv_obj_t *secs[16];
		cJSON *later[16] = { 0 };
		int ns = 0;

		/* a section that is only a big thermostat (that sets no rows) is
		   filled last, to end level with the sections next to it */
		cJSON_ArrayForEach(s, sections) {
			cJSON *span = cJSON_GetObjectItemCaseSensitive(s, "column_span");
			cJSON *cards = cJSON_GetObjectItemCaseSensitive(s, "cards");
			cJSON *first = cJSON_GetArrayItem(cards, 0);
			int k = cJSON_IsNumber(span) ? LV_CLAMP(1, span->valueint, columns) : 1;
			int w = sw * k + S(14) * (k - 1);
			lv_obj_t *sec = section_box(page, w);
			const char *title = jstr(s, "title");

			if (title) {
				lv_obj_t *l = text(sec, f_head, C_TEXT, title);

				lv_obj_set_width(l, w);
				section_add(sec, l, NULL);
			}
			if (ns < 16 && cJSON_GetArraySize(cards) == 1 && is_dial(first) && !card_rows(first)) {
				later[ns] = first;
				secs[ns++] = sec;
				section_place(sec);
				continue;
			}
			if (ns < 16)
				secs[ns++] = sec;
			cJSON_ArrayForEach(c, cards)
				section_card(sec, c);
			section_place(sec);
		}
		lv_obj_update_layout(page);
		for (int i = 0; i < ns; i++) {
			int y = lv_obj_get_y(secs[i]), want = 0, own = 0;
			int cw = section_card_width(later[i], lv_obj_get_width(secs[i]));

			if (!later[i])
				continue;
			for (int j = 0; j < ns; j++)
				if (j != i && !later[j] && lv_obj_get_y(secs[j]) == y &&
				    lv_obj_get_height(secs[j]) > want)
					want = lv_obj_get_height(secs[j]);
			/* the title above the card */
			if (lv_obj_get_child_count(secs[i]))
				own = lv_obj_get_height(lv_obj_get_child(secs[i], 0)) + GAP;
			want = want > own ? want - own : 0;
			int goal = thermostat_height(cw);
			/* in the top row, with nothing next to it taller than the
			   screen: down to the bar, no empty band under it */
			int full = ROW_H(page_rows) - own;
			if (y == lv_obj_get_y(secs[0]) && want <= full && full > goal)
				goal = full;
			if (goal == full && want < full)
				want = full;
			fit_h = want;
			section_add(secs[i], make_card(secs[i], later[i], cw), later[i]);
			fit_h = 0;
			section_place(secs[i]);
		}
		return;
	}
	/* an older view: a plain list of cards, spread over the columns */
	lv_obj_t *col[4];
	int n = LV_MIN(columns, 4);

	for (int i = 0; i < n; i++)
		col[i] = flow(page, sw);
	cJSON_ArrayForEach(c, list) {
		int best = 0;

		for (int i = 1; i < n; i++) {
			lv_obj_update_layout(col[i]);
			if (lv_obj_get_height(col[i]) < lv_obj_get_height(col[best]))
				best = i;
		}
		record(make_card(col[best], c, sw), c);
		lv_obj_update_layout(col[best]);
	}
}

/*
 * The page as a grid: 12 columns over the page's width, rows a tile high.
 * Each card has its place, "position": { "x", "y", "w", "h" } in cells,
 * and gets exactly that size; empty cells stay empty.
 */
#define GRID_COLS	12

static bool is_grid_view(cJSON *view)
{
	const char *t = jstr(view, "type");

	return t && !strcmp(t, "retoon-grid");
}

/* where column col starts, inside the page's margins */
static int grid_x(int col)
{
	return (SCREEN_W - 2 * S(14) + GAP) * col / GRID_COLS;
}

static int pos_int(cJSON *pos, const char *key, int lo, int hi, int def)
{
	cJSON *v = cJSON_GetObjectItemCaseSensitive(pos, key);

	return cJSON_IsNumber(v) ? LV_CLAMP(lo, v->valueint, hi) : def;
}

/* the card at exactly its cells: grown, or cut off when it shows more */
static void fit_card(lv_obj_t *o, cJSON *cfg, int want)
{
	int have;

	if (is_graph(cfg)) {
		graph_set_height(o, want);
		return;
	}
	lv_obj_update_layout(o);
	have = lv_obj_get_height(o);
	if (have == want || is_type(cfg, "custom:retoon-app")) {
		lv_obj_set_height(o, want);
		return;
	}
	lv_obj_set_height(o, want);
	/* a tile: what it shows in the middle of the taller card */
	if (want > have && (is_type(cfg, "tile") || (is_type(cfg, "thermostat") && !is_dial(cfg))))
		lv_obj_set_style_pad_top(o, (want - have) / 2, 0);
}

static void build_grid(lv_obj_t *page, cJSON *view)
{
	cJSON *c;

	lv_obj_set_layout(page, LV_LAYOUT_NONE);
	cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(view, "cards")) {
		cJSON *pos = cJSON_GetObjectItemCaseSensitive(c, "position");
		int x = pos_int(pos, "x", 0, GRID_COLS - 1, 0);
		int w = pos_int(pos, "w", 1, GRID_COLS - x, LV_MIN(6, GRID_COLS - x));
		int y = pos_int(pos, "y", 0, MAX_ROWS - 1, 0);
		int h = pos_int(pos, "h", 1, 16, 2);
		lv_obj_t *o;

		if (fills_rows(c))
			fit_h = ROW_H(h);
		o = make_card(page, c, grid_x(x + w) - grid_x(x) - GAP);
		fit_h = 0;
		if (!o)
			continue;
		fit_card(o, c, ROW_H(h));
		lv_obj_set_pos(o, grid_x(x), y * (TILE_H + GAP));
	}
}

/*
 * A page in an older layout to the grid: where its cards are on the
 * screen, in cells. Cards that come out on the same cells (rounding) move
 * down to the first free row. view is the page in a copy of the dashboard:
 * the cards on the screen keep pointers into the one they were made from.
 */
static void convert_view(cJSON *view, lv_obj_t *page)
{
	lv_area_t pa, a;
	uint16_t used[MAX_ROWS] = { 0 };
	int pw = SCREEN_W - 2 * S(14) + GAP, rh = TILE_H + GAP;
	cJSON *list = cJSON_CreateArray();

	lv_obj_update_layout(page);
	lv_obj_get_content_coords(page, &pa);
	for (int i = 0; i < nrecs; i++) {
		struct rec *r = &recs[i];
		cJSON *c;
		int x, y, w, h;
		uint16_t mask;

		lv_obj_get_coords(r->o, &a);
		x = LV_CLAMP(0, ((a.x1 - pa.x1) * GRID_COLS + pw / 2) / pw, GRID_COLS - 1);
		w = LV_CLAMP(1, ((lv_area_get_width(&a) + GAP) * GRID_COLS + pw / 2) / pw, GRID_COLS - x);
		y = LV_CLAMP(0, (a.y1 - pa.y1 + rh / 2) / rh, MAX_ROWS - 1);
		h = LV_CLAMP(1, (lv_area_get_height(&a) + GAP + rh / 2) / rh, 16);
		mask = (uint16_t)(((1u << w) - 1) << x);
		for (bool free = false; !free && y + h <= MAX_ROWS; ) {
			free = true;
			for (int k = y; k < y + h; k++)
				free = free && !(used[k] & mask);
			if (!free)
				y++;
		}
		for (int k = y; k < y + h && k < MAX_ROWS; k++)
			used[k] |= mask;
		if (r->cfg) {
			c = cJSON_Duplicate(r->cfg, true);
			cJSON_DeleteItemFromObjectCaseSensitive(c, "grid_options");
			cJSON_DeleteItemFromObjectCaseSensitive(c, "layout_options");
			cJSON_DeleteItemFromObjectCaseSensitive(c, "position");
		} else {
			c = cJSON_CreateObject();
			cJSON_AddStringToObject(c, "type", "heading");
			cJSON_AddStringToObject(c, "heading", lv_label_get_text(r->o));
		}
		cJSON *pos = cJSON_AddObjectToObject(c, "position");

		cJSON_AddNumberToObject(pos, "x", x);
		cJSON_AddNumberToObject(pos, "y", y);
		cJSON_AddNumberToObject(pos, "w", w);
		cJSON_AddNumberToObject(pos, "h", h);
		cJSON_AddItemToArray(list, c);
	}
	nrecs = 0;
	cJSON_DeleteItemFromObjectCaseSensitive(view, "sections");
	cJSON_DeleteItemFromObjectCaseSensitive(view, "cards");
	cJSON_DeleteItemFromObjectCaseSensitive(view, "columns");
	cJSON_DeleteItemFromObjectCaseSensitive(view, "max_columns");
	cJSON_DeleteItemFromObjectCaseSensitive(view, "type");
	cJSON_AddStringToObject(view, "type", "retoon-grid");
	cJSON_AddItemToObject(view, "cards", list);
}

/* the dashboard after a change of its own (pages to grids, buttons per
   card); the old one is kept once, as <file>.<suffix> */
static bool save_dashboard(cJSON *doc, const char *suffix)
{
	char old[128];
	bool ok = false;

	char *txt = cJSON_Print(doc);
	FILE *f;

	if (!txt || system("mount -o remount,rw /mnt/data 2>/dev/null")) {
		free(txt);
		return false;
	}
	snprintf(old, sizeof(old), "%s.%s", DASHBOARD_FILE, suffix);
	if (access(old, F_OK))
		(void)!rename(DASHBOARD_FILE, old);
	f = fopen(DASHBOARD_FILE ".tmp", "w");
	if (f && fputs(txt, f) >= 0 && !fclose(f))
		ok = !rename(DASHBOARD_FILE ".tmp", DASHBOARD_FILE);
	else if (f)
		fclose(f);
	free(txt);
	sync();
	if (access("/run/toon/update.lock", F_OK))
		(void)!system("mount -o remount,ro /mnt/data 2>/dev/null");
	return ok;
}

/* ---- views and the side bar -------------------------------------------- */

static void slide_stop(void);

/* an empty page: the cards flow in rows and it scrolls down */
static lv_obj_t *new_page(lv_obj_t *parent, int h)
{
	lv_obj_t *page = box(parent, SCREEN_W, h);

	lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scroll_dir(page, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_ACTIVE);
	lv_obj_set_style_pad_all(page, S(14), 0);
	lv_obj_set_style_pad_row(page, S(14), 0);
	lv_obj_set_style_pad_column(page, S(14), 0);
	lv_obj_set_flex_flow(page, LV_FLEX_FLOW_ROW_WRAP);
	return page;
}

/* retoon/ui "pages": the page names, "|" between them, for toon-mqttd's
   page select in Home Assistant (an option per page, in order). A page
   without a title, or with the title of an earlier page, gets its number. */
static void pages_status(cJSON *list)
{
	static char nm[MAX_VIEWS][40];
	char names[1000] = "";
	size_t n = 0;
	int i = 0;
	cJSON *v;

	cJSON_ArrayForEach(v, list) {
		const char *t = jstr(v, "title");
		char *name = nm[i];
		size_t k = 0;
		bool dup = false;

		if (i == nviews)
			break;
		for (; t && *t && k < sizeof(nm[0]) - 1; t++)
			if ((unsigned char)*t >= 0x20 && !strchr("|\\\"=", *t))
				name[k++] = *t;
		/* cut off inside a UTF-8 character: that character goes */
		if (t && ((unsigned char)*t & 0xc0) == 0x80) {
			while (k && ((unsigned char)name[k - 1] & 0xc0) == 0x80)
				k--;
			if (k)
				k--;
		}
		while (k && name[k - 1] == ' ')
			k--;
		name[k] = 0;
		for (int j = 0; j < i; j++)
			dup |= !strcmp(nm[j], name);
		if (!name[0] || dup)
			snprintf(name, sizeof(nm[0]), tr("Page %d"), i + 1);
		i++;
		if (n + strlen(name) + 2 >= sizeof(names))
			break;	/* the later pages cannot be chosen from Home Assistant */
		n += snprintf(names + n, sizeof(names) - n, "%s%s", n ? "|" : "", name);
	}
	set_status("pages", names);
}

void dash_show(int view)
{
	if (!nviews)
		return;
	slide_stop();
	cur = (view + nviews) % nviews;
	for (int i = 0; i < nviews; i++) {
		if (i == cur)
			lv_obj_remove_flag(views[i], LV_OBJ_FLAG_HIDDEN);
		else
			lv_obj_add_flag(views[i], LV_OBJ_FLAG_HIDDEN);
		lv_obj_set_state(navbtn[i], LV_STATE_CHECKED, i == cur);
	}
	char buf[8];

	snprintf(buf, sizeof(buf), "%d", cur);
	set_status("view", buf);
}

/* ---- sliding from one page to the next ---------------------------------- */

#define SLIDE_MS	220

static lv_obj_t *slide_old;	/* the page that is sliding out */

/* the home page's own page for a dimmed screen ("dimmed" in the first
   view), and the layer over it that takes the touch that wakes it */
static lv_obj_t *dimpage, *shield;
static bool shield_down;

static void slide_x(void *o, int32_t x)
{
	lv_obj_set_x(o, x);
}

static void slide_stop(void)
{
	if (!slide_old)
		return;
	lv_anim_delete(slide_old, slide_x);
	lv_obj_add_flag(slide_old, LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_x(slide_old, 0);
	slide_old = NULL;
	lv_anim_delete(views[cur], slide_x);
	lv_obj_set_x(views[cur], 0);
}

static void slide_done(lv_anim_t *a)
{
	(void)a;
	slide_stop();
}

static void slide_anim(lv_obj_t *o, int32_t from, int32_t to, bool last)
{
	lv_anim_t a;

	lv_anim_init(&a);
	lv_anim_set_var(&a, o);
	lv_anim_set_exec_cb(&a, slide_x);
	lv_anim_set_values(&a, from, to);
	lv_anim_set_duration(&a, SLIDE_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	if (last)
		lv_anim_set_completed_cb(&a, slide_done);
	lv_anim_start(&a);
}

/* show the next (dir 1) or previous (dir -1) page, sliding it in */
static void dash_slide(int dir)
{
	int old = cur;

	slide_stop();
	dash_show(cur + dir);
	if (!page_anim || cur == old)
		return;
	slide_old = views[old];
	lv_obj_remove_flag(slide_old, LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_x(views[cur], dir * SCREEN_W);
	slide_anim(slide_old, 0, -dir * SCREEN_W, false);
	slide_anim(views[cur], dir * SCREEN_W, 0, true);
}

/* the touch that wakes a dimmed page does nothing but that: it shows the
   home page, and the rest of the touch goes to the layer, not the page */
static void on_shield(lv_event_t *ev)
{
	if (lv_event_get_code(ev) == LV_EVENT_PRESSED) {
		shield_down = true;
		dash_dim(false);
		dash_show(0);
	} else {
		shield_down = false;
		lv_obj_delete_async(shield);
		shield = NULL;
	}
}

bool dash_dim(bool on)
{
	if (!on) {
		if (dimpage)
			lv_obj_add_flag(dimpage, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(bar, LV_OBJ_FLAG_HIDDEN);
		if (shield && !shield_down) {
			lv_obj_delete(shield);
			shield = NULL;
		}
		return false;
	}
	if (!dimpage)
		return false;
	slide_stop();
	lv_obj_remove_flag(dimpage, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(dimpage);
	lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
	if (!shield) {
		shield = lv_obj_create(lv_layer_top());
		lv_obj_remove_style_all(shield);
		lv_obj_set_size(shield, SCREEN_W, SCREEN_H);
		lv_obj_add_flag(shield, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb(shield, on_shield, LV_EVENT_PRESSED, NULL);
		lv_obj_add_event_cb(shield, on_shield, LV_EVENT_RELEASED, NULL);
		lv_obj_add_event_cb(shield, on_shield, LV_EVENT_PRESS_LOST, NULL);
	}
	t_tick = 0;	/* its clock at once */
	return true;
}

int dash_current(void)
{
	return nviews ? cur : 0;
}

static void on_nav(lv_event_t *ev)
{
	dash_show((intptr_t)lv_event_get_user_data(ev));
}

static void on_gesture(lv_event_t *ev)
{
	lv_indev_t *indev = lv_indev_active();
	lv_dir_t dir = lv_indev_get_gesture_dir(indev);

	(void)ev;
	if (dir != LV_DIR_LEFT && dir != LV_DIR_RIGHT)
		return;
	/* the rest of this touch is the swipe: no click on the card where it
	   started (that opened an app or toggled a light after the slide) */
	lv_indev_wait_release(indev);
	dash_slide(dir == LV_DIR_LEFT ? 1 : -1);
}

static cJSON *read_json(const char *path)
{
	FILE *f = fopen(path, "r");
	char *buf;
	long n;
	cJSON *j;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	rewind(f);
	buf = malloc(n + 1);
	n = fread(buf, 1, n, f);
	buf[n] = 0;
	fclose(f);
	j = cJSON_Parse(buf);
	free(buf);
	return j;
}

static time_t mtime(const char *path)
{
	struct stat sb;

	return stat(path, &sb) ? 0 : sb.st_mtime;
}

/* the hot water tile of the standard pages: only for a boiler that has it */
static void drop_entity_in(cJSON *cards, const char *id)
{
	cJSON *c, *next;

	for (c = cards ? cards->child : NULL; c; c = next) {
		const char *e = jstr(c, "entity");

		next = c->next;
		if (e && !strcmp(e, id))
			cJSON_Delete(cJSON_DetachItemViaPointer(cards, c));
	}
}

/* a card of the standard pages that this Toon has nothing for */
static void drop_entity(cJSON *list, const char *id)
{
	cJSON *v, *sec;

	cJSON_ArrayForEach(v, list) {
		drop_entity_in(cJSON_GetObjectItemCaseSensitive(v, "cards"), id);
		cJSON_ArrayForEach(sec, cJSON_GetObjectItemCaseSensitive(v, "sections"))
			drop_entity_in(cJSON_GetObjectItemCaseSensitive(sec, "cards"), id);
	}
}

/* "Presets and week program under the thermostat" was one setting for every
   card (ui.conf thermostat_buttons=0: off); now each thermostat card has its
   own choice. Off becomes an empty features list in every card of ReToon's
   own thermostat that had no choice yet. */
static bool buttons_per_card(cJSON *j)
{
	bool changed = false;
	cJSON *k;

	if (cJSON_IsObject(j)) {
		const char *t = jstr(j, "type"), *e = jstr(j, "entity"), *st = jstr(j, "style");

		if (t && !strcmp(t, "thermostat") && e && !strcmp(e, "toon.thermostat") &&
		    (!st || !strcmp(st, "dial") || !strcmp(st, "classic")) &&
		    !cJSON_GetObjectItemCaseSensitive(j, "features")) {
			cJSON_AddItemToObject(j, "features", cJSON_CreateArray());
			changed = true;
		}
	}
	if (cJSON_IsObject(j) || cJSON_IsArray(j))
		for (k = j->child; k; k = k->next)
			changed |= buttons_per_card(k);
	return changed;
}

/* the setting goes from ui.conf once its cards carry it */
static void drop_ui_key(const char *key)
{
	char cmd[320];

	snprintf(cmd, sizeof(cmd), "mount -o remount,rw /mnt/data 2>/dev/null && "
		 "sed -i '/^%s=/d' %s; sync; [ -e /run/toon/update.lock ] || "
		 "mount -o remount,ro /mnt/data 2>/dev/null", key, UI_CONF);
	(void)!system(cmd);
}

static void drop_hot_water(cJSON *list)
{
	drop_entity(list, "toon.hot_water");
}

void dash_load(void)
{
	cJSON *v, *list;
	char buf[32];
	int keep = cur;
	cJSON *conv = NULL;	/* the dashboard with its pages turned into grids */
	int vi = 0;

	while (cards) {
		struct card *c = cards;

		cards = c->next;
		free(c);
	}
	graph_reset();
	energy_reset();
	ent_unbind_all();
	slide_old = NULL;	/* its animation goes with the page */
	if (dimpage)
		lv_obj_delete(dimpage);	/* not on the content: on the screen */
	dimpage = NULL;
	clock_fonts_new();	/* the clocks are gone */
	lv_obj_remove_flag(bar, LV_OBJ_FLAG_HIDDEN);
	if (shield) {
		lv_obj_delete(shield);
		shield = NULL;
		shield_down = false;
	}
	lv_obj_clean(content);
	lv_obj_clean(nav);
	cJSON_Delete(doc);
	nviews = 0;
	std_pages = false;

	/* for pages that do not say: what older versions kept in ui.conf, or two */
	std_columns = 2;
	if (!file_value(UI_CONF, "columns", buf, sizeof(buf)))
		std_columns = LV_CLAMP(1, atoi(buf), 4);
	/* too slow on a Toon 1 to look smooth, so off there unless chosen */
	if (file_value(UI_CONF, "page_anim", buf, sizeof(buf)))
		page_anim = !is_toon1();
	else
		page_anim = atoi(buf) != 0;
	th_buttons = file_value(UI_CONF, "thermostat_buttons", buf, sizeof(buf)) || atoi(buf) != 0;
	doc = read_json(DASHBOARD_FILE);
	list = cJSON_GetObjectItemCaseSensitive(doc, "views");
	if (!cJSON_IsArray(list) || !cJSON_GetArraySize(list)) {
		cJSON_Delete(doc);
		doc = read_json(DEFAULT_DASHBOARD);
		list = cJSON_GetObjectItemCaseSensitive(doc, "views");
		std_pages = true;
		hw_seen = !access(HW_FLAG, F_OK);
		if (!hw_seen)
			drop_hot_water(list);
		/* a Toon 1 measures only the temperature */
		if (is_toon1()) {
			drop_entity(list, "toon.humidity");
			drop_entity(list, "toon.air_quality");
			drop_entity(list, "toon.illuminance");
		}
	} else if (!th_buttons) {
		/* once: the old setting for every card into the cards themselves
		   (the standard pages keep following it until they are saved) */
		if (!buttons_per_card(doc) || save_dashboard(doc, "before-buttons")) {
			drop_ui_key("thermostat_buttons");
			th_buttons = true;
		}
	}

	cJSON_ArrayForEach(v, list) {
		const char *title = jstr(v, "title"), *icon = jstr(v, "icon");
		lv_obj_t *b, *l, *page;

		if (nviews == MAX_VIEWS) {
			fprintf(stderr, "dash: more than %d pages, the rest is left out\n", MAX_VIEWS);
			break;
		}
		b = lv_button_create(nav);
		lv_obj_set_size(b, S(96), BAR_H - S(8));
		lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
		lv_obj_set_style_bg_color(b, lv_color_hex(C_ACCENT), LV_STATE_CHECKED);
		lv_obj_set_style_bg_opa(b, 60, LV_STATE_CHECKED);
		lv_obj_set_style_shadow_width(b, 0, 0);
		lv_obj_set_style_radius(b, S(12), 0);
		lv_obj_set_style_pad_all(b, 0, 0);
		lv_obj_add_event_cb(b, on_nav, LV_EVENT_CLICKED, (void *)(intptr_t)nviews);
		l = text(b, i_mid, C_TEXT, mdi(icon, "mdi:view-dashboard"));
		lv_obj_align(l, LV_ALIGN_TOP_MID, 0, S(6));
		l = text(b, f_small, C_MUTED, title ? title : "");
		lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
		lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_width(l, S(92));
		lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -S(4));
		navbtn[nviews] = b;

		page = new_page(content, SCREEN_H - BAR_H);
		views[nviews++] = page;
		recording = !is_grid_view(v);
		nrecs = 0;
		build_view(page, v);
		if (recording) {
			if (!conv)
				conv = cJSON_Duplicate(doc, true);
			convert_view(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(conv, "views"), vi), page);
		}
		recording = false;
		vi++;
	}
	v = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(list, 0), "dimmed");
	if (nviews && cJSON_IsObject(v)) {
		/* the whole screen, without the bar: 8 rows, the margins
		   above and below a little smaller to fit them */
		dimpage = new_page(lv_obj_get_parent(content), SCREEN_H);
		lv_obj_set_style_pad_ver(dimpage, LV_MAX(0, (SCREEN_H - ROW_H(DIM_ROWS)) / 2), 0);
		/* over the pages: covered where it has no cards */
		lv_obj_set_style_bg_color(dimpage, lv_color_hex(C_BG), 0);
		lv_obj_set_style_bg_opa(dimpage, LV_OPA_COVER, 0);
		lv_obj_add_flag(dimpage, LV_OBJ_FLAG_HIDDEN);
		page_rows = DIM_ROWS;
		recording = !is_grid_view(v);
		nrecs = 0;
		build_view(dimpage, v);
		if (recording) {
			if (!conv)
				conv = cJSON_Duplicate(doc, true);
			convert_view(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(
				cJSON_GetObjectItemCaseSensitive(conv, "views"), 0), "dimmed"), dimpage);
		}
		recording = false;
		page_rows = FIT_ROWS;
	}
	pages_status(list);
	dash_show(keep < nviews ? keep : 0);
	mt_dash = mtime(DASHBOARD_FILE);
	mt_ui = mtime(UI_CONF);
	mt_ha = mtime(HA_CONF);
	t_tick = 0;
	ent_flush();
	/* saved after mt_dash: the next tick builds the pages as grids */
	if (conv && !std_pages)
		save_dashboard(conv, "before-grid");
	cJSON_Delete(conv);
}

static void on_wifi(lv_event_t *ev) { (void)ev; wifi_open(false); }
static void on_info(lv_event_t *ev) { (void)ev; info_open(); }

static void tick_clocks(lv_obj_t *o, const char *hm, const char *date)
{
	for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
		lv_obj_t *ch = lv_obj_get_child(o, i);

		if (lv_obj_has_flag(ch, LV_OBJ_FLAG_USER_2)) {
			aclock_tick(ch);
		} else if (lv_obj_has_flag(ch, LV_OBJ_FLAG_USER_1)) {
			set_text(lv_obj_get_child(ch, 0), hm);
			set_text(lv_obj_get_child(ch, 1), date);
		} else if (lv_obj_get_child_count(ch) && !lv_obj_check_type(ch, &lv_button_class)) {
			tick_clocks(ch, hm, date);
		}
	}
}

void dash_poll(void)
{
	static const char *wifi_icons[] = { "mdi:wifi-strength-1", "mdi:wifi-strength-2",
					    "mdi:wifi-strength-3", "mdi:wifi-strength-4" };
	uint32_t now = now_ms();
	char hm[8], date[48];
	time_t t = time(NULL);
	struct tm tm;
	int sig;

	if (now - t_tick < 1000 && t_tick)
		return;
	t_tick = now;

	if (mtime(DASHBOARD_FILE) != mt_dash || mtime(UI_CONF) != mt_ui ||
	    (std_pages && !access(HW_FLAG, F_OK) != hw_seen) || energy_changed()) {
		dash_load();
		ha_restart();
		mqtt_restart();
	} else if (mtime(HA_CONF) != mt_ha) {
		mt_ha = mtime(HA_CONF);
		ha_restart();
	}

	localtime_r(&t, &tm);
	strftime(hm, sizeof(hm), "%H:%M", &tm);
	date_text(date, sizeof(date), &tm);
	set_text(clock_lbl, tm.tm_year > 120 ? hm : "--:--");
	if (cur < nviews)
		tick_clocks(views[cur], tm.tm_year > 120 ? hm : "--:--", tm.tm_year > 120 ? date : "");
	if (dimpage && !lv_obj_has_flag(dimpage, LV_OBJ_FLAG_HIDDEN))
		tick_clocks(dimpage, tm.tm_year > 120 ? hm : "--:--", tm.tm_year > 120 ? date : "");

	sig = wifi_signal();
	if (!wifi_connected()) {
		set_text(wifi_icon, mdi("mdi:wifi-off", "x"));
		lv_obj_set_style_text_color(wifi_icon, lv_color_hex(C_BAD), 0);
	} else {
		set_text(wifi_icon, mdi(wifi_icons[sig > -55 ? 3 : sig > -67 ? 2 : sig > -78 ? 1 : 0], "w"));
		lv_obj_set_style_text_color(wifi_icon, lv_color_hex(C_MUTED), 0);
	}
	lv_obj_set_style_text_color(ha_icon, lv_color_hex(ha_ready() ? C_OK :
		!strcmp(ha_status(), "Not configured") ? 0x4a5262 : C_WARN), 0);
	{
		uint32_t c = mqtt_state() == 2 ? C_OK : mqtt_state() ? C_WARN : 0x4a5262;

		lv_obj_set_style_text_color(mqtt_icon, lv_color_hex(c), 0);
		lv_obj_set_style_border_color(mqtt_icon, lv_color_hex(c), 0);
	}
}

void dash_init(lv_obj_t *parent)
{
	/* the page: above the bar, inside its margins (S(14)) */
	tile_h = (SCREEN_H - BAR_H - 2 * S(14) - (FIT_ROWS - 1) * GAP) / FIT_ROWS;
	lv_obj_t *b;

	bar = box(parent, SCREEN_W, BAR_H);
	lv_obj_set_pos(bar, 0, SCREEN_H - BAR_H);
	lv_obj_set_style_bg_color(bar, lv_color_hex(C_SIDEBAR), 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

	/* the pages, left; more than fit can be dragged sideways */
	nav = box(bar, SCREEN_W - STATUS_W, BAR_H);
	lv_obj_add_flag(nav, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scroll_dir(nav, LV_DIR_HOR);
	lv_obj_set_scrollbar_mode(nav, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_left(nav, S(8), 0);
	lv_obj_set_style_pad_column(nav, S(4), 0);

	/* status, right; from the corner: clock, WiFi, Home Assistant, MQTT */
	clock_lbl = text(bar, f_head, C_TEXT, "--:--");
	lv_obj_align(clock_lbl, LV_ALIGN_RIGHT_MID, -S(16), 0);

	b = box(bar, S(50), BAR_H);
	lv_obj_align(b, LV_ALIGN_RIGHT_MID, -S(86), 0);
	lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(b, on_wifi, LV_EVENT_CLICKED, NULL);
	wifi_icon = text(b, i_mid, C_MUTED, mdi("mdi:wifi-strength-4", "w"));
	lv_obj_center(wifi_icon);

	b = box(bar, S(50), BAR_H);
	lv_obj_align(b, LV_ALIGN_RIGHT_MID, -S(136), 0);
	lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(b, on_info, LV_EVENT_CLICKED, NULL);
	ha_icon = text(b, i_mid, C_MUTED, mdi("mdi:home-assistant", "h"));
	lv_obj_center(ha_icon);

	/* MQTT has no icon of its own that is not mistaken for WiFi: a word */
	b = box(bar, S(60), BAR_H);
	lv_obj_align(b, LV_ALIGN_RIGHT_MID, -S(188), 0);
	lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(b, on_info, LV_EVENT_CLICKED, NULL);
	mqtt_icon = text(b, f_small, C_MUTED, "MQTT");
	lv_obj_set_style_border_width(mqtt_icon, 2, 0);
	lv_obj_set_style_border_color(mqtt_icon, lv_color_hex(0x4a5262), 0);
	lv_obj_set_style_radius(mqtt_icon, S(6), 0);
	lv_obj_set_style_pad_hor(mqtt_icon, S(5), 0);
	lv_obj_set_style_pad_ver(mqtt_icon, 2, 0);
	lv_obj_center(mqtt_icon);

	content = box(parent, SCREEN_W, SCREEN_H - BAR_H);
	lv_obj_set_pos(content, 0, 0);
	lv_obj_add_event_cb(lv_screen_active(), on_gesture, LV_EVENT_GESTURE, NULL);
}
