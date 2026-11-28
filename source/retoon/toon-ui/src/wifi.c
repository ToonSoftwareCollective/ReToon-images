// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * toon-ui: WiFi setup on the screen itself - the one setting that cannot be
 * made from the web interface. It opens by itself when there is no WiFi
 * configuration or the connection stays down, and from the WiFi icon.
 * The work is done by /usr/bin/toon-wifi (scan, connect with fall-back to
 * the previous settings, status).
 */
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <unistd.h>
#include "ui.h"

#define TOOL "/usr/bin/toon-wifi"

struct net {
	char ssid[72];
	int signal;
	bool secure;
};

static lv_obj_t *win, *body, *status_lbl, *ta;
static struct net nets[24];
static int nnets, sig;
static bool connected, automatic, scanning, busy, dismissed;
static char cur_ssid[72], pick_ssid[72];
static uint32_t t_boot, t_down, t_state, t_scan, t_signal;

lv_obj_t *test_ta;		/* where injected text goes (main.c) */

bool wifi_connected(void) { return connected; }
int wifi_signal(void) { return connected ? sig : 0; }

static void show_list(void);
static void show_password(void);

static void local_ip(char *out, size_t len)
{
	struct ifaddrs *ifa, *i;

	snprintf(out, len, "-");
	if (getifaddrs(&ifa))
		return;
	for (i = ifa; i; i = i->ifa_next)
		if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET &&
		    !strcmp(i->ifa_name, "wlan0"))
			inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, out, len);
	freeifaddrs(ifa);
}

void wifi_ip(char *out, size_t len) { local_ip(out, len); }

static void read_state(void)
{
	char buf[256];
	FILE *f = fopen("/sys/class/net/wlan0/operstate", "r");
	bool up = false;

	if (f) {
		up = fgets(buf, sizeof(buf), f) && !strncmp(buf, "up", 2);
		fclose(f);
	}
	if (up) {
		local_ip(buf, sizeof(buf));
		up = strcmp(buf, "-");
	}
	connected = up;
	/* the signal comes from "iw dev wlan0 link" (ask_link), over nl80211:
	   /proc/net/wireless is the old wireless extensions the kernel warns about */
}

static void set_status_text(void)
{
	char ip[64], buf[200];

	if (!status_lbl)
		return;
	local_ip(ip, sizeof(ip));
	if (connected)
		snprintf(buf, sizeof(buf), tr("Connected to %s  ·  %d dBm  ·  %s"),
			 cur_ssid[0] ? cur_ssid : tr("the network"), sig, ip);
	else if (access(WIFI_CONF, F_OK))
		snprintf(buf, sizeof(buf), "%s", tr("WiFi is not set up yet. Choose your network."));
	else
		snprintf(buf, sizeof(buf), "%s", tr("Not connected"));
	if (strcmp(lv_label_get_text(status_lbl), buf))
		lv_label_set_text(status_lbl, buf);
}

static void on_link(int status, char *out, void *user)
{
	char *p = strstr(out, "ssid="), *s = strstr(out, "signal=");

	(void)status;
	(void)user;
	cur_ssid[0] = 0;
	if (p) {
		snprintf(cur_ssid, sizeof(cur_ssid), "%s", p + 5);
		cur_ssid[strcspn(cur_ssid, "\n")] = 0;
	}
	sig = s ? atoi(s + 7) : 0;
	set_status_text();
}

static void ask_link(void)
{
	static char *cmd[] = { TOOL, "link", NULL };

	job_run(cmd, on_link, NULL);
}

static void on_scan(int status, char *out, void *user)
{
	char *line, *save;

	(void)user;
	scanning = false;
	if (!status) {
		nnets = 0;
		for (line = strtok_r(out, "\n", &save); line && nnets < 24;
		     line = strtok_r(NULL, "\n", &save)) {
			int s, sec, off = 0;

			if (sscanf(line, "%d %d %n", &s, &sec, &off) < 2 || !line[off])
				continue;
			nets[nnets].signal = s;
			nets[nnets].secure = sec;
			snprintf(nets[nnets].ssid, sizeof(nets[0].ssid), "%s", line + off);
			nnets++;
		}
	}
	if (win && !ta && !busy)
		show_list();
}

static void scan(void)
{
	static char *cmd[] = { TOOL, "scan", NULL };

	if (scanning || busy)
		return;
	if (!job_run(cmd, on_scan, NULL))
		scanning = true;
	t_scan = now_ms();
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *s)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_label_set_text(l, s);
	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
	return l;
}

static lv_obj_t *button(lv_obj_t *parent, const char *s, lv_event_cb_t cb, void *user)
{
	lv_obj_t *b = lv_button_create(parent);

	lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD_HI), 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_set_style_radius(b, S(10), 0);
	lv_obj_center(label(b, f_body, C_TEXT, s));
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
	return b;
}

static void close_win(void)
{
	if (win)
		lv_obj_delete(win);
	win = body = status_lbl = ta = test_ta = NULL;
	automatic = false;
}

/* Closed while there is no connection: do not come back until it was up again. */
static void on_close(lv_event_t *ev) { (void)ev; dismissed = !connected; close_win(); }
static void on_rescan(lv_event_t *ev) { (void)ev; scan(); show_list(); }
static void on_back(lv_event_t *ev) { (void)ev; show_list(); }

static void on_done_timer(lv_timer_t *t)
{
	(void)t;
	if (win && busy == false && connected && !ta)
		close_win();
}

static void on_connect_done(int status, char *out, void *user)
{
	lv_obj_t *l;

	(void)out;
	(void)user;
	busy = false;
	read_state();
	ask_link();
	if (!win)
		return;
	lv_obj_clean(body);
	if (!status) {
		l = label(body, f_head, C_OK, tr("Connected"));
		lv_obj_align(l, LV_ALIGN_CENTER, 0, -S(20));
		lv_timer_set_repeat_count(lv_timer_create(on_done_timer, 2500, NULL), 1);
	} else {
		l = label(body, f_head, C_BAD, tr("Could not connect"));
		lv_obj_align(l, LV_ALIGN_CENTER, 0, -S(60));
		l = label(body, f_body, C_MUTED, tr(status == 2 ?
			  "The password must be 8 to 63 characters." :
			  "Check the password. The previous settings are in use again."));
		lv_obj_align(l, LV_ALIGN_CENTER, 0, -S(20));
		l = button(body, tr("Back"), on_back, NULL);
		lv_obj_set_size(l, S(160), S(52));
		lv_obj_align(l, LV_ALIGN_CENTER, 0, S(50));
	}
}

static void connect_to(const char *ssid, const char *psk)
{
	static char a_ssid[72], a_psk[72];
	static char *cmd[] = { TOOL, "connect", a_ssid, a_psk, NULL };
	lv_obj_t *l;
	char buf[128];

	snprintf(a_ssid, sizeof(a_ssid), "%s", ssid);
	snprintf(a_psk, sizeof(a_psk), "%s", psk);
	ta = test_ta = NULL;
	lv_obj_clean(body);
	if (job_run(cmd, on_connect_done, NULL))
		return;
	busy = true;
	l = lv_spinner_create(body);
	lv_obj_set_size(l, S(64), S(64));
	lv_obj_align(l, LV_ALIGN_CENTER, 0, -S(50));
	snprintf(buf, sizeof(buf), tr("Connecting to %s…"), ssid);
	l = label(body, f_body, C_TEXT, buf);
	lv_obj_align(l, LV_ALIGN_CENTER, 0, S(20));
}

static void on_kb(lv_event_t *ev)
{
	lv_event_code_t code = lv_event_get_code(ev);

	if (code == LV_EVENT_CANCEL) {
		show_list();
	} else if (code == LV_EVENT_READY) {
		const char *t = lv_textarea_get_text(ta);

		if (!pick_ssid[0]) {	/* the name of a hidden network comes first */
			if (!t[0])
				return;
			snprintf(pick_ssid, sizeof(pick_ssid), "%s", t);
			show_password();
			return;
		}
		char psk[72];

		snprintf(psk, sizeof(psk), "%s", t);
		connect_to(pick_ssid, psk);
	}
}

static void show_password(void)
{
	lv_obj_t *l, *kb;
	char buf[128];

	lv_obj_clean(body);
	if (pick_ssid[0])
		snprintf(buf, sizeof(buf), tr("Password for %s"), pick_ssid);
	else
		snprintf(buf, sizeof(buf), "%s", tr("Name of the network"));
	l = label(body, f_body, C_TEXT, buf);
	lv_obj_set_pos(l, S(24), S(4));
	ta = test_ta = lv_textarea_create(body);
	lv_textarea_set_one_line(ta, true);
	lv_textarea_set_max_length(ta, 64);
	lv_obj_set_style_text_font(ta, f_head, 0);
	lv_obj_set_size(ta, SCREEN_W - S(48) - S(150), S(52));
	lv_obj_set_pos(ta, S(24), S(36));
	lv_obj_add_state(ta, LV_STATE_FOCUSED);
	l = button(body, tr("Back"), on_back, NULL);
	lv_obj_set_size(l, S(130), S(52));
	lv_obj_set_pos(l, SCREEN_W - S(24) - S(130), S(36));
	kb = lv_keyboard_create(body);
	lv_obj_set_size(kb, SCREEN_W, S(350));
	lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_style_text_font(kb, f_head, 0);
	lv_keyboard_set_textarea(kb, ta);
	lv_obj_add_event_cb(kb, on_kb, LV_EVENT_READY, NULL);
	lv_obj_add_event_cb(kb, on_kb, LV_EVENT_CANCEL, NULL);
}

static void on_pick(lv_event_t *ev)
{
	intptr_t i = (intptr_t)lv_event_get_user_data(ev);

	if (i < 0) {
		pick_ssid[0] = 0;
		show_password();
		return;
	}
	snprintf(pick_ssid, sizeof(pick_ssid), "%s", nets[i].ssid);
	if (nets[i].secure)
		show_password();
	else
		connect_to(pick_ssid, "");
}

static void show_list(void)
{
	static const char *bars[] = { "mdi:wifi-strength-1", "mdi:wifi-strength-2",
				      "mdi:wifi-strength-3", "mdi:wifi-strength-4" };
	lv_obj_t *list, *b, *l;

	ta = test_ta = NULL;
	lv_obj_clean(body);
	list = lv_obj_create(body);
	lv_obj_remove_style_all(list);
	lv_obj_set_size(list, SCREEN_W - S(48), lv_obj_get_height(body) - S(72));
	lv_obj_set_pos(list, S(24), 0);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(list, S(6), 0);
	lv_obj_set_scroll_dir(list, LV_DIR_VER);

	if (!nnets) {
		l = label(list, f_body, C_MUTED, tr(scanning ? "Looking for networks…" :
			  "No networks found."));
		lv_obj_set_style_pad_top(l, S(20), 0);
	}
	for (intptr_t i = 0; i <= nnets; i++) {
		intptr_t k = i == nnets ? -1 : i;	/* "other network" goes last */

		b = lv_obj_create(list);
		lv_obj_remove_style_all(b);
		lv_obj_set_size(b, SCREEN_W - S(48), S(54));
		lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD), 0);
		lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD_HI), LV_STATE_PRESSED);
		lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
		lv_obj_set_style_radius(b, S(12), 0);
		lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_add_event_cb(b, on_pick, LV_EVENT_CLICKED, (void *)k);
		if (k < 0) {
			l = label(b, f_body, C_MUTED, tr("Other network…"));
			lv_obj_align(l, LV_ALIGN_LEFT_MID, S(62), 0);
			continue;
		}
		l = label(b, i_small, C_TEXT, mdi(bars[nets[k].signal > -55 ? 3 : nets[k].signal > -67 ? 2 :
						       nets[k].signal > -78 ? 1 : 0], "w"));
		lv_obj_align(l, LV_ALIGN_LEFT_MID, S(18), 0);
		l = label(b, f_body, connected && !strcmp(nets[k].ssid, cur_ssid) ? C_OK : C_TEXT,
			  nets[k].ssid);
		lv_obj_align(l, LV_ALIGN_LEFT_MID, S(62), 0);
		if (nets[k].secure) {
			l = label(b, i_small, C_MUTED, mdi("mdi:lock", ""));
			lv_obj_align(l, LV_ALIGN_RIGHT_MID, -S(18), 0);
		}
	}

	b = button(body, tr(scanning ? "Searching…" : "Search again"), on_rescan, NULL);
	lv_obj_set_size(b, S(200), S(52));
	lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, S(24), -S(10));
	b = button(body, tr("Close"), on_close, NULL);
	lv_obj_set_size(b, S(160), S(52));
	lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, -S(24), -S(10));
}

void wifi_open(bool is_automatic)
{
	lv_obj_t *l;

	if (win)
		return;
	automatic = is_automatic;
	win = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(win);
	lv_obj_set_size(win, SCREEN_W, SCREEN_H);
	lv_obj_set_style_bg_color(win, lv_color_hex(C_BG), 0);
	lv_obj_set_style_bg_opa(win, LV_OPA_COVER, 0);
	lv_obj_add_flag(win, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(win, LV_OBJ_FLAG_SCROLLABLE);

	l = label(win, i_mid, C_ACCENT, mdi("mdi:wifi", "w"));
	lv_obj_set_pos(l, S(24), S(18));
	l = label(win, f_head, C_TEXT, "WiFi");
	lv_obj_set_pos(l, S(66), S(20));
	status_lbl = label(win, f_small, C_MUTED, "");
	lv_obj_set_pos(status_lbl, S(150), S(26));

	body = lv_obj_create(win);
	lv_obj_remove_style_all(body);
	lv_obj_set_size(body, SCREEN_W, SCREEN_H - S(68));
	lv_obj_set_pos(body, 0, S(68));
	lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

	read_state();
	set_status_text();
	ask_link();
	scan();
	show_list();
}

void wifi_init(void)
{
	t_boot = now_ms();
	read_state();
	ask_link();
}

void wifi_poll(void)
{
	uint32_t now = now_ms();
	bool was = connected;

	if (now - t_state < 3000)
		return;
	t_state = now;
	if (busy)
		return;
	read_state();
	/* the network name when the connection changes, the signal every 15 s */
	if (connected != was || (connected && now - t_signal >= 15000)) {
		t_signal = now;
		ask_link();
	}
	if (connected) {
		t_down = 0;
		dismissed = false;
	} else if (!t_down) {
		t_down = now;
	}
	set_status_text();

	if (!win) {
		/* no configuration: at once; connection down: after a minute */
		if (!connected && !dismissed && ((access(WIFI_CONF, F_OK) && now - t_boot > 8000) ||
				   (now - t_boot > 60000 && now - t_down > 60000)))
			wifi_open(true);
		return;
	}
	if (automatic && connected && !ta) {
		close_win();
		return;
	}
	/* nothing found yet (the stick was just starting): try again soon */
	if (!ta && !connected && now - t_scan > (nnets ? 20000u : 4000u))
		scan();
}
