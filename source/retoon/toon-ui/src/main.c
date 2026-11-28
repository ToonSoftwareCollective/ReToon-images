// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * toon-ui: the Toon 2 screen.
 *
 * Draws dashboards (Home Assistant's card format, see dash.c) with LVGL on
 * the framebuffer, live over the Home Assistant websocket (ha.c), with the
 * unit's own sensors and boiler as toon.* entities (local.c) and the WiFi
 * setup (wifi.c). Everything else is configured from the web interface.
 *
 * Commands come over the Toon's own MQTT broker (libtoonbus), from the web
 * interface, toon-m4d, apps and for tests. retoon/ui/set:
 *   view N | view dim | tap X Y | swipe X1 Y1 X2 Y2 | wifi | info | close | text STRING
 *   wake (un-dim as if touched; sent by toon-m4d on an approach)
 *   calibrate (the touch calibration screen) | calibrate reset
 * retoon/notify/set: a banner or popup, JSON or plain text (see notify.c).
 * What the web interface shows about this program goes to retoon/ui.
 * Apps (JavaScript from p4) are in apps.c.
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "ui.h"


int screen_w = 1024, screen_h = 600;	/* set from the framebuffer in main() */
lv_font_t *f_small, *f_body, *f_head, *f_mid, *f_big;
lv_font_t *i_tiny, *i_small, *i_mid, *i_big;
extern lv_obj_t *test_ta;

static volatile sig_atomic_t stop;
static void on_signal(int sig) { (void)sig; stop = 1; }

/* ---- fonts and icons --------------------------------------------------- */

static void *load_file(const char *name, size_t *len)
{
	char path[256];
	FILE *f;
	void *buf;
	long n;

	snprintf(path, sizeof(path), FONT_DIR "/%s", name);
	f = fopen(path, "rb");
	if (!f) {
		perror(path);
		exit(1);
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	rewind(f);
	buf = malloc(n + 1);
	*len = fread(buf, 1, n, f);
	((char *)buf)[*len] = 0;
	fclose(f);
	return buf;
}

static lv_font_t *font(const char *file, int size, int cache)
{
	size_t len;
	void *data = load_file(file, &len);

	return lv_tiny_ttf_create_data_ex(data, len, size, LV_FONT_KERNING_NONE, cache);
}

/*
 * The clock's digits at a size of their own (a clock card made taller). A
 * few sizes are kept: a page that is built again reuses them. When all
 * places are taken, a size that the pages built since clock_fonts_new()
 * do not use is freed (its clocks are gone). NULL when that does not
 * help either: the caller keeps f_big.
 */
static struct { int size; lv_font_t *f; unsigned int gen; } clocks[4];
static unsigned int clock_gen;

void clock_fonts_new(void)
{
	clock_gen++;
}

lv_font_t *clock_font(int size)
{
	static void *data;
	static size_t len;
	unsigned int i, n = sizeof(clocks) / sizeof(clocks[0]);

	for (i = 0; i < n; i++)
		if (clocks[i].f && clocks[i].size == size) {
			clocks[i].gen = clock_gen;
			return clocks[i].f;
		}
	for (i = 0; i < n && clocks[i].f; i++)
		;
	if (i == n) {
		for (i = 0; i < n && clocks[i].gen == clock_gen; i++)
			;
		if (i == n)
			return NULL;
		lv_tiny_ttf_destroy(clocks[i].f);
		clocks[i].f = NULL;
	}
	if (!data)
		data = load_file("InterDisplay-Light.ttf", &len);
	/* 0-9 and : */
	clocks[i].f = lv_tiny_ttf_create_data_ex(data, len, size, LV_FONT_KERNING_NONE, 12);
	clocks[i].size = clocks[i].f ? size : 0;
	clocks[i].gen = clock_gen;
	return clocks[i].f;
}

struct icon {
	const char *name;
	char utf8[5];
};
static struct icon *icons;
static size_t nicons;

static void load_icons(void)
{
	size_t len, cap = 8000;
	char *map = load_file("mdi.map", &len), *line, *save;

	icons = calloc(cap, sizeof(*icons));
	for (line = strtok_r(map, "\n", &save); line && nicons < cap;
	     line = strtok_r(NULL, "\n", &save)) {
		char *sp = strchr(line, ' ');
		uint32_t cp;
		char *u;

		if (!sp)
			continue;
		*sp = 0;
		cp = strtoul(sp + 1, NULL, 16);
		u = icons[nicons].utf8;
		if (cp < 0x10000) {
			u[0] = 0xe0 | cp >> 12;
			u[1] = 0x80 | (cp >> 6 & 0x3f);
			u[2] = 0x80 | (cp & 0x3f);
		} else {
			u[0] = 0xf0 | cp >> 18;
			u[1] = 0x80 | (cp >> 12 & 0x3f);
			u[2] = 0x80 | (cp >> 6 & 0x3f);
			u[3] = 0x80 | (cp & 0x3f);
		}
		icons[nicons++].name = line;
	}
}

static int icon_cmp(const void *key, const void *elem)
{
	return strcmp(key, ((const struct icon *)elem)->name);
}

const char *mdi(const char *name, const char *fallback)
{
	struct icon *i = NULL;

	if (name && !strncmp(name, "mdi:", 4))
		i = bsearch(name + 4, icons, nicons, sizeof(*icons), icon_cmp);
	if (i)
		return i->utf8;
	if (fallback && !strncmp(fallback, "mdi:", 4))
		return mdi(fallback, "");
	return fallback ? fallback : "";
}

/* ---- info screen ------------------------------------------------------- */

static lv_obj_t *info_win;

static void info_close(lv_event_t *ev)
{
	(void)ev;
	if (info_win)
		lv_obj_delete(info_win);
	info_win = NULL;
}

/* percent-encode what goes in the user:password part of a URL */
static void url_part(char *out, size_t len, const char *s)
{
	size_t n = 0;

	for (; *s && n + 4 < len; s++) {
		unsigned char c = *s;

		if (isalnum(c) || strchr("-._~", c))
			out[n++] = c;
		else
			n += snprintf(out + n, len - n, "%%%02X", c);
	}
	out[n] = 0;
}

/* restart, after asking: where it goes depends on "Always start ReToon" */
static lv_obj_t *restart_box;

static void restart_cancel(lv_event_t *ev)
{
	(void)ev;
	if (restart_box)
		lv_msgbox_close(restart_box);
	restart_box = NULL;
}

static void restart_now(lv_event_t *ev)
{
	(void)ev;
	lv_msgbox_close(restart_box);
	restart_box = lv_msgbox_create(NULL);
	lv_obj_set_style_text_font(lv_msgbox_add_text(restart_box, tr("Restarting...")), f_body, 0);
	lv_refr_now(NULL);
	sync();
	if (fork() == 0) {
		execl("/sbin/reboot", "reboot", (char *)NULL);
		_exit(1);
	}
}

static void restart_ask(lv_event_t *ev)
{
	/* toon-update makes the start after an update come back here once */
	bool back = !access(CONFIG_DIR "/autoboot", F_OK) ||
		    !access("/run/toon/boot-retoon-once", F_OK);
	lv_obj_t *b;

	(void)ev;
	if (restart_box)
		return;
	restart_box = lv_msgbox_create(NULL);
	lv_obj_set_width(restart_box, S(560));
	lv_obj_set_style_text_font(lv_msgbox_add_title(restart_box, tr("Restart")), f_head, 0);
	lv_obj_set_style_text_font(lv_msgbox_add_text(restart_box, tr(back ?
		"Restart the Toon now? ReToon starts again." :
		"Restart the Toon now? It will start the original Toon software, because "
		"\"Always start ReToon\" is off (web interface, System).")), f_body, 0);
	b = lv_msgbox_add_footer_button(restart_box, tr("Cancel"));
	lv_obj_set_size(b, S(180), S(52));
	lv_obj_set_style_text_font(b, f_body, 0);
	lv_obj_add_event_cb(b, restart_cancel, LV_EVENT_CLICKED, NULL);
	b = lv_msgbox_add_footer_button(restart_box, tr("Restart"));
	lv_obj_set_size(b, S(180), S(52));
	lv_obj_set_style_text_font(b, f_body, 0);
	lv_obj_set_style_bg_color(b, lv_color_hex(C_BAD), 0);
	lv_obj_add_event_cb(b, restart_now, LV_EVENT_CLICKED, NULL);
	/* the footer exists once it has a button */
	lv_obj_set_style_min_height(lv_msgbox_get_footer(restart_box), S(76), 0);
}

static void info_line(lv_obj_t *parent, int y, const char *key, const char *value)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_obj_set_style_text_font(l, f_body, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(C_MUTED), 0);
	lv_label_set_text(l, key);
	lv_obj_set_pos(l, S(40), y);
	l = lv_label_create(parent);
	lv_obj_set_style_text_font(l, f_body, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
	lv_label_set_text(l, value);
	lv_obj_set_pos(l, S(300), y);
}

void info_open(void)
{
	char host[64] = "", ip[64] = "-", url[96], pw[64] = "-", fp[128] = "", mode[16] = "";
	char version[32] = "", st1[96], st2[96];
	const char *scheme;
	FILE *f;
	struct ifaddrs *ifa, *i;
	lv_obj_t *l, *b;

	if (info_win)
		return;
	/* written into the image by the release build */
	if ((f = fopen("/etc/retoon-version", "r"))) {
		if (fgets(version, sizeof(version), f))
			version[strcspn(version, "\r\n")] = 0;
		fclose(f);
	}
	gethostname(host, sizeof(host) - 1);
	if (!getifaddrs(&ifa)) {
		for (i = ifa; i; i = i->ifa_next)
			if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET &&
			    strcmp(i->ifa_name, "lo"))
				inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr,
					  ip, sizeof(ip));
		freeifaddrs(ifa);
	}
	file_value(WEB_CONF, "password", pw, sizeof(pw));
	file_value(WEB_CONF, "http", mode, sizeof(mode));
	file_value("/run/toon/web", "fingerprint", fp, sizeof(fp));
	scheme = fp[0] && strcmp(mode, "on") ? "https" : "http";

	info_win = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(info_win);
	lv_obj_set_size(info_win, SCREEN_W, SCREEN_H);
	lv_obj_set_style_bg_color(info_win, lv_color_hex(C_BG), 0);
	lv_obj_set_style_bg_opa(info_win, LV_OPA_COVER, 0);
	lv_obj_add_flag(info_win, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(info_win, LV_OBJ_FLAG_SCROLLABLE);

	l = lv_label_create(info_win);
	lv_obj_set_style_text_font(l, f_head, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
	lv_label_set_text(l, tr("This ReToon"));
	lv_obj_set_pos(l, S(40), S(28));
	b = l;
	l = lv_label_create(info_win);
	lv_obj_set_style_text_font(l, f_body, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(C_MUTED), 0);
	if (version[0])
		lv_label_set_text_fmt(l, tr("version %s"), version);
	else
		lv_label_set_text(l, tr("own build"));
	lv_obj_align_to(l, b, LV_ALIGN_OUT_RIGHT_BOTTOM, S(16), 0);

	l = lv_label_create(info_win);
	lv_obj_set_style_text_font(l, f_body, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(C_MUTED), 0);
	lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(l, SCREEN_W - S(80));
	lv_label_set_text(l, tr("Pages, the Home Assistant connection and all other settings are "
			  "made in a browser on your phone or computer. Open the address below."));
	lv_obj_set_pos(l, S(40), S(76));

	snprintf(url, sizeof(url), "%s://%s/", scheme, ip);
	info_line(info_win, S(160), tr("Web interface"), url);
	snprintf(url, sizeof(url), "%s://%s.local/", scheme, host);
	info_line(info_win, S(200), tr("or"), url);
	info_line(info_win, S(240), tr("User name"), "admin");
	info_line(info_win, S(280), tr("Password"), pw);
	info_line(info_win, S(330), "Home Assistant", status_text(ha_status(), st1, sizeof(st1)));
	info_line(info_win, S(366), "MQTT", status_text(mqtt_status(), st2, sizeof(st2)));
	info_line(info_win, S(402), tr("Name"), host);

	/* scan to open the web interface already logged in: the address, not
	   the .local name, which not every phone can look up */
	if (strcmp(ip, "-") && strcmp(pw, "-")) {
		char user[16], pass[200], login[300];
		lv_obj_t *qr, *frame;

		url_part(user, sizeof(user), "admin");
		url_part(pass, sizeof(pass), pw);
		snprintf(login, sizeof(login), "%s://%s:%s@%s/", scheme, user, pass, ip);
		frame = lv_obj_create(info_win);
		lv_obj_remove_style_all(frame);
		lv_obj_set_size(frame, S(236), S(236));
		lv_obj_set_style_bg_color(frame, lv_color_white(), 0);
		lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
		lv_obj_set_style_radius(frame, S(10), 0);
		lv_obj_set_pos(frame, SCREEN_W - S(236) - S(40), S(150));
		qr = lv_qrcode_create(frame);
		lv_qrcode_set_size(qr, S(212));
		lv_qrcode_set_dark_color(qr, lv_color_black());
		lv_qrcode_set_light_color(qr, lv_color_white());
		lv_qrcode_update(qr, login, strlen(login));
		lv_obj_center(qr);
		l = lv_label_create(info_win);
		lv_obj_set_style_text_font(l, f_small, 0);
		lv_obj_set_style_text_color(l, lv_color_hex(C_MUTED), 0);
		lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_width(l, S(236));
		lv_label_set_text(l, tr("Scan to open it, logged in"));
		lv_obj_set_pos(l, SCREEN_W - S(236) - S(40), S(394));
	}
	if (fp[0]) {
		/* to compare with what the browser shows in its certificate warning */
		l = lv_label_create(info_win);
		lv_obj_set_style_text_font(l, f_small, 0);
		lv_obj_set_style_text_color(l, lv_color_hex(C_MUTED), 0);
		lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(l, SCREEN_W - S(260));
		lv_label_set_text_fmt(l, tr("The browser warns once that it does not know this ReToon's "
				      "certificate. Its SHA-256 fingerprint: %s"), fp);
		lv_obj_set_pos(l, S(40), S(440));
	}

	b = lv_button_create(info_win);
	lv_obj_set_size(b, S(160), S(52));
	lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, -S(24), -S(20));
	lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD_HI), 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_add_event_cb(b, info_close, LV_EVENT_CLICKED, NULL);
	l = lv_label_create(b);
	lv_obj_set_style_text_font(l, f_body, 0);
	lv_label_set_text(l, tr("Close"));
	lv_obj_center(l);

	b = lv_button_create(info_win);
	lv_obj_set_size(b, S(160), S(52));
	lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, -S(24) - S(160) - S(16), -S(20));
	lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD_HI), 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_add_event_cb(b, restart_ask, LV_EVENT_CLICKED, NULL);
	l = lv_label_create(b);
	lv_obj_set_style_text_font(l, f_body, 0);
	lv_label_set_text(l, tr("Restart"));
	lv_obj_center(l);
}

/* ---- injected touches and the command pipe ------------------------------ */

static struct {
	int x1, y1, x2, y2;
	uint32_t t0, dur;
	bool active;
} inj;

static void inject_read(lv_indev_t *indev, lv_indev_data_t *data)
{
	uint32_t el = now_ms() - inj.t0;

	(void)indev;
	if (!inj.active) {
		data->state = LV_INDEV_STATE_RELEASED;
		data->point.x = inj.x2;
		data->point.y = inj.y2;
		return;
	}
	if (el >= inj.dur) {
		inj.active = false;
		el = inj.dur;
		data->state = LV_INDEV_STATE_RELEASED;
	} else {
		data->state = LV_INDEV_STATE_PRESSED;
	}
	data->point.x = inj.x1 + (inj.x2 - inj.x1) * (int)el / (int)inj.dur;
	data->point.y = inj.y1 + (inj.y2 - inj.y1) * (int)el / (int)inj.dur;
}

static void command(char *line)
{
	int a, b, c, d;

	/* a notification wakes the screen only when it asks to */
	if (!strncmp(line, "notify ", 7)) {
		notify_command(line + 7);
		return;
	}
	lv_display_trigger_activity(NULL);
	if (sscanf(line, "tap %d %d", &a, &b) == 2) {
		inj = (typeof(inj)){ a, b, a, b, now_ms(), 120, true };
	} else if (sscanf(line, "swipe %d %d %d %d", &a, &b, &c, &d) == 4) {
		inj = (typeof(inj)){ a, b, c, d, now_ms(), 250, true };
	} else if (!strcmp(line, "view dim")) {
		dash_dim(true);	/* a preview, until the next touch */
	} else if (sscanf(line, "view %d", &a) == 1) {
		dash_dim(false);
		dash_show(a);
	} else if (!strcmp(line, "wifi")) {
		wifi_open(false);
	} else if (!strcmp(line, "info")) {
		info_open();
	} else if (!strcmp(line, "close")) {
		info_close(NULL);
		apps_close();
	} else if (!strncmp(line, "text ", 5) && test_ta) {
		lv_textarea_add_text(test_ta, line + 5);
	} else if (!strcmp(line, "calibrate")) {
		touchcal_open(false);
	} else if (!strcmp(line, "calibrate reset")) {
		touchcal_reset();
	} else if (!strcmp(line, "wake")) {
		/* the activity trigger above is all a wake needs: dim_poll()
		   restores the brightness and the normal timer dims again */
	}
}

/* retoon/ui/set: one command a line */
static void on_ui(const char *topic, const char *payload)
{
	char buf[512], *line, *save;

	(void)topic;
	snprintf(buf, sizeof(buf), "%s", payload);
	for (line = strtok_r(buf, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save))
		command(line);
}

/* retoon/notify/set: JSON as it is, plain text as the message */
static void on_notify(const char *topic, const char *payload)
{
	const char *p = payload;

	(void)topic;
	while (isspace((unsigned char)*p))
		p++;
	if (*p == '{') {
		notify_command(p);
	} else if (*p) {
		cJSON *o = cJSON_CreateObject();
		char *s;

		cJSON_AddStringToObject(o, "message", p);
		s = cJSON_PrintUnformatted(o);
		cJSON_Delete(o);
		if (s)
			notify_command(s);
		free(s);
	}
}

static void cmd_poll(void)
{
	bus_poll();
}

/* ---- display dimming ---------------------------------------------------- */

/* Grey instead of colour (while dimmed, when asked): every area is made
   grey just before it goes to the screen. */
static bool grey;

static void grey_flush(lv_event_t *ev)
{
	lv_draw_buf_t *b = lv_display_get_buf_active(lv_event_get_target(ev));
	const lv_area_t *a = lv_event_get_param(ev);
	int w = lv_area_get_width(a), h = lv_area_get_height(a);

	if (!grey || !b)
		return;
	for (int y = 0; y < h; y++) {
		uint8_t *p = b->data + y * b->header.stride;

		if (b->header.cf == LV_COLOR_FORMAT_RGB565) {
			uint16_t *q = (uint16_t *)p;

			for (int x = 0; x < w; x++) {
				int r = (q[x] >> 11) << 3, g = ((q[x] >> 5) & 0x3f) << 2, bl = (q[x] & 0x1f) << 3;
				int v = (r * 77 + g * 150 + bl * 29) >> 8;

				q[x] = (v >> 3) << 11 | (v >> 2) << 5 | v >> 3;
			}
		} else if (b->header.cf == LV_COLOR_FORMAT_XRGB8888 || b->header.cf == LV_COLOR_FORMAT_ARGB8888) {
			for (int x = 0; x < w; x++, p += 4)
				p[0] = p[1] = p[2] = (p[2] * 77 + p[1] * 150 + p[0] * 29) >> 8;
		}
	}
}

static void set_grey(bool on)
{
	if (on == grey)
		return;
	grey = on;
	lv_obj_invalidate(lv_screen_active());	/* everything again, in the new colours */
}

static void dim_poll(void)
{
	static uint32_t t_conf;
	static struct timespec mt_conf = { -1, 0 };
	static int dim_after, dim_level = 10, saved = -1, normal = -1;
	static int home_after = 10;	/* back to the home page after; 0 = stay */
	static bool dim_grey;
	static int seen = -1;		/* the backlight at the last look */
	static uint32_t t_bl, raised_at;
	/* the screen off at night: while the thermostat is on Sleep, and/or
	   between two times (minutes of the day, -1 not set) */
	static bool sleep_off, off_mode;
	static int off_from = -1, off_to = -1;
	static uint32_t t_mode;
	uint32_t idle = lv_display_get_inactive_time(NULL), now = now_ms();
	struct stat sb;
	char buf[32];

	/* re-read the settings as soon as the web page has saved them */
	if (now - t_conf > 250 || !t_conf) {
		t_conf = now;
		if (stat(UI_CONF, &sb))
			memset(&sb, 0, sizeof(sb));
		if (sb.st_mtim.tv_sec == mt_conf.tv_sec && sb.st_mtim.tv_nsec == mt_conf.tv_nsec)
			goto apply;
		mt_conf = sb.st_mtim;
		dim_after = file_value(UI_CONF, "dim_after", buf, sizeof(buf)) ? 0 : atoi(buf);
		dim_grey = !file_value(UI_CONF, "dim_grey", buf, sizeof(buf)) && atoi(buf) == 1;
		/* standard on, after 10 seconds */
		home_after = !file_value(UI_CONF, "home_auto", buf, sizeof(buf)) && atoi(buf) == 0 ? 0 :
			     file_value(UI_CONF, "home_after", buf, sizeof(buf)) ? 10 :
			     LV_CLAMP(3, atoi(buf), 3600);
		if (saved >= 0)
			set_grey(dim_grey);
		int level = file_value(UI_CONF, "dim_level", buf, sizeof(buf)) ? 10 :
			    LV_CLAMP(0, atoi(buf), 100);

		sleep_off = !file_value(UI_CONF, "sleep_off", buf, sizeof(buf)) && atoi(buf) == 1;
		off_from = off_to = -1;
		if (!file_value(UI_CONF, "off_from", buf, sizeof(buf)) && strchr(buf, ':'))
			off_from = (atoi(buf) * 60 + atoi(strchr(buf, ':') + 1)) % 1440;
		if (!file_value(UI_CONF, "off_to", buf, sizeof(buf)) && strchr(buf, ':'))
			off_to = (atoi(buf) * 60 + atoi(strchr(buf, ':') + 1)) % 1440;
		if (off_from == off_to)
			off_from = off_to = -1;
		t_mode = 0;	/* look again at once */

		/* a new dimmed level shows at once when the screen is dimmed */
		if (level != dim_level && saved >= 0 && !off_mode) {
			backlight_set(level);
			seen = -1;	/* our own change, no wake */
		}
		dim_level = level;
		/* the normal brightness: set at the start and when it is changed;
		 * a change (from the web page) wakes a dimmed screen to show it */
		if (!file_value(UI_CONF, "brightness", buf, sizeof(buf)) && atoi(buf) > 0 &&
		    atoi(buf) != normal) {
			int first = normal < 0;

			normal = LV_CLAMP(5, atoi(buf), 100);
			backlight_set(normal);
			if (saved >= 0)
				saved = normal;
			if (!first)
				lv_display_trigger_activity(NULL);
		}
	}
apply:
	/* the screen off at night: dimmed means off then. Once a second: is
	   the thermostat on Sleep, is it within the times? */
	if (now - t_mode >= 1000 || !t_mode) {
		bool was = off_mode;
		time_t t = time(NULL);
		struct tm tm;
		int m;

		t_mode = now ? now : 1;
		off_mode = false;
		if (sleep_off) {
			const char *p = ent_str(ent_get("toon.thermostat"), "preset_mode");

			off_mode = p && !strcmp(p, "sleep");
		}
		if (off_from >= 0 && off_to >= 0 && localtime_r(&t, &tm) && tm.tm_year > 120) {
			m = tm.tm_hour * 60 + tm.tm_min;
			/* 23:00 - 07:00 goes over midnight */
			if (off_from < off_to ? m >= off_from && m < off_to : m >= off_from || m < off_to)
				off_mode = true;
		}
		/* already dimmed: off now, or back to the dimmed level */
		if (off_mode != was && saved >= 0) {
			backlight_set(off_mode ? 0 : dim_level);
			seen = -1;	/* our own change, no wake */
		}
	}
	int level = off_mode ? 0 : dim_level;
	/* off at night also without dimming: after half a minute */
	int after = off_mode && dim_after <= 0 ? 30 : dim_after;

	/* the backlight raised by someone else (Home Assistant, a dashboard
	   action): a wake as by a touch, and the dim timer counts from then.
	   A dimmed screen was otherwise left bright until the next touch. */
	if (now - t_bl >= 250 || !t_bl) {
		int cur = backlight_get();

		t_bl = now;
		if (seen >= 0 && cur > seen) {
			raised_at = now;
			if (saved >= 0) {
				set_grey(false);
				dash_dim(false);
				saved = -1;
			}
		}
		seen = cur;
	}
	/* untouched for a while: back to the home page (once, it then is) */
	if (home_after > 0 && idle > (uint32_t)home_after * 1000 && dash_current() != 0)
		dash_show(0);
	if (saved < 0 && after > 0 && idle > (uint32_t)after * 1000 &&
	    now - raised_at > (uint32_t)after * 1000) {
		int cur = backlight_get();

		if (cur > level) {
			saved = cur;
			backlight_set(level);
			set_grey(dim_grey);
			dash_dim(true);
		}
	} else if (saved >= 0 && (idle < 1500 || after <= 0)) {
		/* the touch that wakes the screen also presses what is under it:
		   the dimmed screen still shows the page */
		backlight_set(saved);
		set_grey(false);
		dash_dim(false);
		saved = -1;
	}
}

/* ---- main --------------------------------------------------------------- */

static void find_touch(char *out, size_t len)
{
	char path[300], name[128];
	FILE *f;

	snprintf(out, len, "/dev/input/event0");
	for (int i = 0; i < 8; i++) {
		snprintf(path, sizeof(path), "/sys/class/input/event%d/device/name", i);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (fgets(name, sizeof(name), f) && strstr(name, "ouchscreen"))
			snprintf(out, len, "/dev/input/event%d", i);
		fclose(f);
	}
}

int main(int argc, char **argv)
{
	/* --rescue <reason>: the initrd's screen, see rescue.c */
	bool rescue = argc > 1 && !strcmp(argv[1], "--rescue");
	char touch[64];
	lv_display_t *disp;
	lv_indev_t *indev;
	lv_obj_t *scr;
	bool calibrate;

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);
	mkdir("/run/toon", 0755);
	lang_load();

	lv_init();
	lv_tick_set_cb(now_ms);
	disp = lv_linux_fbdev_create();
	lv_linux_fbdev_set_file(disp, "/dev/fb0");
	lv_display_add_event_cb(disp, grey_flush, LV_EVENT_FLUSH_START, NULL);
	screen_w = lv_display_get_horizontal_resolution(disp);
	screen_h = lv_display_get_vertical_resolution(disp);
	find_touch(touch, sizeof(touch));
	calibrate = touchcal_init(lv_evdev_create(LV_INDEV_TYPE_POINTER, touch), touch);
	indev = lv_indev_create();
	lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(indev, inject_read);

	f_small = font("Inter-Regular.ttf", S(16), 256);
	f_body = font("Inter-Regular.ttf", S(19), 256);
	f_head = font("Inter-SemiBold.ttf", S(22), 160);
	/* the initrd carries only the two fonts its screens need */
	f_mid = font(rescue ? "Inter-SemiBold.ttf" : "Inter-Medium.ttf", S(34), 48);
	f_big = rescue ? f_mid : font("InterDisplay-Light.ttf", S(78), 24);
	i_tiny = font("materialdesignicons-webfont.ttf", S(18), 8);	/* badges */
	i_small = font("materialdesignicons-webfont.ttf", S(26), 128);
	i_mid = font("materialdesignicons-webfont.ttf", S(32), 48);
	i_big = font("materialdesignicons-webfont.ttf", S(48), 16);
	load_icons();
	/* LVGL's own symbols (keyboard keys, check marks) come from its built-in font */
	f_small->fallback = &lv_font_montserrat_14;
	f_body->fallback = f_head->fallback = f_mid->fallback = &lv_font_montserrat_20;

	lv_display_set_theme(disp, lv_theme_default_init(disp, lv_color_hex(C_ACCENT),
			     lv_color_hex(0xff9800), true, f_body));
	scr = lv_screen_active();
	lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

	bus_open("ui");
	bus_on("retoon/ui/set", on_ui);
	bus_on("retoon/notify/set", on_notify);

	if (rescue) {
		rescue_init(scr, argc > 2 ? argv[2] : "");
		wifi_init();
		while (!stop) {
			uint32_t wait = lv_timer_handler();

			jobs_poll();
			wifi_poll();
			rescue_poll();
			cmd_poll();
			notify_poll();
			usleep(LV_MIN(wait, 8) * 1000);
		}
		return 0;
	}

	local_init();
	wifi_init();
	ha_init();
	apps_init();
	dash_init(scr);
	dash_load();
	mqtt_init();
	/* a resistive panel without any calibration: ask for one at once */
	if (calibrate)
		touchcal_open(true);

	while (!stop) {
		uint32_t wait = lv_timer_handler();

		jobs_poll();
		ha_poll();
		mqtt_poll();
		local_poll();
		wifi_poll();
		dash_poll();
		graph_poll();
		energy_poll();
		apps_poll();
		cmd_poll();
		notify_poll();
		dim_poll();
		usleep(LV_MIN(wait, 8) * 1000);
	}
	graph_save();
	bus_put("ui", NULL);	/* the screen is gone */
	bus_close();
	return 0;
}
