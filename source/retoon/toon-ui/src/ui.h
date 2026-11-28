/* SPDX-License-Identifier: GPL-2.0-or-later */
/* toon-ui: shared declarations */
#ifndef TOON_UI_H
#define TOON_UI_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>
#include "lvgl.h"
#include <toonbus.h>

/* a path "retoon:<name>" reads the newest retoon/<name> from the bus */
#define STATE	"retoon:"
FILE *state_open(const char *path);

#define CONFIG_DIR	"/mnt/data/toon-os/config"
#define DASHBOARD_FILE	CONFIG_DIR "/dashboard.json"
#define DEFAULT_DASHBOARD "/usr/share/toon/default-dashboard.json"
#define HW_FLAG		CONFIG_DIR "/boiler-hot-water"	/* an OpenTherm boiler with hot water */
#define HA_CONF		CONFIG_DIR "/ha.conf"
#define UI_CONF		CONFIG_DIR "/ui.conf"
#define WEB_CONF	CONFIG_DIR "/web.conf"
#define WIFI_CONF	CONFIG_DIR "/wpa_supplicant.conf"
#define LANG_CONF	CONFIG_DIR "/language"
#define FONT_DIR	"/usr/share/toon/fonts"

/* The screen comes from the framebuffer: 1024x600 on the Toon 2, 800x480 on
 * the Toon 1. Every fixed size in pixels is written for 1024 wide and goes
 * through S(): the same on the Toon 2, scaled down on the Toon 1. The two 7"
 * panels differ in pixel density by the same factor, so text and buttons keep
 * their physical size. */
extern int screen_w, screen_h;
#define SCREEN_W	screen_w
#define SCREEN_H	screen_h
#define S(px)		((int)(px) * screen_w / 1024)
#define BAR_H		S(70)	/* the bar at the bottom: pages left, clock and status right */
#define STATUS_W	S(252)

#define C_BG		0x000000
#define C_SIDEBAR	0x151a23
#define C_CARD		0x1c222d
#define C_CARD_HI	0x283040
#define C_TEXT		0xe9edf3
#define C_MUTED		0x8c96a8
#define C_ACCENT	0x4da3ff
#define C_SEL		0x193550	/* a thermostat's chosen preset */
#define C_SEL_TEXT	0xa6d7ff
#define C_OK		0x4caf50
#define C_WARN		0xffb300
#define C_BAD		0xef5350

/* fonts (main.c) */
extern lv_font_t *f_small, *f_body, *f_head, *f_mid, *f_big;
extern lv_font_t *i_tiny, *i_small, *i_mid, *i_big;
lv_font_t *clock_font(int size);	/* the clock's digits at another size */
void clock_fonts_new(void);		/* the pages are built again: old sizes may go */
/* UTF-8 text of an "mdi:name" icon; fallback when the name is unknown */
const char *mdi(const char *name, const char *fallback);

/* key=value files */
int file_value(const char *path, const char *key, char *out, size_t len);

/* i18n.c: the screen in Dutch (the default) or English */
extern bool lang_nl;
void lang_load(void);
const char *tr(const char *en) __attribute__((format_arg(1)));
const char *state_text(const char *state);	/* "on" -> "Aan"; NULL: keep it */
const char *status_text(const char *s, char *buf, size_t len);
struct tm;
void date_text(char *out, size_t len, const struct tm *tm);
uint32_t now_ms(void);
void set_status(const char *key, const char *value);

/* background commands: cb gets the exit status and everything on stdout */
typedef void (*job_cb)(int status, char *out, void *user);
int job_run(char *const argv[], job_cb cb, void *user);
void jobs_poll(void);

/* entities (ha.c) */
struct entity {
	char id[96];
	char *state;		/* NULL = not known yet */
	cJSON *attr;		/* object, never NULL */
	bool dirty, wanted;
	unsigned gen;		/* raised when the state or an attribute changed */
	struct entity *next;
};
struct entity *ent_get(const char *id);
void ent_set(struct entity *e, const char *state);
const char *ent_str(struct entity *e, const char *attr);
double ent_num(struct entity *e, const char *attr, double def);
int ent_precision(struct entity *e);	/* Home Assistant's display precision; -1: none */
const char *fmt_number(struct entity *e, const char *s, double v);	/* dash.c */
void ent_bind(const char *id, void (*cb)(void *user), void *user);
void ent_unbind_all(void);
void ent_flush(void);
void ent_want(const char *const *ids, int n);	/* keep these subscribed, page or not */
bool ent_want_apps(const char *const *ids, int n);	/* the same for apps; true: new ones */
struct entity *ent_find(const char *id);	/* NULL when nothing uses it */
static inline bool is_local(const char *id) { return !strncmp(id, "toon.", 5); }
static inline bool is_mqtt(const char *id) { return !strncmp(id, "mqtt.", 5); }

/* Home Assistant link (ha.c) */
void ha_init(void);
void ha_poll(void);
void ha_restart(void);		/* after a new dashboard or new settings */
bool ha_ready(void);
const char *ha_status(void);
void ha_call(const char *domain, const char *service, const char *entity, cJSON *data);
/* a question over the websocket (o gets its id and is freed); cb gets the
   answer's "result", or NULL when it failed or the link went down. false:
   not connected, cb is not called */
typedef void (*ha_cb)(cJSON *result, void *user);
bool ha_request(cJSON *o, ha_cb cb, void *user);
const char *ha_error(void);	/* why the last ha_request failed (in its cb) */
bool ha_http(char *base, size_t bn, char *resolve, size_t rn, bool *check);

/* the unit's own values as toon.* entities (local.c) */
void local_init(void);
void local_poll(void);
void local_call(const char *domain, const char *service, const char *entity, cJSON *data);
int backlight_get(void);		/* percent, -1 unknown */
void backlight_set(int percent);

/* values straight from an MQTT broker as mqtt.* entities (mqtt.c) */
void mqtt_init(void);
void mqtt_poll(void);
void mqtt_restart(void);	/* after a new dashboard */
int mqtt_state(void);		/* 0 not set up, 1 no connection, 2 connected */
const char *mqtt_status(void);
void mqtt_call(const char *domain, const char *service, const char *entity, cJSON *data);
const char *mqtt_domain(const char *id);	/* "light", "sensor", ... */

/* dashboards (dash.c) */
void dash_init(lv_obj_t *parent);
void dash_load(void);
void dash_poll(void);
void dash_show(int view);
int dash_current(void);		/* the page shown, 0 = the home page */
/* show (true) or hide the home page's dimmed page; false if there is none */
bool dash_dim(bool on);
int dash_view(void);
int dash_views(void);
uint32_t dash_color(const char *name, uint32_t def);	/* "red", "#rrggbb" */

/* graphs: history-graph, statistics-graph, sensor with graph: line (graph.c) */
lv_obj_t *graph_card(lv_obj_t *parent, cJSON *cfg, int w);
void graph_set_height(lv_obj_t *root, int h);
void graph_reset(void);		/* before the dashboard is built again */
void graph_poll(void);
void graph_save(void);		/* when toon-ui stops */

/* Home Assistant's energy cards (energy.c) */
lv_obj_t *energy_card(lv_obj_t *parent, cJSON *cfg, const char *type, int w);	/* NULL: not one */
bool energy_is_graph(const char *type);
bool energy_changed(void);	/* its settings came or changed: build the dashboard again */
void energy_reset(void);	/* before the dashboard is built again */
void energy_poll(void);

/* WiFi screen (wifi.c) */
void wifi_init(void);
void wifi_poll(void);
void wifi_open(bool automatic);
bool wifi_connected(void);
int wifi_signal(void);		/* dBm, 0 unknown */
void wifi_ip(char *out, size_t len);	/* "-" when there is none */

/* rescue mode, started from the initrd (rescue.c) */
void rescue_init(lv_obj_t *scr, const char *reason);
void rescue_poll(void);

/* apps: JavaScript from p4, see apps.c */
/* bus.c: the Toon's own MQTT broker, for apps */
typedef void (*appbus_cb)(const char *topic, const char *payload, size_t len, bool retained);
bool appbus_publish(const char *topic, const void *payload, size_t len, bool retain);
void appbus_subscribe(const char *pattern);
void appbus_unsubscribe(const char *pattern);
bool appbus_online(void);
void appbus_poll(appbus_cb cb);

void apps_init(void);
lv_obj_t *apps_camera_picture(lv_obj_t *parent, const char *camera, int w, int h);
void apps_poll(void);
lv_obj_t *apps_card(lv_obj_t *parent, cJSON *cfg, int w);
void apps_close(void);		/* the open app screen */

/* touch calibration (touchcal.c) */
bool touchcal_init(lv_indev_t *indev, const char *path);	/* true: open the screen */
void touchcal_open(bool automatic);
void touchcal_reset(void);

/* notifications (notify.c): banner or popup, with a sound. cb (an app's)
   is called once at the end: event "button" (with its label), "dismissed"
   or "gone" (timed out or replaced) */
typedef void (*notify_cb)(void *user, const char *event, const char *button);
bool notify_show(const char *json, notify_cb cb, void *user);
void notify_command(const char *line);
void notify_sound(const char *name, int volume);	/* volume -1: the setting */
void notify_poll(void);

/* info screen (main.c) */
void info_open(void);

#endif
