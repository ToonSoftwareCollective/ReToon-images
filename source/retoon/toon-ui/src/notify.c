// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * toon-ui: notifications. A message from Home Assistant (over MQTT, through
 * toon-mqttd), from an app (retoon.notify) or from the web interface shows
 * as a banner at the top of the screen, or as a popup in the middle when it
 * needs an answer. It arrives as JSON on retoon/notify/set (plain text is
 * taken as the message), or from the web interface or an app:
 *
 *   title, message   the text (a message alone is fine)
 *   level            "info" (the default), "warning" or "alert"
 *   icon             "mdi:name" (default: one for the level)
 *   style            "banner" or "popup"; default: a popup for an alert,
 *                    with buttons or with timeout 0, a banner otherwise
 *   timeout          seconds until it goes by itself, 0 = until it is tapped
 *                    (default: 10 for a banner, 0 for a popup)
 *   buttons          up to three labels; a tap closes the popup and is
 *                    reported
 *   sound            "none", "beep", "chime", "alarm" (again every few
 *                    seconds until the message is closed, at most two
 *                    minutes) or the name of a WAV file in SOUND_DIR;
 *                    default: none for info, beep for a warning, chime for
 *                    an alert
 *   volume           0..100 (default: the sound setting)
 *   tag              a new message with the same tag takes this one's
 *                    place; {"tag": "x", "dismiss": true} takes it away
 *   wake             false: a dimmed screen stays dimmed
 *   camera           "camera.door": a snapshot of that Home Assistant camera
 *                    in the popup (always a popup then), fetched once over
 *                    the Toon's own connection when it opens and freed when
 *                    it closes. An app only for a camera its manifest lists.
 *
 * A "{s}" in the title or message shows the seconds left until it goes by
 * itself (a timeout), counting down: "Restart in {s} s".
 *
 * Without a title and a message only the sound plays. A banner that is
 * tapped opens as a popup. A tap on a button or on close is reported as a
 * JSON message on retoon/notify/event, for toon-mqttd to pass on to Home
 * Assistant, or to the app that sent the message.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "ui.h"

#define EVENTS_TOPIC	"retoon/notify/event"
#define SOUND_DIR	"/mnt/data/toon-os/sounds"
#define SOUND_BIN	"/usr/bin/toon-sound"
#define MAX_POPUPS	5
#define MAX_BUTTONS	3
#define BANNER_S	10	/* a banner's default time on the screen */
#define ALARM_EVERY_MS	3000
#define ALARM_MAX_MS	(2 * 60 * 1000)

enum { L_INFO, L_WARNING, L_ALERT };

struct note {
	char title[96], message[480], icon[64], tag[64], sound[48], camera[128];
	char buttons[MAX_BUTTONS][32];
	int nbuttons, level, timeout, volume;
	bool popup;
	notify_cb cb;		/* an app's: told once, at the end */
	void *user;
	uint32_t until;		/* 0: stays */
	uint32_t alarm_end, alarm_next;
};

static struct note *banner;
static lv_obj_t *banner_obj;
static struct note *popups[MAX_POPUPS];	/* [0] is on the screen */
static int npopups;
static lv_obj_t *popup_obj;
static bool sound_busy;

/* the labels on the screen that count down ("{s}"), and their text */
static struct live {
	lv_obj_t *l;
	const char *text;
	struct note *n;
	int shown;
} live[2][2];		/* [0] the banner's, [1] the popup's */

static const uint32_t level_color[] = { C_ACCENT, C_WARN, C_BAD };
static const char *const level_icon[] = { "mdi:information", "mdi:alert", "mdi:alert-octagon" };

/* ---- sound ---------------------------------------------------------------- */

static void sound_done(int status, char *out, void *user)
{
	(void)status;
	(void)out;
	(void)user;
	sound_busy = false;
}

static bool valid_name(const char *s)
{
	if (!*s)
		return false;
	for (; *s; s++)
		if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_')
			return false;
	return true;
}

/* toon-sound takes one sound at a time: one that comes while another plays
   is left out instead of piling up */
void notify_sound(const char *name, int volume)
{
	char vol[8], path[128];
	char *argv[6];
	int n = 0;

	if (!name || !*name || !strcmp(name, "none") || sound_busy)
		return;
	argv[n++] = SOUND_BIN;
	if (volume >= 0) {
		snprintf(vol, sizeof(vol), "%d", LV_CLAMP(0, volume, 100));
		argv[n++] = "-v";
		argv[n++] = vol;
	}
	if (!strcmp(name, "beep") || !strcmp(name, "alarm")) {
		argv[n++] = "beep";
	} else if (!strcmp(name, "chime")) {
		argv[n++] = "test";
	} else if (valid_name(name)) {
		snprintf(path, sizeof(path), SOUND_DIR "/%s.wav", name);
		if (access(path, R_OK))
			return;
		argv[n++] = "play";
		argv[n++] = path;
	} else {
		return;
	}
	argv[n] = NULL;
	if (!job_run(argv, sound_done, NULL))
		sound_busy = true;
}

/* ---- what happened to a message ------------------------------------------- */

/* the end of a message: tell the app, or toon-mqttd when a person did it */
static void finish(struct note *n, const char *event, const char *button)
{
	if (n->cb) {
		n->cb(n->user, event, button);
	} else if (strcmp(event, "gone")) {
		cJSON *o = cJSON_CreateObject();
		char *s;

		cJSON_AddStringToObject(o, "event_type", event);
		if (button)
			cJSON_AddStringToObject(o, "button", button);
		cJSON_AddStringToObject(o, "tag", n->tag);
		cJSON_AddStringToObject(o, "title", n->title);
		s = cJSON_PrintUnformatted(o);
		cJSON_Delete(o);
		/* nobody listening (toon-mqttd not running): nobody to tell */
		if (s)
			bus_send(EVENTS_TOPIC, s);
		free(s);
	}
	free(n);
}

/* ---- the banner ------------------------------------------------------------ */

static void show_popup(void);
static void add_popup(struct note *n, bool first);

/* also from the banner's own click: deleted once that event is over */
static void banner_close(void)
{
	if (banner_obj) {
		/* hidden at once: no second tap on it before it is gone */
		lv_obj_add_flag(banner_obj, LV_OBJ_FLAG_HIDDEN);
		lv_obj_delete_async(banner_obj);
	}
	banner_obj = NULL;
	memset(live[0], 0, sizeof(live[0]));
}

/* a tap opens the banner as a popup, to read all of it */
static void banner_tap(lv_event_t *ev)
{
	struct note *n = banner;

	(void)ev;
	if (!n)
		return;
	banner = NULL;
	banner_close();
	n->popup = true;
	n->timeout = 0;
	add_popup(n, true);
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
	lv_label_set_text(l, text);
	return l;
}

/* seconds until it goes by itself (before it is timed: its timeout) */
static int secs_left(const struct note *n)
{
	int32_t ms = n->until ? (int32_t)(n->until - now_ms()) : n->timeout * 1000;

	return ms > 0 ? (ms + 999) / 1000 : 0;
}

/* text with "{s}" filled in */
static const char *counted(const struct note *n, const char *text)
{
	static char buf[600];
	const char *p = strstr(text, "{s}");

	if (!p)
		return text;
	snprintf(buf, sizeof(buf), "%.*s%d%s", (int)(p - text), text, secs_left(n), p + 3);
	return buf;
}

/* a label whose text may count down; which: 0 banner, 1 popup */
static lv_obj_t *text_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
			    struct note *n, const char *text, int which)
{
	lv_obj_t *l = label(parent, font, color, counted(n, text));

	if (strstr(text, "{s}") && n->timeout > 0)
		for (int i = 0; i < 2; i++)
			if (!live[which][i].l) {
				live[which][i] = (struct live){ l, text, n, secs_left(n) };
				break;
			}
	return l;
}

static void live_poll(void)
{
	for (int w = 0; w < 2; w++)
		for (int i = 0; i < 2; i++) {
			struct live *v = &live[w][i];

			if (v->l && secs_left(v->n) != v->shown) {
				v->shown = secs_left(v->n);
				lv_label_set_text(v->l, counted(v->n, v->text));
			}
		}
}

static void show_banner(void)
{
	struct note *n = banner;
	int w = SCREEN_W - S(48), h = S(88), tx = S(84);
	lv_obj_t *b, *l;
	lv_anim_t a;

	banner_close();
	b = banner_obj = lv_obj_create(lv_layer_sys());
	lv_obj_remove_style_all(b);
	lv_obj_set_size(b, w, h);
	lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD_HI), 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(b, S(12), 0);
	lv_obj_set_style_border_side(b, LV_BORDER_SIDE_LEFT, 0);
	lv_obj_set_style_border_width(b, S(8), 0);
	lv_obj_set_style_border_color(b, lv_color_hex(level_color[n->level]), 0);
	lv_obj_set_style_shadow_width(b, S(24), 0);
	lv_obj_set_style_shadow_opa(b, LV_OPA_50, 0);
	lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_event_cb(b, banner_tap, LV_EVENT_CLICKED, NULL);
	lv_obj_align(b, LV_ALIGN_TOP_MID, 0, S(12));

	l = label(b, i_mid, level_color[n->level], mdi(n->icon, level_icon[n->level]));
	lv_obj_align(l, LV_ALIGN_LEFT_MID, S(28), 0);
	if (n->title[0]) {
		l = text_label(b, f_head, C_TEXT, n, n->title, 0);
		lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
		lv_obj_set_width(l, w - tx - S(24));
		lv_obj_set_pos(l, tx, n->message[0] ? S(14) : (h - lv_font_get_line_height(f_head)) / 2);
	}
	if (n->message[0]) {
		l = text_label(b, n->title[0] ? f_body : f_head, n->title[0] ? C_MUTED : C_TEXT, n, n->message, 0);
		lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
		lv_obj_set_width(l, w - tx - S(24));
		lv_obj_set_pos(l, tx, n->title[0] ? S(48) : (h - lv_font_get_line_height(f_head)) / 2);
	}

	/* slides in from above */
	lv_anim_init(&a);
	lv_anim_set_var(&a, b);
	lv_anim_set_values(&a, -h, S(12));
	lv_anim_set_duration(&a, 250);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
	lv_anim_start(&a);
	/* an open popup stays in front */
	if (popup_obj)
		lv_obj_move_foreground(popup_obj);
}

/* ---- popups ---------------------------------------------------------------- */

static void popup_close(void)
{
	if (popup_obj) {
		/* hidden at once: no second tap on it before it is gone */
		lv_obj_add_flag(popup_obj, LV_OBJ_FLAG_HIDDEN);
		lv_obj_delete_async(popup_obj);
	}
	popup_obj = NULL;
	memset(live[1], 0, sizeof(live[1]));
}

/* the one on the screen goes; the next one in line comes */
static void popup_done(const char *event, const char *button)
{
	struct note *n;

	if (!npopups)
		return;
	n = popups[0];
	memmove(popups, popups + 1, (npopups - 1) * sizeof(*popups));
	npopups--;
	popup_close();
	finish(n, event, button);
	if (npopups)
		show_popup();
}

static void popup_button(lv_event_t *ev)
{
	intptr_t i = (intptr_t)lv_event_get_user_data(ev);
	char label_text[32];

	if (!npopups)
		return;
	if (i < 0) {
		popup_done("dismissed", NULL);
		return;
	}
	snprintf(label_text, sizeof(label_text), "%s", popups[0]->buttons[i]);
	popup_done("button", label_text);
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, uint32_t color, intptr_t id)
{
	lv_obj_t *b = lv_button_create(parent), *l;

	lv_obj_set_size(b, S(180), S(56));
	lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_add_event_cb(b, popup_button, LV_EVENT_CLICKED, (void *)id);
	l = label(b, f_body, C_TEXT, text);
	/* a fixed width: the dots of a label that is too long need one */
	lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
	lv_obj_set_width(l, S(160));
	lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_center(l);
	return b;
}

static void show_popup(void)
{
	struct note *n = popups[0];
	int w = S(640);
	lv_obj_t *card, *row, *l;

	popup_close();
	/* the backdrop takes every touch away from what is under it */
	popup_obj = lv_obj_create(lv_layer_sys());
	lv_obj_remove_style_all(popup_obj);
	lv_obj_set_size(popup_obj, SCREEN_W, SCREEN_H);
	lv_obj_set_style_bg_color(popup_obj, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(popup_obj, LV_OPA_60, 0);
	lv_obj_add_flag(popup_obj, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(popup_obj, LV_OBJ_FLAG_SCROLLABLE);

	card = lv_obj_create(popup_obj);
	lv_obj_remove_style_all(card);
	lv_obj_set_width(card, w);
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_set_style_max_height(card, SCREEN_H - S(48), 0);
	lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
	lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(card, S(16), 0);
	lv_obj_set_style_border_side(card, LV_BORDER_SIDE_TOP, 0);
	lv_obj_set_style_border_width(card, S(8), 0);
	lv_obj_set_style_border_color(card, lv_color_hex(level_color[n->level]), 0);
	lv_obj_set_style_pad_all(card, S(28), 0);
	lv_obj_set_style_pad_row(card, S(18), 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_center(card);

	/* icon and title */
	row = lv_obj_create(card);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, S(16), 0);
	label(row, i_big, level_color[n->level], mdi(n->icon, level_icon[n->level]));
	l = text_label(row, f_mid, C_TEXT, n, n->title[0] ? n->title : n->message[0] ? n->message :
		       n->camera, 1);
	lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
	lv_obj_set_flex_grow(l, 1);
	if (n->nbuttons) {
		/* closing without an answer */
		lv_obj_t *x = lv_button_create(row);

		lv_obj_set_size(x, S(56), S(56));
		lv_obj_set_style_bg_opa(x, LV_OPA_TRANSP, 0);
		lv_obj_set_style_shadow_width(x, 0, 0);
		lv_obj_add_event_cb(x, popup_button, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
		lv_obj_center(label(x, i_mid, C_MUTED, mdi("mdi:close", "x")));
	}

	if (n->title[0] && n->message[0]) {
		l = text_label(card, f_body, C_TEXT, n, n->message, 1);
		lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(l, LV_PCT(100));
	}

	/* the camera's snapshot: 16:9 in the width of the card, a grey area
	   until it is in */
	if (n->camera[0]) {
		lv_obj_t *pic = apps_camera_picture(card, n->camera, w - 2 * S(28), S(288));

		if (pic) {
			lv_obj_set_style_bg_color(pic, lv_color_hex(C_CARD_HI), 0);
			lv_obj_set_style_bg_opa(pic, LV_OPA_COVER, 0);
			lv_obj_set_style_radius(pic, S(8), 0);
			lv_obj_set_style_clip_corner(pic, true, 0);
		}
	}

	row = lv_obj_create(card);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, S(16), 0);
	lv_obj_set_style_pad_top(row, S(8), 0);
	if (!n->nbuttons)
		button(row, tr("Close"), C_CARD_HI, -1);
	for (int i = 0; i < n->nbuttons; i++)
		button(row, n->buttons[i], i ? C_CARD_HI : level_color[n->level], i);

	n->until = n->timeout > 0 ? now_ms() + n->timeout * 1000u : 0;
	if (!strcmp(n->sound, "alarm") && !n->alarm_end) {
		n->alarm_end = now_ms() + ALARM_MAX_MS;
		n->alarm_next = 0;
	}
}

/* in line; first: in front of the one on the screen */
static void add_popup(struct note *n, bool first)
{
	if (npopups == MAX_POPUPS)	/* the newest one waiting makes room */
		finish(popups[--npopups], "gone", NULL);
	if (first) {
		memmove(popups + 1, popups, npopups * sizeof(*popups));
		popups[0] = n;
		npopups++;
		show_popup();
	} else {
		popups[npopups++] = n;
		if (npopups == 1)
			show_popup();
	}
}

/* ---- incoming ------------------------------------------------------------ */

static void str(cJSON *o, const char *key, char *out, size_t len)
{
	cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

	if (cJSON_IsString(v))
		snprintf(out, len, "%s", v->valuestring);
	else if (cJSON_IsNumber(v))
		snprintf(out, len, "%g", v->valuedouble);
}

/* the message with this tag, wherever it is, without telling anyone */
static void drop_tag(const char *tag)
{
	if (!tag[0])
		return;
	if (banner && !strcmp(banner->tag, tag)) {
		finish(banner, "gone", NULL);
		banner = NULL;
		banner_close();
	}
	for (int i = 0; i < npopups; i++)
		if (!strcmp(popups[i]->tag, tag)) {
			if (!i) {
				popup_done("gone", NULL);
			} else {
				finish(popups[i], "gone", NULL);
				memmove(popups + i, popups + i + 1, (npopups - i - 1) * sizeof(*popups));
				npopups--;
			}
			break;
		}
}

bool notify_show(const char *json, notify_cb cb, void *user)
{
	cJSON *o = cJSON_Parse(json), *v;
	struct note *n;
	char buf[16] = "", style[16] = "";
	bool wake = true, dismiss;

	if (!cJSON_IsObject(o)) {
		cJSON_Delete(o);
		return false;
	}
	n = calloc(1, sizeof(*n));
	n->cb = cb;
	n->user = user;
	n->volume = -1;
	n->timeout = -1;
	str(o, "title", n->title, sizeof(n->title));
	str(o, "message", n->message, sizeof(n->message));
	str(o, "icon", n->icon, sizeof(n->icon));
	str(o, "tag", n->tag, sizeof(n->tag));
	str(o, "sound", n->sound, sizeof(n->sound));
	str(o, "camera", n->camera, sizeof(n->camera));
	str(o, "level", buf, sizeof(buf));
	n->level = !strcmp(buf, "alert") ? L_ALERT : !strcmp(buf, "warning") ? L_WARNING : L_INFO;
	str(o, "style", style, sizeof(style));
	if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(o, "timeout")))
		n->timeout = LV_MAX(0, v->valueint);
	if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(o, "volume")))
		n->volume = LV_CLAMP(0, v->valueint, 100);
	if (cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(o, "wake")))
		wake = false;
	dismiss = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "dismiss"));
	cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(o, "buttons"))
		if (cJSON_IsString(v) && v->valuestring[0] && n->nbuttons < MAX_BUTTONS)
			snprintf(n->buttons[n->nbuttons++], sizeof(n->buttons[0]), "%s", v->valuestring);
	cJSON_Delete(o);

	if (dismiss) {
		drop_tag(n->tag);
		finish(n, "gone", NULL);
		return true;
	}
	if (!n->sound[0])
		snprintf(n->sound, sizeof(n->sound), "%s",
			 n->level == L_ALERT ? "chime" : n->level == L_WARNING ? "beep" : "none");
	/* only a sound */
	if (!n->title[0] && !n->message[0] && !n->camera[0]) {
		notify_sound(strcmp(n->sound, "alarm") ? n->sound : "beep", n->volume);
		finish(n, "gone", NULL);
		return true;
	}

	if (!strcmp(style, "popup"))
		n->popup = true;
	else if (!strcmp(style, "banner"))
		n->popup = false;
	else
		n->popup = n->level == L_ALERT || n->nbuttons || !n->timeout;
	/* a picture only fits in a popup */
	if (n->camera[0])
		n->popup = true;
	/* a banner has no room for buttons */
	if (!n->popup)
		n->nbuttons = 0;
	if (n->timeout < 0)
		n->timeout = n->popup ? 0 : BANNER_S;

	if (wake)
		lv_display_trigger_activity(NULL);
	/* in the place of an earlier one with the same tag */
	if (n->tag[0] && n->popup)
		for (int i = 0; i < npopups; i++)
			if (!strcmp(popups[i]->tag, n->tag)) {
				finish(popups[i], "gone", NULL);
				popups[i] = n;
				if (!i)
					show_popup();
				if (banner && !strcmp(banner->tag, n->tag)) {
					finish(banner, "gone", NULL);
					banner = NULL;
					banner_close();
				}
				goto sound;
			}
	drop_tag(n->tag);
	if (n->popup) {
		add_popup(n, false);
	} else {
		if (banner)
			finish(banner, "gone", NULL);
		banner = n;
		n->until = n->timeout ? now_ms() + n->timeout * 1000u : 0;
		show_banner();
	}
sound:
	/* an alarm repeats while its popup is open; a banner beeps once */
	if (strcmp(n->sound, "alarm") || !n->popup)
		notify_sound(n->sound, n->volume);
	return true;
}

/* a line from ui.cmd, without its "notify " */
void notify_command(const char *line)
{
	if (!notify_show(line, NULL, NULL))
		fprintf(stderr, "notify: not a JSON object: %.80s\n", line);
}

void notify_poll(void)
{
	uint32_t now = now_ms();

	live_poll();
	if (banner && banner->until && (int32_t)(now - banner->until) >= 0) {
		finish(banner, "gone", NULL);
		banner = NULL;
		banner_close();
	}
	if (npopups) {
		struct note *n = popups[0];

		if (n->until && (int32_t)(now - n->until) >= 0) {
			popup_done("gone", NULL);
		} else if (n->alarm_end && (int32_t)(now - n->alarm_end) < 0 &&
			   (!n->alarm_next || (int32_t)(now - n->alarm_next) >= 0)) {
			n->alarm_next = now + ALARM_EVERY_MS;
			notify_sound("alarm", n->volume);
		}
	}
}
