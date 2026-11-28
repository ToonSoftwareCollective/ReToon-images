// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * toon-ui --rescue <reason>: the one screen of rescue mode. The initrd
 * starts it when the root image cannot be used. It says what is wrong and
 * how to reach the Toon, and lets WiFi be set up again on the screen itself
 * (wifi.c), which is what makes a lost network something the owner can fix.
 */
#include <unistd.h>
#include "ui.h"

static lv_obj_t *net_lbl, *ssh_lbl;

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *s)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_label_set_text(l, s);
	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
	return l;
}

static lv_obj_t *button(lv_obj_t *parent, const char *s, lv_event_cb_t cb)
{
	lv_obj_t *b = lv_button_create(parent);

	lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD_HI), 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_set_style_radius(b, S(10), 0);
	lv_obj_set_height(b, S(56));
	lv_obj_center(label(b, f_body, C_TEXT, s));
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
	return b;
}

static void on_wifi(lv_event_t *ev) { (void)ev; wifi_open(false); }

/* The initrd took boot.scr away at this start; with it back the next start
 * comes to ReToon again, without it the original Toon software starts. */
static void on_retry(lv_event_t *ev)
{
	(void)ev;
	if (system("mkdir -p /mnt/boot && mount -t vfat /dev/mmcblk3p1 /mnt/boot && "
		   "cp /mnt/data/toon-os/boot.scr /mnt/boot/boot.new && sync && "
		   "mv /mnt/boot/boot.new /mnt/boot/boot.scr; sync; umount /mnt/boot; sync; reboot"))
		return;
}

static void on_vendor(lv_event_t *ev)
{
	(void)ev;
	if (system("sync; reboot"))
		return;
}

void rescue_init(lv_obj_t *scr, const char *reason)
{
	lv_obj_t *l, *row, *b;
	char buf[300];

	l = label(scr, i_mid, C_WARN, mdi("mdi:lifebuoy", "!"));
	lv_obj_set_pos(l, S(40), S(34));
	l = label(scr, f_mid, C_TEXT, tr("ReToon rescue mode"));
	lv_obj_set_pos(l, S(88), S(28));

	snprintf(buf, sizeof(buf), tr("ReToon could not start: %s."), reason[0] ? reason : tr("unknown reason"));
	l = label(scr, f_head, C_WARN, buf);
	lv_obj_set_width(l, SCREEN_W - S(80));
	lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
	lv_obj_set_pos(l, S(40), S(100));

	l = label(scr, f_body, C_MUTED,
		  tr("This is a small rescue system. Nothing is lost: your settings and the "
		  "original Toon software are still there. From a computer on the same "
		  "network the system can be repaired or put back."));
	lv_obj_set_width(l, SCREEN_W - S(80));
	lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
	lv_obj_set_pos(l, S(40), S(150));

	net_lbl = label(scr, f_head, C_TEXT, "");
	lv_obj_set_pos(net_lbl, S(40), S(268));
	ssh_lbl = label(scr, f_body, C_TEXT, "");
	lv_obj_set_pos(ssh_lbl, S(40), S(308));

	row = lv_obj_create(scr);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, SCREEN_W - S(80), S(56));
	lv_obj_set_pos(row, S(40), SCREEN_H - S(150));
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(row, S(16), 0);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	b = button(row, "WiFi", on_wifi);
	lv_obj_set_width(b, S(180));
	b = button(row, tr("Try ReToon again"), on_retry);
	lv_obj_set_width(b, S(300));
	b = button(row, tr("Start the original Toon software"), on_vendor);
	lv_obj_set_flex_grow(b, 1);

	l = label(scr, f_small, C_MUTED,
		  tr("Both buttons on the right restart the Toon. If the cause is not solved, "
		  "\"Try ReToon again\" comes back to this screen."));
	lv_obj_set_pos(l, S(40), SCREEN_H - S(76));
	backlight_set(80);
	rescue_poll();
}

void rescue_poll(void)
{
	static uint32_t t;
	uint32_t now = now_ms();
	char ip[64], buf[200];

	if (t && now - t < 2000)
		return;
	t = now ? now : 1;
	wifi_ip(ip, sizeof(ip));
	if (wifi_connected() && strcmp(ip, "-")) {
		snprintf(buf, sizeof(buf), tr("On the network as %s"), ip);
		lv_obj_set_style_text_color(net_lbl, lv_color_hex(C_OK), 0);
	} else {
		snprintf(buf, sizeof(buf), "%s", tr(access(WIFI_CONF, F_OK) ?
			 "No WiFi settings: press WiFi and choose your network" :
			 "Not on the network (yet). Press WiFi to choose a network"));
		lv_obj_set_style_text_color(net_lbl, lv_color_hex(C_WARN), 0);
	}
	if (strcmp(lv_label_get_text(net_lbl), buf))
		lv_label_set_text(net_lbl, buf);
	if (wifi_connected() && strcmp(ip, "-"))
		snprintf(buf, sizeof(buf), tr("Log in with:  ssh root@%s   (password: toon)"), ip);
	else
		buf[0] = 0;
	if (strcmp(lv_label_get_text(ssh_lbl), buf))
		lv_label_set_text(ssh_lbl, buf);
}
