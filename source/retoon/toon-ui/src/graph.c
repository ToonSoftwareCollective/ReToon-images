// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * toon-ui: graphs of a value over time, for Home Assistant's history-graph and
 * statistics-graph cards and a sensor card with "graph: line".
 *
 * Home Assistant entities: the history comes over the websocket ha.c already
 * has (history/history_during_period; for long periods of entities with a
 * state_class the much smaller 5-minute statistics), and every new state is
 * added as it comes. A graph is a row of time buckets, each the time-weighted
 * mean of the value in it. toon.* and mqtt.* entities have no recorder behind
 * them, so this file keeps one: 5-minute means for a week, in /run (survives
 * a restart of toon-ui) and, twice a day and when toon-ui stops, on the data
 * partition (survives a reboot).
 */
#define _GNU_SOURCE		/* strptime, timegm */
#include <math.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "ui.h"

#define MAXS		8		/* lines in one card */
#define MAXG		3		/* plots in one card, one per unit */
#define MAXB		240		/* buckets in a plot */
#define MAXP		800		/* points of a statistics graph */
#define REDRAW_MS	10000		/* a changed graph is drawn again at most this often */
#define SCROLL_MS	60000		/* and this often anyway, as time moves on */
#define GRID		0x2a3140

static const uint32_t palette[MAXS] = {
	C_ACCENT, 0xff9800, 0x4caf50, 0xe91e63, 0x00bcd4, 0xffeb3b, 0x926bc7, 0x9e9e9e,
};

enum { K_HISTORY, K_STATS, K_SENSOR };
enum { L_ASK, L_ASKED, L_DONE, L_LOCAL };

struct pt { double t0, t1, v; };

struct series {
	struct graph *g;
	char id[96], unit[24];
	bool named;			/* the card gives the name */
	bool invert;			/* stacked bars: drawn below zero (energy back to the grid) */
	uint32_t color;
	int plot;
	/* history: bucket k (time / B) lives in slot k % nb */
	long key[MAXB];
	double sum[MAXB], cov[MAXB];
	double last_v;			/* the value since last_t; NAN: not a number */
	time_t last_t;
	int load;
	uint32_t t_retry;
	/* statistics */
	struct pt *pts;
	int npts;
	lv_obj_t *name, *value;
};

struct graph {
	cJSON *cfg;
	int kind, nb, ns, nplots, plot_h;
	int fix_h;		/* a height from the page's grid: the plots share it */
	double B;			/* seconds per bucket */
	time_t span;			/* seconds on the graph */
	char period[12], stat[12];
	bool bars;
	/* the energy graphs, as Home Assistant draws them: the bars of an hour on
	   top of each other (stack), and no meter readings in the legend; instead
	   what each meter counted on the whole graph (totals) */
	bool stack, no_values, totals;
	struct series s[MAXS];
	lv_obj_t *root, *plots[MAXG];
	char units[MAXG][24];
	bool dirty;
	uint32_t t_draw, t_built, t_stats;
	struct graph *next;
};

static struct graph *graphs;
static int gen = 1;
static bool was_ready;
static uint32_t t_tick;

static bool clock_ok(time_t t) { return t > 1700000000; }

static const char *jstr(cJSON *o, const char *key)
{
	cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

	return cJSON_IsString(v) && v->valuestring[0] ? v->valuestring : NULL;
}

static double num_state(const char *s)
{
	char *end;
	double v;

	if (!s || !*s)
		return NAN;
	v = strtod(s, &end);
	return end != s && !*end && isfinite(v) ? v : NAN;
}

static bool is_ha(const char *id) { return !is_local(id) && !is_mqtt(id); }

/* ---- our own recorder for toon.* and mqtt.* ------------------------------ */

#define LB		300		/* seconds per value */
#define LN		2016		/* a week */
#define RUN_DIR		"/run/toon/history"
#define DATA_DIR	CONFIG_DIR "/history"
#define RUN_SAVE_S	900
#define DATA_SAVE_S	(12 * 3600)

struct lstore {
	char id[96];
	uint32_t k[LN];			/* bucket number (time / LB) of each slot */
	float v[LN];
	long cur_k;			/* the bucket being filled */
	double cur_sum, cur_cov, last_v;
	time_t last_t;
	bool used;
	struct lstore *next;
};

static struct lstore *stores;
static time_t t_run_save, t_data_save;

static void store_path(char *out, size_t len, const char *dir, const char *id)
{
	snprintf(out, len, "%s/%s", dir, id);
	for (char *p = out + strlen(dir) + 1; *p; p++)
		if (*p == '/')
			*p = '_';
}

static bool store_read(struct lstore *l, const char *path)
{
	FILE *f = fopen(path, "rb");
	bool ok;

	if (!f)
		return false;
	ok = fread(l->k, sizeof(l->k), 1, f) == 1 && fread(l->v, sizeof(l->v), 1, f) == 1;
	fclose(f);
	if (!ok) {
		memset(l->k, 0, sizeof(l->k));
		memset(l->v, 0, sizeof(l->v));
	}
	return ok;
}

static struct lstore *store_get(const char *id)
{
	struct lstore *l;
	char a[160], b[160];
	struct stat sa, sb;

	for (l = stores; l; l = l->next)
		if (!strcmp(l->id, id))
			return l;
	l = calloc(1, sizeof(*l));
	snprintf(l->id, sizeof(l->id), "%s", id);
	l->last_v = NAN;
	/* the newer of the two copies */
	store_path(a, sizeof(a), RUN_DIR, id);
	store_path(b, sizeof(b), DATA_DIR, id);
	if (!stat(a, &sa) && (stat(b, &sb) || sa.st_mtime >= sb.st_mtime)) {
		if (!store_read(l, a))
			store_read(l, b);
	} else {
		store_read(l, b);
	}
	l->next = stores;
	stores = l;
	return l;
}

static void store_close_bucket(struct lstore *l)
{
	int i = l->cur_k % LN;

	if (l->cur_k && l->cur_cov >= LB / 10) {
		/* the same 5 minutes saved before a restart: the rest of them */
		if (l->k[i] == l->cur_k && l->cur_cov < LB)
			l->v[i] = (l->v[i] * (LB - l->cur_cov) + l->cur_sum) / LB;
		else
			l->v[i] = l->cur_sum / l->cur_cov;
		l->k[i] = l->cur_k;
	}
	l->cur_sum = l->cur_cov = 0;
}

/* the value was last_v from last_t until now; from now on it is v */
static void store_feed(struct lstore *l, time_t now, double v)
{
	if (l->last_t && now > l->last_t && now - l->last_t < 3600 && !isnan(l->last_v)) {
		for (time_t t = l->last_t; t < now;) {
			long k = t / LB;
			time_t end = LV_MIN(now, (time_t)(k + 1) * LB);

			if (k != l->cur_k) {
				store_close_bucket(l);
				l->cur_k = k;
			}
			l->cur_sum += l->last_v * (end - t);
			l->cur_cov += end - t;
			t = end;
		}
	} else if (now / LB != l->cur_k) {
		store_close_bucket(l);
		l->cur_k = now / LB;
	}
	l->last_v = v;
	l->last_t = now;
}

static void stores_write(const char *dir)
{
	char path[160], tmp[168];

	mkdir(dir, 0755);
	for (struct lstore *l = stores; l; l = l->next) {
		FILE *f;

		store_path(path, sizeof(path), dir, l->id);
		snprintf(tmp, sizeof(tmp), "%s.new", path);
		f = fopen(tmp, "wb");
		if (!f)
			continue;
		fwrite(l->k, sizeof(l->k), 1, f);
		fwrite(l->v, sizeof(l->v), 1, f);
		if (fclose(f) || rename(tmp, path))
			unlink(tmp);
	}
}

static void stores_save_data(void)
{
	if (system("mount -o remount,rw /mnt/data 2>/dev/null"))
		return;
	stores_write(DATA_DIR);
	if (system("sync; mount -o remount,ro /mnt/data 2>/dev/null")) { /* the next writer closes it */ }
}

void graph_save(void)
{
	time_t now = time(NULL);

	if (!stores)
		return;
	/* the 5 minutes being filled, so far */
	for (struct lstore *l = stores; l; l = l->next)
		if (l->used && clock_ok(now)) {
			store_feed(l, now, l->last_v);
			store_close_bucket(l);
		}
	stores_write(RUN_DIR);
	stores_save_data();
}

/* ---- buckets ------------------------------------------------------------- */

static void seg_add(struct series *s, double t0, double t1, double v)
{
	struct graph *g = s->g;
	double now = time(NULL);
	long first = (long)(now / g->B) - g->nb + 1;

	if (t1 > now)
		t1 = now;
	if (isnan(v) || t1 <= t0)
		return;
	for (long k = LV_MAX((long)(t0 / g->B), first); k * g->B < t1; k++) {
		int i = k % g->nb;
		double a = LV_MAX(t0, k * g->B), b = LV_MIN(t1, (k + 1) * g->B);

		if (s->key[i] != k) {
			s->key[i] = k;
			s->sum[i] = s->cov[i] = 0;
		}
		if (b > a) {
			s->sum[i] += v * (b - a);
			s->cov[i] += b - a;
		}
	}
}

static void series_clear(struct series *s)
{
	memset(s->key, 0, sizeof(s->key));
	memset(s->sum, 0, sizeof(s->sum));
	memset(s->cov, 0, sizeof(s->cov));
}

/* the value so far until now, then v */
static void series_feed(struct series *s, time_t now, double v)
{
	if (s->last_t && now > s->last_t)
		seg_add(s, s->last_t, now, s->last_v);
	s->last_v = v;
	s->last_t = now;
}

/* from our own recorder: a week of 5-minute means, and the one being filled */
static void series_from_store(struct series *s)
{
	struct lstore *l = store_get(s->id);
	time_t now = time(NULL);

	l->used = true;
	for (int i = 0; i < LN; i++)
		if (l->k[i] && (time_t)l->k[i] * LB > now - s->g->span - LB)
			seg_add(s, (double)l->k[i] * LB, (double)(l->k[i] + 1) * LB, l->v[i]);
	if (l->cur_cov > 0)
		seg_add(s, (double)l->cur_k * LB, l->last_t, l->cur_sum / l->cur_cov);
	if (l->last_t)
		seg_add(s, l->last_t, now, l->last_v);
	s->last_t = now;
}

/* ---- the legend ---------------------------------------------------------- */

static void fmt_value(struct series *s, struct entity *e, char *out, size_t len)
{
	double v = e ? num_state(e->state) : NAN;

	if (!e || !e->state)
		snprintf(out, len, "…");
	else if (isnan(v))
		snprintf(out, len, "%s", tr(!strcmp(e->state, "unavailable") ? "Unavailable" :
					      !strcmp(e->state, "unknown") ? "Unknown" : e->state));
	else	/* with Home Assistant's display precision when it has one */
		snprintf(out, len, "%s%s%s", fmt_number(e, e->state, v), s->unit[0] ? " " : "", s->unit);
}

/* the legend of a graph with totals: what the meter counted on the graph */
static void show_total(struct series *s)
{
	struct entity *e = ent_find(s->id);
	const char *u = e ? ent_str(e, "unit_of_measurement") : NULL;
	double sum = 0;
	char buf[64];

	if (!s->value || !s->npts)
		return;
	for (int i = 0; i < s->npts; i++)
		sum += s->pts[i].v;
	snprintf(buf, sizeof(buf), fabs(sum) < 10 ? "%.2f%s%s" : "%.1f%s%s", sum, u ? " " : "", u ? u : "");
	if (strcmp(lv_label_get_text(s->value), buf))
		lv_label_set_text(s->value, buf);
}

static void on_entity(void *user)
{
	struct series *s = user;
	struct entity *e = ent_find(s->id);
	time_t now = time(NULL);
	const char *u, *fn;
	char buf[64];

	if (!e)
		return;
	u = ent_str(e, "unit_of_measurement");
	if (u && strcmp(u, s->unit)) {
		snprintf(s->unit, sizeof(s->unit), "%s", u);
		s->g->nplots = -1;	/* group the lines again */
	}
	fn = ent_str(e, "friendly_name");
	if (!s->named && fn && s->name && strcmp(lv_label_get_text(s->name), fn))
		lv_label_set_text(s->name, fn);
	if (s->g->totals) {
		show_total(s);
	} else {
		fmt_value(s, e, buf, sizeof(buf));
		if (s->value && strcmp(lv_label_get_text(s->value), buf))
			lv_label_set_text(s->value, buf);
	}
	if (s->g->kind != K_STATS && clock_ok(now))
		series_feed(s, now, num_state(e->state));
	s->g->dirty = true;
}

/* ---- loading from Home Assistant ----------------------------------------- */

struct ask {
	int gen, si;
	struct graph *g;
	time_t t;			/* asked at */
	bool stats;
	time_t from, until;		/* a tail: only this part, added to what is there */
};

static bool ask_tail(struct graph *g, int si, time_t from, time_t until);

static void iso(char *out, size_t len, time_t t)
{
	struct tm tm;

	gmtime_r(&t, &tm);
	strftime(out, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static double json_time(cJSON *v)
{
	struct tm tm = { 0 };

	if (cJSON_IsNumber(v))
		return v->valuedouble > 1e11 ? v->valuedouble / 1000 : v->valuedouble;
	if (cJSON_IsString(v) && strptime(v->valuestring, "%Y-%m-%dT%H:%M:%S", &tm))
		return timegm(&tm);	/* Home Assistant gives UTC */
	return NAN;
}

/* which of a statistics row's values to show */
static double stat_value(struct graph *g, cJSON *row)
{
	static const char *const pref[] = { "mean", "change", "state", "sum" };
	cJSON *v = g->stat[0] ? cJSON_GetObjectItemCaseSensitive(row, g->stat) : NULL;

	for (size_t i = 0; !cJSON_IsNumber(v) && !g->stat[0] && i < 4; i++)
		v = cJSON_GetObjectItemCaseSensitive(row, pref[i]);
	return cJSON_IsNumber(v) ? v->valuedouble : NAN;
}

static void on_result(cJSON *res, void *user)
{
	struct ask a = *(struct ask *)user;
	struct graph *g = a.gen == gen ? a.g : NULL;
	struct series *s;
	cJSON *list, *it;
	double pt = NAN, pv = NAN;

	free(user);
	if (!g)
		return;
	s = &g->s[a.si];
	if (!res) {		/* refused, or the link went down */
		s->load = L_ASK;
		s->t_retry = now_ms() + 60000;
		return;
	}
	s->load = L_DONE;
	list = cJSON_GetObjectItemCaseSensitive(res, s->id);
	if (a.until) {
		/* the minutes after the statistics: history, clipped to them */
		cJSON_ArrayForEach(it, list) {
			cJSON *st = cJSON_GetObjectItemCaseSensitive(it, "s");
			double t = json_time(cJSON_GetObjectItemCaseSensitive(it, "lu"));

			if (isnan(t))
				continue;
			t = LV_MAX(t, (double)a.from);
			if (!isnan(pt))
				seg_add(s, pt, LV_MIN(t, (double)a.until), pv);
			pt = t;
			pv = num_state(cJSON_IsString(st) ? st->valuestring : NULL);
		}
		if (!isnan(pt))
			seg_add(s, pt, a.until, pv);
		g->dirty = true;
		return;
	}
	g->dirty = true;
	g->t_draw = 0;

	if (g->kind == K_STATS) {
		int n = cJSON_GetArraySize(list), skip = LV_MAX(0, n - MAXP), i = 0;
		bool change = !strcmp(g->stat, "change");

		free(s->pts);
		s->pts = calloc(LV_MAX(1, n - skip), sizeof(*s->pts));
		s->npts = 0;
		cJSON_ArrayForEach(it, list) {
			double t0 = json_time(cJSON_GetObjectItemCaseSensitive(it, "start"));
			double t1 = json_time(cJSON_GetObjectItemCaseSensitive(it, "end"));
			double v = stat_value(g, it);

			if (i++ < skip || isnan(t0) || isnan(t1) || isnan(v))
				continue;
			if (!g->stat[0] && !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(it, "mean")) &&
			    cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(it, "change")))
				change = true;
			s->pts[s->npts++] = (struct pt){ t0, t1, v };
		}
		if (change)
			g->bars = true;
		if (g->totals)
			show_total(s);
		return;
	}

	series_clear(s);
	if (a.stats) {
		/* 5-minute statistics: [{"start": ms, "end": ms, "mean": v}] */
		cJSON_ArrayForEach(it, list) {
			double t0 = json_time(cJSON_GetObjectItemCaseSensitive(it, "start"));
			double t1 = json_time(cJSON_GetObjectItemCaseSensitive(it, "end"));
			cJSON *m = cJSON_GetObjectItemCaseSensitive(it, "mean");

			if (!isnan(t0) && !isnan(t1) && cJSON_IsNumber(m))
				seg_add(s, t0, t1, m->valuedouble);
			if (!isnan(t1))
				pt = t1;
		}
		/* the last minutes the statistics do not have yet (up to 10):
		   their history, or else the value now */
		if (!isnan(pt) && a.t - pt > 30 && g->kind != K_STATS &&
		    (a.t - pt > 3600 || !ask_tail(g, a.si, pt, a.t)))
			seg_add(s, pt, a.t, s->last_v);
	} else {
		/* [{"s": "21.3", "lu": 1730000000.5}, ...], oldest first */
		cJSON_ArrayForEach(it, list) {
			cJSON *st = cJSON_GetObjectItemCaseSensitive(it, "s");
			double t = json_time(cJSON_GetObjectItemCaseSensitive(it, "lu"));

			if (isnan(t))
				t = json_time(cJSON_GetObjectItemCaseSensitive(it, "lc"));
			if (isnan(t))
				continue;
			if (!isnan(pt))
				seg_add(s, pt, t, pv);
			pt = t;
			pv = num_state(cJSON_IsString(st) ? st->valuestring : NULL);
		}
		if (!isnan(pt))
			seg_add(s, pt, a.t, pv);
	}
	s->last_t = a.t;	/* from here on what came in meanwhile */
}

static void ask(struct graph *g, int si)
{
	struct series *s = &g->s[si];
	struct entity *e = ent_find(s->id);
	time_t now = time(NULL);
	struct ask *a;
	cJSON *o, *ids;
	char start[32];

	a = calloc(1, sizeof(*a));
	*a = (struct ask){ gen, si, g, now, false, 0, 0 };
	o = cJSON_CreateObject();
	iso(start, sizeof(start), now - g->span);
	cJSON_AddStringToObject(o, "start_time", start);
	/* a fast sensor sends thousands of states an hour, and parsing them
	   takes long without an FPU (Toon 1: 6 h of a 1/s sensor 1.8 s, 24 h
	   8 s): over two hours, entities with statistics use those */
	if (g->kind == K_STATS || (g->span > 2 * 3600 && e && ent_str(e, "state_class"))) {
		a->stats = true;
		cJSON_AddStringToObject(o, "type", "recorder/statistics_during_period");
		cJSON_AddStringToObject(o, "period", g->kind == K_STATS ? g->period : "5minute");
		ids = cJSON_AddArrayToObject(o, "statistic_ids");
		cJSON_AddItemToArray(ids, cJSON_CreateString(s->id));
		ids = cJSON_AddArrayToObject(o, "types");
		if (g->kind == K_STATS && g->stat[0]) {
			cJSON_AddItemToArray(ids, cJSON_CreateString(g->stat));
		} else {
			cJSON_AddItemToArray(ids, cJSON_CreateString("mean"));
			if (g->kind == K_STATS) {
				cJSON_AddItemToArray(ids, cJSON_CreateString("change"));
				cJSON_AddItemToArray(ids, cJSON_CreateString("state"));
			}
		}
	} else {
		cJSON_AddStringToObject(o, "type", "history/history_during_period");
		ids = cJSON_AddArrayToObject(o, "entity_ids");
		cJSON_AddItemToArray(ids, cJSON_CreateString(s->id));
		cJSON_AddBoolToObject(o, "minimal_response", true);
		cJSON_AddBoolToObject(o, "no_attributes", true);
		cJSON_AddBoolToObject(o, "significant_changes_only", false);
	}
	if (ha_request(o, on_result, a))
		s->load = L_ASKED;
	else
		free(a);
}

static bool ask_tail(struct graph *g, int si, time_t from, time_t until)
{
	struct series *s = &g->s[si];
	struct ask *a = calloc(1, sizeof(*a));
	cJSON *o = cJSON_CreateObject(), *ids;
	char start[32];

	*a = (struct ask){ gen, si, g, until, false, from, until };
	iso(start, sizeof(start), from);
	cJSON_AddStringToObject(o, "type", "history/history_during_period");
	cJSON_AddStringToObject(o, "start_time", start);
	ids = cJSON_AddArrayToObject(o, "entity_ids");
	cJSON_AddItemToArray(ids, cJSON_CreateString(s->id));
	cJSON_AddBoolToObject(o, "minimal_response", true);
	cJSON_AddBoolToObject(o, "no_attributes", true);
	cJSON_AddBoolToObject(o, "significant_changes_only", false);
	if (!ha_request(o, on_result, a)) {
		free(a);
		return false;
	}
	s->load = L_ASKED;
	return true;
}

/* ---- drawing ------------------------------------------------------------- */

static void draw_text(lv_layer_t *layer, const char *s, int x, int y, int w, lv_text_align_t al,
		      uint32_t color)
{
	lv_draw_label_dsc_t d;
	lv_area_t a = { x, y, x + w - 1, y + f_small->line_height };

	lv_draw_label_dsc_init(&d);
	d.font = f_small;
	d.color = lv_color_hex(color);
	d.align = al;
	d.text = s;
	d.text_local = 1;
	lv_draw_label(layer, &d, &a);
}

static void draw_line(lv_layer_t *layer, int x1, int y1, int x2, int y2, uint32_t color, int w)
{
	lv_draw_line_dsc_t d;

	lv_draw_line_dsc_init(&d);
	d.p1.x = x1;
	d.p1.y = y1;
	d.p2.x = x2;
	d.p2.y = y2;
	d.color = lv_color_hex(color);
	d.width = w;
	d.round_start = d.round_end = 1;
	lv_draw_line(layer, &d);
}

/* the points of a line: bucket means, or the statistics rows */
static int points(struct series *s, time_t now, struct pt *out)
{
	struct graph *g = s->g;
	int n = 0;

	if (g->kind == K_STATS) {
		memcpy(out, s->pts, s->npts * sizeof(*out));
		return s->npts;
	}
	for (long k = (long)(now / g->B) - g->nb + 1; k <= (long)(now / g->B); k++) {
		int i = k % g->nb;

		if (s->key[i] == k && s->cov[i] > 0)
			out[n++] = (struct pt){ k * g->B, LV_MIN((k + 1) * g->B, (double)now),
						s->sum[i] / s->cov[i] };
	}
	return n;
}

static const char *empty_text(struct graph *g, int p)
{
	bool ha = false, asked = false;

	for (int i = 0; i < g->ns; i++)
		if (g->s[i].plot == p && is_ha(g->s[i].id)) {
			ha = true;
			asked |= g->s[i].load == L_ASKED;
		}
	if (ha && !ha_ready())
		return tr("Waiting for Home Assistant");
	return asked ? tr("Loading…") : tr("No history yet");
}

static void on_draw(lv_event_t *ev)
{
	struct graph *g = lv_event_get_user_data(ev);
	lv_obj_t *obj = lv_event_get_current_target(ev);
	int p = (intptr_t)lv_obj_get_user_data(obj);
	lv_layer_t *layer = lv_event_get_layer(ev);
	lv_area_t ar;
	time_t now = time(NULL);
	double ws = now - g->span, lo = INFINITY, hi = -INFINITY;
	bool axes = g->kind != K_SENSOR;
	static struct pt pts[MAXS][MAXP > MAXB ? MAXP : MAXB];
	/* stacked bars: where each bar starts (the bars under it in that hour) */
	static double base[MAXS][MAXP > MAXB ? MAXP : MAXB];
	int np[MAXS] = { 0 }, x0, x1, y0, y1, lw = LV_MAX(2, S(3)), any = 0;
	double tick = 1;
	char buf[32];

	lv_obj_get_coords(obj, &ar);
	x0 = ar.x1 + (axes ? S(60) : 0);
	x1 = ar.x2 - (axes ? S(6) : 0);
	y0 = ar.y1 + S(8);
	y1 = ar.y2 - (axes ? S(26) : S(4));

	for (int i = 0; i < g->ns; i++) {
		if (g->s[i].plot != p)
			continue;
		np[i] = points(&g->s[i], now, pts[i]);
		for (int j = 0; j < np[i]; j++) {
			base[i][j] = 0;
			if (g->stack) {
				/* below zero for an inverted one; on top of the earlier
				   ones of that hour on the same side of zero */
				if (g->s[i].invert)
					pts[i][j].v = -pts[i][j].v;
				for (int m = 0; m < i; m++) {
					if (g->s[m].plot != p)
						continue;
					for (int k = 0; k < np[m]; k++)
						if (fabs(pts[m][k].t0 - pts[i][j].t0) < 1 &&
						    (pts[m][k].v < 0) == (pts[i][j].v < 0))
							base[i][j] += pts[m][k].v;
				}
				lo = LV_MIN(lo, base[i][j]);
				hi = LV_MAX(hi, base[i][j]);
			}
			lo = LV_MIN(lo, base[i][j] + pts[i][j].v);
			hi = LV_MAX(hi, base[i][j] + pts[i][j].v);
			ws = g->kind == K_STATS ? LV_MIN(ws, pts[i][j].t0) : ws;
		}
		any += np[i];
	}
	if (!any) {
		draw_text(layer, empty_text(g, p), ar.x1, (ar.y1 + ar.y2) / 2 - S(10),
			  lv_area_get_width(&ar), LV_TEXT_ALIGN_CENTER, C_MUTED);
		return;
	}
	/* round steps (1, 2, 2.5 or 5 times a power of ten), two to four of them;
	   a line that never goes below zero does not get a negative axis */
	{
		bool pos = lo >= 0;
		double r, step, m;

		if (g->bars) {
			lo = LV_MIN(lo, 0);
			hi = LV_MAX(hi, 0);
		}
		/* a nearly flat line: at least 1 % of the value (22.0 to 22.2, not
		   22.035 to 22.045), or 1 around zero */
		{
			double want = LV_MAX(fabs(lo + hi) / 2 * 0.01, 1e-9), mid = (lo + hi) / 2;

			if (lo == hi && !lo)
				want = 1;
			if (hi - lo < want) {
				lo = mid - want / 2;
				hi = mid + want / 2;
			}
		}
		r = (hi - lo) / 3;
		m = pow(10, floor(log10(r)));
		step = r / m <= 1 ? m : r / m <= 2 ? 2 * m : r / m <= 2.5 ? 2.5 * m : r / m <= 5 ? 5 * m : 10 * m;
		lo = floor(lo / step) * step;
		hi = ceil(hi / step) * step;
		if (pos && lo < 0)
			lo = 0;
		tick = step;
	}
#define X(t)	(x0 + (int)(((t) - ws) * (x1 - x0) / (now - ws)))
#define Y(v)	(y1 - (int)(((v) - lo) * (y1 - y0) / (hi - lo)))

	if (axes) {
		int dec = tick < 0.01 ? 3 : tick < 0.1 ? 2 : tick < 1 ? 1 : 0;
		struct tm tm;

		if (dec == 1 && fabs(tick * 10 - round(tick * 10)) > 1e-6)
			dec = 2;	/* 0.25 */
		if (dec == 0 && fabs(tick - round(tick)) > 1e-6)
			dec = 1;	/* 2.5 */
		for (double v = lo; v <= hi + tick / 2; v += tick) {
			int y = Y(v);

			draw_line(layer, x0, y, x1, y, GRID, 1);
			snprintf(buf, sizeof(buf), "%.*f", dec, fabs(v) < tick / 1000 ? 0 : v);
			draw_text(layer, buf, ar.x1, y - S(10), S(54), LV_TEXT_ALIGN_RIGHT, C_MUTED);
		}
		for (int i = 0; i <= 3; i++) {
			time_t t = ws + (now - ws) * i / 3;
			int x = X(t);

			localtime_r(&t, &tm);
			if (now - ws > 2 * 86400 + 60)
				snprintf(buf, sizeof(buf), "%d-%d", tm.tm_mday, tm.tm_mon + 1);
			else
				strftime(buf, sizeof(buf), "%H:%M", &tm);
			draw_text(layer, buf, i == 0 ? x0 : i == 3 ? x1 - S(80) : x - S(40), y1 + S(6), S(80),
				  i == 0 ? LV_TEXT_ALIGN_LEFT : i == 3 ? LV_TEXT_ALIGN_RIGHT :
				  LV_TEXT_ALIGN_CENTER, C_MUTED);
		}
	}

	for (int i = 0; i < g->ns; i++) {
		struct pt *q = pts[i];

		if (g->s[i].plot != p)
			continue;
		if (g->bars) {
			lv_draw_rect_dsc_t d;
			int n = 0;

			for (int j = 0; j < g->ns; j++)
				n += g->s[j].plot == p;
			if (g->stack)
				n = 1;		/* one bar an hour, the series on top of each other */
			lv_draw_rect_dsc_init(&d);
			d.bg_color = lv_color_hex(g->s[i].color);
			d.radius = g->stack ? 0 : 2;
			for (int j = 0; j < np[i]; j++) {
				int a = X(q[j].t0), b = X(q[j].t1), w = LV_MAX(1, (b - a) * 8 / 10 / n);
				int k = 0;
				double v0 = base[i][j], v1 = base[i][j] + q[j].v;

				for (int m = 0; m < i && !g->stack; m++)
					k += g->s[m].plot == p;
				a += (b - a) / 10 + k * w;
				lv_area_t r = { a, LV_MIN(Y(v1), Y(v0)), a + w - 1, LV_MAX(Y(v1), Y(v0)) };

				lv_draw_rect(layer, &d, &r);
			}
			continue;
		}
		for (int j = 0; j < np[i]; j++) {
			int xa = X((q[j].t0 + q[j].t1) / 2), ya = Y(q[j].v);

			if (j + 1 < np[i] && q[j + 1].t0 - q[j].t1 < (q[j].t1 - q[j].t0) + 1) {
				int xb = X((q[j + 1].t0 + q[j + 1].t1) / 2), yb = Y(q[j + 1].v);

				draw_line(layer, xa, ya, xb, yb, g->s[i].color, lw);
			} else if (j == 0 || q[j].t0 - q[j - 1].t1 >= (q[j - 1].t1 - q[j - 1].t0) + 1) {
				draw_line(layer, xa, ya, xa + 1, ya, g->s[i].color, lw);	/* alone */
			}
		}
	}
#undef X
#undef Y
}

/* one plot per unit, as Home Assistant does */
static void group(struct graph *g)
{
	int n = 0;

	for (int i = 0; i < g->ns; i++) {
		int p = -1;

		for (int j = 0; j < n && p < 0; j++)
			if (!strcmp(g->units[j], g->s[i].unit))
				p = j;
		if (p < 0 && n < MAXG) {
			p = n++;
			snprintf(g->units[p], sizeof(g->units[p]), "%s", g->s[i].unit);
		}
		g->s[i].plot = p < 0 ? MAXG - 1 : p;
	}
	if (!n)
		n = 1;
	for (int p = 0; p < MAXG; p++) {
		if (p < n && !g->plots[p]) {
			lv_obj_t *o = lv_obj_create(g->root);

			lv_obj_remove_style_all(o);
			lv_obj_set_width(o, lv_pct(100));
			lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
			lv_obj_set_user_data(o, (void *)(intptr_t)p);
			lv_obj_add_event_cb(o, on_draw, LV_EVENT_DRAW_MAIN, g);
			g->plots[p] = o;
		}
		if (g->plots[p]) {
			lv_obj_set_height(g->plots[p], n > 1 ? g->plot_h * 3 / 4 : g->plot_h);
			if (g->fix_h) {
				lv_obj_set_height(g->plots[p], S(20));
				lv_obj_set_flex_grow(g->plots[p], 1);
			}
			if (p < n)
				lv_obj_remove_flag(g->plots[p], LV_OBJ_FLAG_HIDDEN);
			else
				lv_obj_add_flag(g->plots[p], LV_OBJ_FLAG_HIDDEN);
		}
	}
	g->nplots = n;
}

/* ---- the card ------------------------------------------------------------ */

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *s)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_label_set_text(l, s ? s : "");
	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
	return l;
}

static int config_int(cJSON *cfg, const char *key, int def, int lo, int hi)
{
	cJSON *v = cJSON_GetObjectItemCaseSensitive(cfg, key);

	if (cJSON_IsString(v))
		return LV_CLAMP(lo, atoi(v->valuestring), hi);
	return cJSON_IsNumber(v) ? LV_CLAMP(lo, (int)v->valuedouble, hi) : def;
}

static void add_series(struct graph *g, cJSON *it, int w)
{
	const char *id = cJSON_IsString(it) ? it->valuestring : jstr(it, "entity");
	const char *name = cJSON_IsObject(it) ? jstr(it, "name") : NULL;
	const char *color = cJSON_IsObject(it) ? jstr(it, "color") : NULL;
	struct series *s;

	if (!id || g->ns == MAXS)
		return;
	s = &g->s[g->ns];
	s->g = g;
	snprintf(s->id, sizeof(s->id), "%s", id);
	s->named = name;
	s->invert = cJSON_IsObject(it) && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "invert"));
	s->last_v = NAN;
	s->color = color ? dash_color(color, palette[g->ns]) : palette[g->ns];
	s->load = is_ha(id) ? L_ASK : L_LOCAL;
	if (g->kind != K_SENSOR) {
		lv_obj_t *row = lv_obj_create(lv_obj_get_child(g->root, -1)), *dot;

		lv_obj_remove_style_all(row);
		lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(row, S(6), 0);
		dot = lv_obj_create(row);
		lv_obj_remove_style_all(dot);
		lv_obj_set_size(dot, S(12), S(12));
		lv_obj_set_style_radius(dot, S(6), 0);
		lv_obj_set_style_bg_color(dot, lv_color_hex(s->color), 0);
		lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
		s->name = label(row, f_small, C_MUTED, name ? name : id);
		if (!g->no_values || g->totals)
			s->value = label(row, f_small, C_TEXT, "…");
	}
	g->ns++;
}

lv_obj_t *graph_card(lv_obj_t *parent, cJSON *cfg, int w)
{
	const char *type = jstr(cfg, "type"), *title = jstr(cfg, "title"), *p;
	struct graph *g = calloc(1, sizeof(*g));
	cJSON *list = cJSON_GetObjectItemCaseSensitive(cfg, "entities"), *it;
	time_t now = time(NULL);
	int hours;

	g->cfg = cfg;
	g->kind = !strcmp(type, "statistics-graph") ? K_STATS : !strcmp(type, "sensor") ? K_SENSOR :
		  K_HISTORY;
	g->root = lv_obj_create(parent);
	lv_obj_remove_style_all(g->root);
	lv_obj_set_size(g->root, w, LV_SIZE_CONTENT);
	lv_obj_remove_flag(g->root, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_color(g->root, lv_color_hex(C_CARD), 0);
	lv_obj_set_style_bg_opa(g->root, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(g->root, S(14), 0);
	lv_obj_set_style_pad_all(g->root, S(14), 0);
	lv_obj_set_style_pad_row(g->root, S(8), 0);
	lv_obj_set_flex_flow(g->root, LV_FLEX_FLOW_COLUMN);

	if (g->kind == K_STATS) {
		static const struct { const char *n; double s; } per[] = {
			{ "5minute", 300 }, { "hour", 3600 }, { "day", 86400 }, { "week", 7 * 86400 },
			{ "month", 30 * 86400 },
		};
		cJSON *st = cJSON_GetObjectItemCaseSensitive(cfg, "stat_types");

		p = jstr(cfg, "period");
		snprintf(g->period, sizeof(g->period), "hour");
		for (size_t i = 0; p && i < sizeof(per) / sizeof(per[0]); i++)
			if (!strcmp(p, per[i].n))
				snprintf(g->period, sizeof(g->period), "%s", p);
		cJSON_ArrayForEach(it, st)
			if (cJSON_IsString(it) && !g->stat[0] &&
			    (!strcmp(it->valuestring, "mean") || !strcmp(it->valuestring, "min") ||
			     !strcmp(it->valuestring, "max") || !strcmp(it->valuestring, "state") ||
			     !strcmp(it->valuestring, "sum") || !strcmp(it->valuestring, "change")))
				snprintf(g->stat, sizeof(g->stat), "%s", it->valuestring);
		if (cJSON_IsString(st))
			snprintf(g->stat, sizeof(g->stat), "%.11s", st->valuestring);
		g->span = (time_t)config_int(cfg, "days_to_show", 30, 1, 365) * 86400;
		p = jstr(cfg, "chart_type");
		g->bars = (p && !strcmp(p, "bar")) || !strcmp(g->stat, "change");
		/* ReToon's own keys, set by energy.c for its graphs */
		g->stack = g->bars && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cfg, "stack"));
		g->no_values = cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(cfg, "show_values"));
		g->totals = g->bars && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cfg, "show_totals"));
		g->plot_h = S(170);
	} else if (g->kind == K_SENSOR) {
		int detail = config_int(cfg, "detail", 1, 1, 2);

		hours = config_int(cfg, "hours_to_show", 24, 1, 24 * 7);
		g->span = hours * 3600;
		g->nb = LV_CLAMP(2, hours * (detail == 2 ? 6 : 1), MAXB);
		g->plot_h = S(70);
	} else {
		hours = config_int(cfg, "hours_to_show", 24, 1, 24 * 7);
		g->span = hours * 3600;
		g->nb = LV_CLAMP(24, (w - S(100)) / S(4), MAXB);
		g->plot_h = S(170);
	}
	if (g->nb)
		g->B = (double)g->span / g->nb;

	if (g->kind == K_SENSOR) {
		const char *id = jstr(cfg, "entity"), *name = jstr(cfg, "name");
		lv_obj_t *top = lv_obj_create(g->root);

		lv_obj_remove_style_all(top);
		lv_obj_set_size(top, lv_pct(100), LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(top, LV_FLEX_FLOW_COLUMN);
		add_series(g, cJSON_GetObjectItemCaseSensitive(cfg, "entity"), w);
		if (g->ns) {
			struct series *s = &g->s[0];

			s->named = name;
			s->name = label(top, f_body, C_MUTED, name ? name : id);
			lv_label_set_long_mode(s->name, LV_LABEL_LONG_DOT);
			lv_obj_set_width(s->name, lv_pct(100));
			s->value = label(top, f_mid, C_TEXT, "…");
		}
	} else {
		lv_obj_t *legend;

		if (title)
			label(g->root, f_head, C_TEXT, title);
		legend = lv_obj_create(g->root);
		lv_obj_remove_style_all(legend);
		lv_obj_set_size(legend, lv_pct(100), LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW_WRAP);
		lv_obj_set_style_pad_column(legend, S(18), 0);
		lv_obj_set_style_pad_row(legend, S(4), 0);
		cJSON_ArrayForEach(it, list)
			add_series(g, it, w);
	}
	group(g);
	for (int i = 0; i < g->ns; i++) {
		struct series *s = &g->s[i];

		if (s->load == L_LOCAL && g->kind != K_STATS && clock_ok(now))
			series_from_store(s);
		ent_bind(s->id, on_entity, s);
		on_entity(s);
	}
	g->t_built = now_ms();
	g->next = graphs;
	graphs = g;
	return g->root;
}

/* the card gets this height (rows of the page's grid): the plots take
   what the title and legend leave */
void graph_set_height(lv_obj_t *root, int h)
{
	for (struct graph *g = graphs; g; g = g->next)
		if (g->root == root) {
			g->fix_h = h;
			lv_obj_set_height(root, h);
			group(g);
			return;
		}
}

/* before the dashboard is built again (its objects are about to go) */
void graph_reset(void)
{
	while (graphs) {
		struct graph *g = graphs;

		graphs = g->next;
		for (int i = 0; i < g->ns; i++)
			free(g->s[i].pts);
		free(g);
	}
	for (struct lstore *l = stores; l; l = l->next)
		l->used = false;
	gen++;
}

void graph_poll(void)
{
	uint32_t ms = now_ms();
	time_t now = time(NULL);
	bool ready = ha_ready();

	if (ms - t_tick < 1000)
		return;
	t_tick = ms;

	/* Home Assistant came back: fill the gap from its recorder */
	if (ready && !was_ready)
		for (struct graph *g = graphs; g; g = g->next)
			for (int i = 0; i < g->ns; i++)
				if (g->s[i].load == L_DONE)
					g->s[i].load = L_ASK;
	was_ready = ready;

	for (struct graph *g = graphs; g; g = g->next) {
		if (g->nplots < 0)
			group(g);
		/* statistics are made by Home Assistant: ask again now and then */
		if (g->kind == K_STATS && ready && ms - g->t_stats > (strcmp(g->period, "5minute") ?
								     1800000u : 600000u)) {
			g->t_stats = ms;
			for (int i = 0; i < g->ns; i++)
				if (g->s[i].load == L_DONE)
					g->s[i].load = L_ASK;
		}
		for (int i = 0; i < g->ns; i++) {
			struct series *s = &g->s[i];
			struct entity *e = ent_find(s->id);

			/* the entity's attributes say whether statistics exist:
			   wait for them a little */
			if (ready && clock_ok(now) && s->load == L_ASK && (int32_t)(ms - s->t_retry) >= 0 &&
			    ((e && e->state) || ms - g->t_built > 15000))
				ask(g, i);
		}
		if ((g->dirty && ms - g->t_draw >= REDRAW_MS) || ms - g->t_draw >= SCROLL_MS) {
			g->dirty = false;
			g->t_draw = ms;
			if (g->kind != K_STATS && clock_ok(now))
				for (int i = 0; i < g->ns; i++)
					series_feed(&g->s[i], now, g->s[i].last_v);
			for (int p = 0; p < MAXG; p++)
				if (g->plots[p])
					lv_obj_invalidate(g->plots[p]);
		}
	}

	/* our own recorder */
	if (!clock_ok(now))
		return;
	for (struct lstore *l = stores; l; l = l->next) {
		struct entity *e = l->used ? ent_find(l->id) : NULL;

		if (e)
			store_feed(l, now, num_state(e->state));
	}
	if (!t_run_save)
		t_run_save = t_data_save = now;
	if (now - t_run_save >= RUN_SAVE_S) {
		t_run_save = now;
		if (stores)
			stores_write(RUN_DIR);
	}
	if (now - t_data_save >= DATA_SAVE_S) {
		t_data_save = now;
		if (stores)
			stores_save_data();
	}
}
