// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * toon-ui: Home Assistant's energy cards, from its energy settings
 * (energy/get_prefs: the grid's meters in and out, solar, battery, gas,
 * water).
 *   energy-usage-graph   the grid's energy in and out an hour, as a
 *                        statistics-graph of bars (the last 24 hours)
 *   energy-solar-graph   solar an hour, the same way
 *   energy-gas-graph     gas an hour, the same way
 *   energy-distribution  today's totals: grid in and out, solar, battery,
 *                        gas, water and what the house used
 *   energy-date-selection, energy-compare: nothing (the Toon shows today)
 * The settings are asked once Home Assistant is connected and again every
 * hour; when they are new or changed the dashboard is built again.
 */
#include <math.h>
#include <string.h>
#include <time.h>
#include "ui.h"

#define MAXID	6

struct prefs {
	char in[MAXID][96], out[MAXID][96], solar[MAXID][96], gas[MAXID][96];
	char bat_in[MAXID][96], bat_out[MAXID][96];	/* charging, discharging */
	char water[MAXID][96];
	int nin, nout, nsolar, ngas, nbat_in, nbat_out, nwater;
};

static struct prefs ep;
static int ep_state;		/* 0 not asked, 1 asked, 2 known */
static bool wanted, changed;
static uint32_t t_asked;
static int gen = 1;		/* cards of an older dashboard: their answers are dropped */
static cJSON *made;		/* the statistics-graph settings made for the graphs */

/* ---- the settings --------------------------------------------------------------- */

static void add_id(char (*list)[96], int *n, cJSON *o, const char *key)
{
	cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

	if (cJSON_IsString(v) && v->valuestring[0] && *n < MAXID)
		snprintf(list[(*n)++], 96, "%s", v->valuestring);
}

static void on_prefs(cJSON *res, void *user)
{
	struct prefs p = { 0 };
	cJSON *src, *it, *f;

	(void)user;
	if (!res && ep_state == 2)
		return;		/* the link went down: keep what was known */
	/* no answer at all: Home Assistant has no energy settings (asked
	   again in an hour) */
	src = cJSON_GetObjectItemCaseSensitive(res, "energy_sources");
	cJSON_ArrayForEach(it, src) {
		cJSON *type = cJSON_GetObjectItemCaseSensitive(it, "type");

		if (!cJSON_IsString(type))
			continue;
		if (!strcmp(type->valuestring, "grid")) {
			/* older Home Assistant: lists of meters; newer: one each */
			cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(it, "flow_from"))
				add_id(p.in, &p.nin, f, "stat_energy_from");
			cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(it, "flow_to"))
				add_id(p.out, &p.nout, f, "stat_energy_to");
			add_id(p.in, &p.nin, it, "stat_energy_from");
			add_id(p.out, &p.nout, it, "stat_energy_to");
		} else if (!strcmp(type->valuestring, "battery")) {
			add_id(p.bat_out, &p.nbat_out, it, "stat_energy_from");
			add_id(p.bat_in, &p.nbat_in, it, "stat_energy_to");
		} else if (!strcmp(type->valuestring, "solar")) {
			add_id(p.solar, &p.nsolar, it, "stat_energy_from");
		} else if (!strcmp(type->valuestring, "gas")) {
			add_id(p.gas, &p.ngas, it, "stat_energy_from");
		} else if (!strcmp(type->valuestring, "water")) {
			add_id(p.water, &p.nwater, it, "stat_energy_from");
		}
	}
	if (ep_state != 2 || memcmp(&p, &ep, sizeof(p)))
		changed = wanted;	/* only a dashboard with energy cards is built again */
	ep = p;
	ep_state = 2;
}

/* the dashboard asks: built again because the settings came or changed */
bool energy_changed(void)
{
	bool c = changed;

	changed = false;
	return c;
}

/* ---- today's totals --------------------------------------------------------------- */

struct dist {
	/* grid in, grid out, solar, gas, house, battery out (to the house),
	   battery in, water */
	lv_obj_t *root, *val[8], *ring[3];	/* the house's ring: from solar, battery, grid */
	int gen;
	uint32_t t_asked;
	bool asked;
	struct dist *next;
};

static struct dist *dists;

struct dist_ask {
	struct dist *d;
	int gen;
};

static double sum_of(cJSON *res, char (*list)[96], int n)
{
	double total = 0;

	for (int i = 0; i < n; i++) {
		cJSON *rows = cJSON_GetObjectItemCaseSensitive(res, list[i]), *r;

		cJSON_ArrayForEach(r, rows) {
			cJSON *c = cJSON_GetObjectItemCaseSensitive(r, "change");

			if (cJSON_IsNumber(c))
				total += c->valuedouble;
		}
	}
	return total;
}

static void set_value(lv_obj_t *l, double v, const char *unit)
{
	char buf[32];

	if (!l)
		return;
	/* as Home Assistant: small amounts with two decimals (0.06 kWh,
	   0.12 m³), nothing at all as 0 */
	if (fabs(v) < 0.005)
		snprintf(buf, sizeof(buf), "0 %s", unit);
	else
		snprintf(buf, sizeof(buf), "%.*f %s", fabs(v) < 10 ? 2 : fabs(v) < 100 ? 1 : 0, v, unit);
	lv_label_set_text(l, buf);
}

static void on_totals(cJSON *res, void *user)
{
	struct dist_ask a = *(struct dist_ask *)user;
	double in, out, solar, bat_out, bat_in, s_grid, s_bat, b_grid, g_bat, from[3], home;
	int angle = 270;

	free(user);
	if (a.gen != gen || !res)
		return;
	a.d->asked = false;
	in = sum_of(res, ep.in, ep.nin);
	out = sum_of(res, ep.out, ep.nout);
	solar = sum_of(res, ep.solar, ep.nsolar);
	bat_out = sum_of(res, ep.bat_out, ep.nbat_out);
	bat_in = sum_of(res, ep.bat_in, ep.nbat_in);
	set_value(a.d->val[0], in, "kWh");
	set_value(a.d->val[1], out, "kWh");
	set_value(a.d->val[2], solar, "kWh");
	set_value(a.d->val[3], sum_of(res, ep.gas, ep.ngas), "m³");
	set_value(a.d->val[5], bat_out, "kWh");
	set_value(a.d->val[6], bat_in, "kWh");
	set_value(a.d->val[7], sum_of(res, ep.water, ep.nwater), "m³");
	/* who gave what: what went back to the grid came from the sun first,
	   what the battery took too; the rest of each went to the house */
	s_grid = LV_MIN(out, solar);
	b_grid = LV_MIN(out - s_grid, bat_out);
	s_bat = LV_MIN(bat_in, solar - s_grid);
	g_bat = LV_MIN(bat_in - s_bat, in);
	from[0] = LV_MAX(0, solar - s_grid - s_bat);
	from[1] = LV_MAX(0, bat_out - b_grid);
	from[2] = LV_MAX(0, in - g_bat);
	home = from[0] + from[1] + from[2];
	set_value(a.d->val[4], home, "kWh");
	/* the house's ring in the colours of where it came from */
	for (int i = 0; i < 3; i++) {
		int span = home > 0 ? (int)lround(360 * from[i] / home) : i == 2 ? 360 : 0;

		if (!a.d->ring[i])
			continue;
		if (i == 2)
			span = 270 + 360 - angle;	/* the rest, so it closes */
		if (span <= 0) {
			lv_obj_add_flag(a.d->ring[i], LV_OBJ_FLAG_HIDDEN);
			continue;
		}
		lv_obj_remove_flag(a.d->ring[i], LV_OBJ_FLAG_HIDDEN);
		if (span >= 360)
			lv_arc_set_angles(a.d->ring[i], 0, 360);
		else
			lv_arc_set_angles(a.d->ring[i], angle % 360, (angle + span) % 360);
		angle += span;
	}
}

static void add_ids(cJSON *ids, char (*list)[96], int n)
{
	for (int i = 0; i < n; i++)
		cJSON_AddItemToArray(ids, cJSON_CreateString(list[i]));
}

static void ask_totals(struct dist *d)
{
	time_t now = time(NULL);
	struct tm tm;
	char start[32];
	cJSON *o = cJSON_CreateObject(), *ids;
	struct dist_ask *a = malloc(sizeof(*a));

	localtime_r(&now, &tm);
	tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
	now = mktime(&tm);
	gmtime_r(&now, &tm);
	strftime(start, sizeof(start), "%Y-%m-%dT%H:%M:%SZ", &tm);
	cJSON_AddStringToObject(o, "type", "recorder/statistics_during_period");
	cJSON_AddStringToObject(o, "start_time", start);
	cJSON_AddStringToObject(o, "period", "hour");
	ids = cJSON_AddArrayToObject(o, "statistic_ids");
	add_ids(ids, ep.in, ep.nin);
	add_ids(ids, ep.out, ep.nout);
	add_ids(ids, ep.solar, ep.nsolar);
	add_ids(ids, ep.gas, ep.ngas);
	add_ids(ids, ep.bat_in, ep.nbat_in);
	add_ids(ids, ep.bat_out, ep.nbat_out);
	add_ids(ids, ep.water, ep.nwater);
	ids = cJSON_AddArrayToObject(o, "types");
	cJSON_AddItemToArray(ids, cJSON_CreateString("change"));
	/* in the units the card shows: Home Assistant converts (a water meter
	   in litres or gallons, electricity in Wh or MWh) */
	ids = cJSON_AddObjectToObject(o, "units");
	cJSON_AddStringToObject(ids, "volume", "m³");
	cJSON_AddStringToObject(ids, "energy", "kWh");
	*a = (struct dist_ask){ d, gen };
	d->t_asked = now_ms();
	if (ha_request(o, on_totals, a))
		d->asked = true;
	else
		free(a);
}

/* ---- the cards --------------------------------------------------------------------- */

static lv_obj_t *card_panel(lv_obj_t *parent, int w, int h)
{
	lv_obj_t *o = lv_obj_create(parent);

	lv_obj_remove_style_all(o);
	lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_size(o, w, h);
	lv_obj_set_style_bg_color(o, lv_color_hex(C_CARD), 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(o, S(14), 0);
	return o;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *s)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_label_set_text(l, s);
	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
	return l;
}

/* waiting for the settings, or Home Assistant has none of this kind */
static lv_obj_t *note(lv_obj_t *parent, int w, const char *msg)
{
	lv_obj_t *o = card_panel(parent, w, S(44)), *l = label(o, f_small, C_MUTED, msg);

	lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
	lv_obj_set_width(l, w - S(28));
	lv_obj_align(l, LV_ALIGN_LEFT_MID, S(14), 0);
	return o;
}

/*
 * The distribution, as Home Assistant draws it: solar on top in the middle
 * and gas on top above the house, the grid on the left and the house on the
 * right, a battery under them in the middle and water under the house. A
 * circle holds its icon and its value(s); its name is above the circles on
 * top and under the others, so no line crosses a name.
 */
#define DIST_R		S(56)
#define DIST_NAME	S(24)	/* the height of a name */
#define DIST_GAP	S(24)	/* between a circle and the next row */

static lv_obj_t *circle(lv_obj_t *root, int cx, int cy, uint32_t color, const char *icon,
			const char *name, bool name_above)
{
	int r = DIST_R;
	lv_obj_t *c = lv_obj_create(root), *l;

	lv_obj_remove_style_all(c);
	lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_size(c, 2 * r, 2 * r);
	lv_obj_set_pos(c, cx - r, cy - r);
	lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_border_width(c, S(4), 0);
	lv_obj_set_style_border_color(c, lv_color_hex(color), 0);
	lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD), 0);
	lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);	/* lines end under it */
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(c, S(1), 0);
	label(c, i_small, color, mdi(icon, ""));
	if (!name)		/* a line runs where the name would be */
		return c;
	l = label(root, f_small, C_MUTED, name);
	lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_width(l, 3 * r);
	lv_obj_set_pos(l, cx - 3 * r / 2, name_above ? cy - r - DIST_NAME : cy + r + S(4));
	return c;
}

/* a value in a circle; with an arrow for a way in or out */
static lv_obj_t *value(lv_obj_t *c, const char *arrow, uint32_t color)
{
	lv_obj_t *row = lv_obj_create(c), *v;

	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, S(2), 0);
	if (arrow)
		label(row, i_tiny, color, mdi(arrow, ""));
	v = label(row, f_small, arrow ? color : C_TEXT, "…");
	return v;
}

static void on_line_delete(lv_event_t *ev)
{
	free(lv_event_get_user_data(ev));
}

static void line_points(lv_obj_t *root, lv_point_precise_t *p, int n, uint32_t color)
{
	lv_obj_t *l = lv_line_create(root);

	lv_line_set_points(l, p, n);
	lv_obj_set_style_line_width(l, S(2), 0);
	lv_obj_set_style_line_rounded(l, true, 0);
	lv_obj_set_style_line_color(l, lv_color_hex(color), 0);
	lv_obj_set_style_line_opa(l, LV_OPA_80, 0);
	lv_obj_add_event_cb(l, on_line_delete, LV_EVENT_DELETE, p);
}

/* a straight line between two circles' middles (the circles cover its ends) */
static void draw_link(lv_obj_t *root, int x1, int y1, int x2, int y2, uint32_t color)
{
	lv_point_precise_t *p = malloc(2 * sizeof(*p));

	p[0] = (lv_point_precise_t){ x1, y1 };
	p[1] = (lv_point_precise_t){ x2, y2 };
	line_points(root, p, 2, color);
}

/* as Home Assistant draws a flow: from (x, y0) straight up or down to the
   height yc, round the corner, and level to the circle at x1 */
#define BEND_STEPS	8
static void draw_bend(lv_obj_t *root, int x, int y0, int yc, int x1, uint32_t color)
{
	int rad = S(26), sx = x1 < x ? -1 : 1, sy = yc < y0 ? -1 : 1, n = 0;
	lv_point_precise_t *p = malloc((BEND_STEPS + 3) * sizeof(*p));

	p[n++] = (lv_point_precise_t){ x, y0 };
	for (int i = 0; i <= BEND_STEPS; i++) {
		double a = M_PI / 2 * i / BEND_STEPS;

		p[n++] = (lv_point_precise_t){ x + sx * rad * (1 - cos(a)), yc - sy * rad * (1 - sin(a)) };
	}
	p[n++] = (lv_point_precise_t){ x1, yc };
	line_points(root, p, n, color);
}

/* Home Assistant's colours */
#define E_GRID_IN	0x488fc2
#define E_GRID_OUT	0x8353d1
#define E_SOLAR		0xff9800
#define E_BAT_IN	0xf06292
#define E_BAT_OUT	0x4db6ac
#define E_GAS		0x8e021b
#define E_WATER		0x00bcd4

/* the house's ring: an arc per source, over the circle's own border */
static lv_obj_t *ring_part(lv_obj_t *root, int cx, int cy, uint32_t color)
{
	lv_obj_t *a = lv_arc_create(root);

	lv_obj_remove_style(a, NULL, LV_PART_KNOB);
	lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_size(a, 2 * DIST_R, 2 * DIST_R);
	lv_obj_set_pos(a, cx - DIST_R, cy - DIST_R);
	lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);
	lv_obj_set_style_arc_width(a, S(5), LV_PART_INDICATOR);
	lv_obj_set_style_arc_rounded(a, false, LV_PART_INDICATOR);
	lv_obj_set_style_arc_color(a, lv_color_hex(color), LV_PART_INDICATOR);
	lv_arc_set_bg_angles(a, 0, 360);
	lv_arc_set_angles(a, 0, 0);
	lv_obj_add_flag(a, LV_OBJ_FLAG_HIDDEN);
	return a;
}

static void on_dist_delete(lv_event_t *ev)
{
	struct dist *d = lv_event_get_user_data(ev), **pp;

	for (pp = &dists; *pp; pp = &(*pp)->next)
		if (*pp == d) {
			*pp = d->next;
			break;
		}
	free(d);
}

/* the rows: where the middle (grid, house) and the bottom row are */
static int dist_rows(bool title, int *y_solar, int *y_mid, int *y_low)
{
	int r = DIST_R, top = title ? S(48) : S(14);
	bool high = ep.nsolar || ep.ngas, low = ep.nbat_in || ep.nbat_out || ep.nwater;

	*y_solar = top + DIST_NAME + r;
	*y_mid = high ? *y_solar + 2 * r + DIST_GAP : top + r;
	*y_low = *y_mid + 2 * r + DIST_NAME + DIST_GAP - S(10);
	return (low ? *y_low : *y_mid) + r + DIST_NAME + S(14);
}

static lv_obj_t *distribution(lv_obj_t *parent, cJSON *cfg, int w)
{
	const char *title = cJSON_IsString(cJSON_GetObjectItemCaseSensitive(cfg, "title")) ?
			    cJSON_GetObjectItemCaseSensitive(cfg, "title")->valuestring : NULL;
	int ys, ym, yl, h = dist_rows(title, &ys, &ym, &yl);
	int xg = S(16) + DIST_R * 3 / 2, xh = w - xg, xm = w / 2, o = S(10);
	bool bat = ep.nbat_in || ep.nbat_out;
	struct dist *d = calloc(1, sizeof(*d));
	lv_obj_t *root = card_panel(parent, w, h), *c;

	if (title) {
		lv_obj_t *l = label(root, f_head, C_TEXT, title);

		lv_obj_set_pos(l, S(16), S(12));
	}
	/* the lines first: the circles cover their ends. The grid to the
	   house is level; the sun's flows bend in above it, the battery's
	   below it, each on its own side of the middle */
	draw_link(root, xg, ym, xh, ym, E_GRID_IN);
	if (ep.nsolar) {
		draw_bend(root, xm + o, ys, ym - o, xh, E_SOLAR);
		if (ep.nout)
			draw_bend(root, xm - o, ys, ym - o, xg, E_GRID_OUT);
		if (bat)
			draw_link(root, xm, ys, xm, yl, E_BAT_IN);
	}
	if (bat) {
		draw_bend(root, xm + o, yl, ym + o, xh, E_BAT_OUT);
		draw_bend(root, xm - o, yl, ym + o, xg, E_GRID_IN);
	}
	/* gas from above and water from below, straight into the house */
	if (ep.ngas)
		draw_link(root, xh, ys, xh, ym, E_GAS);
	if (ep.nwater)
		draw_link(root, xh, yl, xh, ym, E_WATER);

	if (ep.nsolar) {
		c = circle(root, xm, ys, E_SOLAR, "mdi:solar-power", tr("Solar"), true);
		d->val[2] = value(c, NULL, 0);
	}
	c = circle(root, xg, ym, E_GRID_IN, "mdi:transmission-tower", tr("Grid"), false);
	if (ep.nout)
		d->val[1] = value(c, "mdi:arrow-left", E_GRID_OUT);
	d->val[0] = value(c, "mdi:arrow-right", E_GRID_IN);
	/* without a name when water comes in from below, as in Home Assistant */
	c = circle(root, xh, ym, E_GRID_IN, "mdi:home", ep.nwater ? NULL : tr("House"), false);
	lv_obj_set_style_border_width(c, S(5), 0);
	d->val[4] = value(c, NULL, 0);
	if (ep.nsolar)
		d->ring[0] = ring_part(root, xh, ym, E_SOLAR);
	if (bat)
		d->ring[1] = ring_part(root, xh, ym, E_BAT_OUT);
	d->ring[2] = ring_part(root, xh, ym, E_GRID_IN);
	if (bat) {
		c = circle(root, xm, yl, E_BAT_OUT, "mdi:battery-high", tr("Battery"), false);
		d->val[6] = value(c, "mdi:arrow-down", E_BAT_IN);
		d->val[5] = value(c, "mdi:arrow-up", E_BAT_OUT);
	}
	if (ep.ngas) {
		c = circle(root, xh, ys, E_GAS, "mdi:fire", tr("Gas"), true);
		d->val[3] = value(c, NULL, 0);
	}
	if (ep.nwater) {
		c = circle(root, xh, yl, E_WATER, "mdi:water", tr("Water"), false);
		d->val[7] = value(c, NULL, 0);
	}
	d->root = root;
	d->gen = gen;
	d->next = dists;
	dists = d;
	lv_obj_add_event_cb(root, on_dist_delete, LV_EVENT_DELETE, d);
	return root;
}

/* the graphs: a statistics-graph of what an hour brought, made from the settings */
static lv_obj_t *graph(lv_obj_t *parent, const char *type, cJSON *cfg, int w)
{
	cJSON *g = cJSON_CreateObject(), *list, *e;
	const char *title = cJSON_IsString(cJSON_GetObjectItemCaseSensitive(cfg, "title")) ?
			    cJSON_GetObjectItemCaseSensitive(cfg, "title")->valuestring : NULL;
	int n = 0;

	if (!made)
		made = cJSON_CreateArray();
	cJSON_AddItemToArray(made, g);
	cJSON_AddStringToObject(g, "type", "statistics-graph");
	list = cJSON_AddArrayToObject(g, "entities");
#define ADD(arr, cnt, name, color, below) \
	for (int i = 0; i < (cnt); i++, n++) { \
		e = cJSON_CreateObject(); \
		cJSON_AddStringToObject(e, "entity", (arr)[i]); \
		if ((cnt) == 1)	/* more meters: each by its own name */ \
			cJSON_AddStringToObject(e, "name", (name)); \
		cJSON_AddStringToObject(e, "color", (color)); \
		if (below) \
			cJSON_AddBoolToObject(e, "invert", true); \
		cJSON_AddItemToArray(list, e); \
	}
	if (!strcmp(type, "energy-usage-graph")) {
		/* as Home Assistant: from the grid up, back to the grid below zero */
		ADD(ep.in, ep.nin, tr("From the grid"), "#488fc2", false);
		ADD(ep.out, ep.nout, tr("To the grid"), "#8353d1", true);
		if (!title)
			title = tr("Energy usage");
	} else if (!strcmp(type, "energy-solar-graph")) {
		ADD(ep.solar, ep.nsolar, tr("Solar"), "#ff9800", false);
		if (!title)
			title = tr("Solar production");
	} else {
		ADD(ep.gas, ep.ngas, tr("Gas"), "#e91e63", false);
		if (!title)
			title = tr("Gas usage");
	}
#undef ADD
	if (!n)
		return note(parent, w, tr("Home Assistant's energy settings have nothing for this card"));
	cJSON_AddStringToObject(g, "title", title);
	cJSON_AddStringToObject(g, "period", "hour");
	cJSON_AddStringToObject(g, "stat_types", "change");
	cJSON_AddStringToObject(g, "chart_type", "bar");
	cJSON_AddNumberToObject(g, "days_to_show", 1);
	/* as Home Assistant's energy graphs: the meters of an hour stacked in one
	   bar, and no meter readings in the legend; there each meter's total of
	   the 24 hours shown */
	cJSON_AddBoolToObject(g, "stack", true);
	cJSON_AddBoolToObject(g, "show_values", false);
	cJSON_AddBoolToObject(g, "show_totals", true);
	return graph_card(parent, g, w);
}

bool energy_is_graph(const char *type)
{
	return type && (!strcmp(type, "energy-usage-graph") || !strcmp(type, "energy-solar-graph") ||
			!strcmp(type, "energy-gas-graph"));
}

/* the height a card will have, for the note in its place before the settings
   came: a page laid out then keeps room for it */
static int expected_h(const char *type, cJSON *cfg)
{
	int top = cJSON_GetObjectItemCaseSensitive(cfg, "title") ? S(48) : S(14);

	if (energy_is_graph(type))
		return S(250);
	/* solar, the grid and the house, and a row under them */
	return top + DIST_NAME + 2 * DIST_R + DIST_GAP + 2 * DIST_R + DIST_NAME + DIST_GAP - S(10) +
	       2 * DIST_R + DIST_NAME + S(14);
}

/* NULL: not an energy card this file knows */
lv_obj_t *energy_card(lv_obj_t *parent, cJSON *cfg, const char *type, int w)
{
	if (!type || strncmp(type, "energy-", 7))
		return NULL;
	if (!strcmp(type, "energy-date-selection") || !strcmp(type, "energy-compare")) {
		lv_obj_t *o = lv_obj_create(parent);	/* the Toon shows today */

		lv_obj_remove_style_all(o);
		lv_obj_set_size(o, 0, 0);
		return o;
	}
	if (!energy_is_graph(type) && strcmp(type, "energy-distribution"))
		return NULL;
	wanted = true;
	if (ep_state != 2) {
		lv_obj_t *o = note(parent, w, !strcmp(ha_status(), "Not configured") ?
				   tr("Energy cards need a link to Home Assistant") :
				   tr("Waiting for Home Assistant's energy settings…"));

		lv_obj_set_height(o, expected_h(type, cfg));
		return o;
	}
	if (!ep.nin && !ep.nsolar && !ep.ngas)
		return note(parent, w, tr("No energy settings in Home Assistant"));
	if (energy_is_graph(type))
		return graph(parent, type, cfg, w);
	return distribution(parent, cfg, w);
}

/* before the dashboard is built again */
void energy_reset(void)
{
	cJSON_Delete(made);
	made = NULL;
	wanted = false;
	gen++;
}

void energy_poll(void)
{
	static uint32_t t_tick;
	uint32_t now = now_ms();

	if (now - t_tick < 1000)
		return;
	t_tick = now;
	if (!ha_ready()) {
		if (ep_state == 1)
			ep_state = 0;
		return;
	}
	if (wanted && (ep_state == 0 || (ep_state == 2 && now - t_asked > 3600000))) {
		cJSON *o = cJSON_CreateObject();

		cJSON_AddStringToObject(o, "type", "energy/get_prefs");
		t_asked = now;
		if (ha_request(o, on_prefs, NULL))
			ep_state = ep_state == 2 ? 2 : 1;
	}
	/* today's totals: every five minutes */
	for (struct dist *d = dists; d; d = d->next)
		if (d->gen == gen && (!d->t_asked || (!d->asked && now - d->t_asked > 300000) ||
				      (d->asked && now - d->t_asked > 60000)))
			ask_totals(d);
}
