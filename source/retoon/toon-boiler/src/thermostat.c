// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * toon-boilerd: what the thermostat wants - presets, the week program,
 * pre-heating, frost protection and the flow temperature. The boiler side
 * (hysteresis, the pause between requests, the frames) stays in
 * toon-boilerd.c.
 *
 * Settings: /mnt/data/toon-os/config/thermostat.json, written by the web
 * interface and read again when it changes:
 *
 *   { "presets": { "comfort": 20, "home": 18, "sleep": 15, "away": 12 },
 *     "program_enabled": true,
 *     "program": [ [ ["07:00", "comfort"], ["23:00", "sleep"] ], ... ],
 *                                    seven days, Monday first
 *     "preheat": true, "max_preheat": 180,      minutes
 *     "modulation": true, "flow": 60, "frost": 5,
 *     "max_flow_mode": "fixed", "max_flow": 45,       or "curve":
 *     "max_flow_curve": { "cold": -10, "cold_flow": 45,
 *                         "warm": 15, "warm_flow": 30 },
 *     "max_rate": 3, "heat_start": 30, "outside_entity": "sensor.outside",
 *     "onoff_period": 20,                       minutes
 *     "flow_ignored": false,     true: OpenTherm, but the heat source does
 *                                not follow the water temperature asked for
 *     "hot_water": { "setpoint": 55, "program_enabled": true,
 *                    "program": [ [ ["06:30", 60], ["22:00", 45] ], ... ],
 *                    "keep_warm": false } }
 *                                    the same seven days, in degrees
 * Without "hot_water" the boiler keeps its own hot water setting.
 * "keep_warm" false clears the hot water switch in the OpenTherm master
 * status: most combi boilers then stop keeping the tap water warm (and
 * still heat it when a tap opens). Without it: on. The original software's
 * "dhwEnabled" 0 becomes keep_warm false once, see take_vendor_dhw().
 * "flow" is the boiler water temperature with "modulation" off (OpenTherm):
 * fixed, while the room band only switches the heating on and off. Without
 * it, it follows the distance to the target (40 + 20 per degree).
 * The maximum boiler water temperature (OpenTherm, up to 80, for example for
 * floor heating without its own pump) is the boiler's own, a fixed one, or
 * one by the outside temperature: a straight line between the two points of
 * the curve, the end value beyond them, and the cold value when no outside
 * temperature is known. Below the boiler's own maximum it is what the rule,
 * pre-heating and what is learned all use. "max_rate" is the fastest
 * warming aimed for, in degrees an hour (standard 3). "heat_start" (20-40,
 * standard 30) is the flow above which heat is asked for (OpenTherm with
 * the rule): lower for floor heating or a heat pump. A maximum is never
 * less than 2 degrees above it, or the heating could never start.
 *
 * Cooling, for an OpenTherm boiler that says it can (id 3):
 *     "presets_cool": { "comfort": 23, "home": 24, "sleep": 25, "away": 28 },
 *     "cooling": { "enabled": true, "mode": "auto",     heat, cool or auto
 *                  "outside_entity": "sensor.outside",  if OpenTherm has none
 *                  "cool_above": 24, "heat_below": 18 }
 * The program and the presets then use the cooling temperatures. Automatic:
 * half an hour on the other side of the limit switches over; without an
 * outside temperature it stays as it is.
 *
 * What the user last chose and what was learned about the house is kept in
 * thermostat.state next to it, so a restart changes nothing.
 *
 * How it behaves:
 *  - A preset or a temperature set by hand holds until the next step of the
 *    program; without a program it simply stays.
 *  - Frost protection works with the thermostat off.
 *
 * The control rule works once a minute with two numbers it learns about the
 * house:
 *    loss      degrees a minute the room loses with the boiler idle (< 0)
 *    factor    degrees a minute the room gains per degree that the boiler
 *              water is above the room
 *  - Flow temperature:
 *       flow = T - loss / factor + (T - room - offset) * RATE / (60 * factor)
 *    T is the target, RATE the fastest warming aimed for (3 degrees an
 *    hour), limited to 6 .. the maximum. Heat is asked for when this flow
 *    is above the start point ("heat_start", standard 30), until it is
 *    below it by what 0.15 degree of room makes in the flow (at least 2):
 *    that, not a band around the room temperature, is the on/off decision
 *    (OpenTherm).
 *  - offset (-1.5 .. +0.5) follows, every four hours, how far the room
 *    stayed from the target while the boiler ran in its middle range (not
 *    while starting early or stopping for after-heat), and takes away the
 *    shortfall a proportional rule leaves, or a little of a surplus.
 *  - factor is measured over stretches at full flow temperature, loss over
 *    hours of rest.
 *  - With an outside temperature, the loss also follows it: a house loses
 *    heat in proportion to the difference with outside, loss = -k * (T -
 *    outside). k (per minute per degree) is measured in the same hours of
 *    rest when outside is at least K_MIN_DIFF colder; from three such
 *    measurements on it replaces the fixed loss wherever it is used (the
 *    flow, starting early, measuring factor). Without an outside
 *    temperature the fixed loss stays.
 *  - After-heat: after a burst of heat the room keeps warming from what is
 *    in the radiators. "stored" counts the heat put in (minutes at full
 *    temperature; a modulating boiler's minute by how far its water was
 *    towards the maximum) and lets it fade with STORE_TAU minutes, so it
 *    levels off in long heating and is still there in a short pause. After
 *    every stop the room (its one-minute average) is followed to its peak:
 *    the rise per unit of "stored" at the stop is "after". Only a clear peak
 *    counts (no heat before it, no timeout, at least AFTER_MIN_EQ stored).
 *    Median of the first three, then blended like the loss. From three on,
 *    heat stops when room + after * stored reaches the target plus half the
 *    swing of the shortest burst (at least LAG_ALLOW), after PULSE_MIN
 *    minutes; and does not start while even a PULSE_MIN burst on top of
 *    what is stored would pass it. A pulse stopped so is over for its
 *    period. A modulating boiler turns its flow down near the target by
 *    itself: there it only counts while the flow asked for is the maximum.
 *    It is learned per way of heating (flow, pulses, room band).
 *  - A stretch of at least AFTER_LONG minutes at full temperature that gave
 *    a factor measurement is measured again up to the peak, so the factor
 *    counts the after-heat too.
 *  - Starting early corrects itself: after each start the time the room
 *    reached the step's temperature is compared with the step (early:
 *    minutes before it; late: minutes after it, up to PH_LATE_MAX). Half
 *    of that error goes into "ph_extra", minutes added to the estimate
 *    (PH_EXTRA_MIN .. PH_EXTRA_MAX). A start that ends without its step
 *    (the program or target changed), or during which no heat could
 *    come (no boiler module answering), does not count.
 *  - Once, when nothing is learned yet, what the Toon's original software
 *    learned (its config_happ_thermstat.xml, still on the data partition) is
 *    the starting point: its measured factors and loss. Not its offset:
 *    that corrects its own room reading, which is not ours.
 *  - Pre-heating: before a step to a warmer preset, the minutes needed are
 *       (next - room - minutes_left * loss) / ((max - room) * factor) + 15
 *    and the target is raised when that reaches the minutes left.
 * A boiler on the on/off contact has no flow temperature. There the same
 * rule decides in pulses. A counter runs through "onoff_period" minutes
 * (10-30, standard 20); every minute the minutes on are worked out again,
 *       period * (flow - room) / (max - room)
 * and the contact is closed while the counter is below that. Near the
 * target that is a few minutes at the start of the period, so the radiators
 * are not filled with hot water that keeps warming the room after the target
 * is reached; when the room cools more than expected, or the target goes up,
 * it comes on again later in the same period. A pulse lasts at least
 * PULSE_MIN minutes, a pause shorter than PULSE_GAP is not made, and there is
 * no heat when the room is OVER_STOP above the target. The boiler counts as
 * "full" while on.
 *
 * Some heat sources talk OpenTherm but do not follow the water temperature
 * asked for: a district heating set mostly delivers what the network gives.
 * The rule above would ask for lukewarm water near the target and keep the
 * heat on for long, while hot water arrives, and the room overshoots. With
 * "flow_ignored" the same pulses as for the on/off contact decide over
 * OpenTherm, the maximum is asked for while on, and what is learned counts
 * the source as full while on: as with the contact, it learns the house as
 * it is heated. The other OpenTherm values (hot water, faults) stay.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <cjson/cJSON.h>
#include "thermostat.h"

/* p4 is read-only in normal use; the host test (test/) writes to a folder */
#ifdef TH_TEST
#define data_rw()	0
#define data_ro()	0
#else
#define data_rw()	system("mount -o remount,rw /mnt/data 2>/dev/null")
#define data_ro()	system("sync; mount -o remount,ro /mnt/data 2>/dev/null")
#endif

#ifndef CONF_FILE
#define CONF_FILE	"/mnt/data/toon-os/config/thermostat.json"
#endif
#ifndef SAVE_FILE
#define SAVE_FILE	"/mnt/data/toon-os/config/thermostat.state"
#endif
#ifndef VENDOR_FILE
#define VENDOR_FILE	"/mnt/data/qmf/config/config_happ_thermstat.xml"
#endif
#define K_MIN_DIFF	3.0	/* degrees colder outside before k is measured */
#define K_MAX		0.01	/* per minute per degree: an open tent */
#define AFTER_WATCH	90	/* minutes a stop is followed to the room's peak */
#define STORE_TAU	20.0	/* minutes: how fast the heat in the radiators fades */
#define AFTER_MIN_EQ	2.0	/* stored at a stop, at least, for a measurement */
#define OFFSET_MAX	0.5	/* the offset may also lower the room this much */
#define PH_LATE_MAX	120	/* minutes late, at most, counted for a start */
#define PH_EXTRA_MIN	-60	/* minutes the learned correction of starting early */
#define PH_EXTRA_MAX	120	/* ... may take off or add */
#define AFTER_MAX	1.0	/* degrees per full minute: no house is above this */
#define LAG_ALLOW	0.2	/* degrees above the target the peak may reach */
#define AFTER_LONG	20	/* minutes: a stretch long enough to re-measure factor at the peak */
#define FULL_MIN	8	/* minutes at full temperature, at least, for a factor measurement */

#define WEEK		(7 * 1440)
#define MAX_STEPS	(7 * 12)
#define OUT_MIN		6.0	/* flow asked for when nothing is needed */
#define OUT_HEAT	30.0	/* standard: above this flow, heat is asked for */
#define RATE_STD	3.0	/* degrees an hour: the fastest warming aimed for */
#define FACTOR_START	0.001
#define LOSS_START	-0.003
#define LOSS_MIN	-0.14
#define LOSS_MAX	-0.0001
#define OFFSET_MIN	-1.5
#define OFFSET_EVERY	240	/* minutes */
#define HIST		60	/* minutes of history */
#define N_MEAS		8
#define AUTO_MINUTES	30	/* past the limit this long: heat <-> cool */
#define PERIOD_STD	20	/* on/off contact: minutes per pulse period */
#define PULSE_MIN	2	/* minutes: the shortest pulse */
#define PULSE_GAP	3	/* minutes: the shortest pause (toon-boilerd waits 3) */
#define OVER_STOP	0.5	/* degrees above the target: no pulse at all */

static const char *const preset_name[N_PRESETS] = { "comfort", "home", "sleep", "away" };

struct th th = {
	.target = 18.0, .preset = -1, .out = OUT_MIN,
	.factor = FACTOR_START, .loss = LOSS_START,
};

/* settings */
static double preset_temp[N_PRESETS] = { 20.0, 18.0, 15.0, 12.0 };
static int program_on, preheat_on = 1, max_preheat = 180, modulation = 1;
static double frost = 5.0;
static double fixed_flow;		/* 0 = from the distance to the target */
static int flow_ignored;		/* OpenTherm, but the flow asked for is not followed */
static double max_flow;			/* 0 = the boiler's own maximum */
enum { MAXF_BOILER, MAXF_FIXED, MAXF_CURVE };
static int max_mode;
static double cv_cold = -10, cv_cold_flow = 45, cv_warm = 15, cv_warm_flow = 30;
static double max_now;			/* the user's maximum this minute, 0 = none */
static double max_rate = RATE_STD;
static double heat_start = OUT_HEAT;
static int onoff_period = PERIOD_STD;
static struct { int at, preset; } steps[MAX_STEPS];	/* at: minute of the week */
static int nsteps;
static double preset_cool[N_PRESETS] = { 23.0, 24.0, 25.0, 28.0 };
static int cool_mode;			/* 0 heat, 1 cool, 2 automatic */
static double cool_above = 24.0, heat_below = 18.0;
static int auto_season, auto_minutes;
static double hw_setpoint;		/* 0 = leave it to the boiler */
static int hw_keep_warm = 1;		/* the hot water switch in id 0 */
static int hw_keep_set;			/* "keep_warm" is in the settings */
static int hw_program_on;
static struct { int at; double temp; } hw_steps[MAX_STEPS];
static int hw_nsteps;
static long long conf_mtime;		/* the settings file: time to the ns, and size */

static long long conf_stamp(void)
{
	struct stat st;

	return stat(CONF_FILE, &st) ? 0 :
	       st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec + st.st_size;
}

/* state */
static time_t hold_until;	/* a choice by hand ends here; 0 = no hold */
static int hold;		/* a choice by hand is in force: 1 until the program's
				   next step, 2 for good (locked), also with the program */
static int prog_preset = -1;	/* the preset the program has now, -1 none */
static int last_step = -1;

/*
 * The change log (setting "log_changes", off by default): a line for each
 * change of the thermostat's on/off, target, preset, hold, week program or
 * pre-heating, with where it came from: screen, web, ha (Home Assistant over
 * MQTT), app (the Toon's own MQTT bus), program, settings or start. On p4,
 * so it stays over a restart; the newest LOG_MAX bytes and one older file.
 */
#ifndef LOG_FILE
#define LOG_FILE	"/mnt/data/toon-os/thermostat-changes.log"
#endif
#define LOG_MAX		65536
static int log_on;
static const char *cmd_source;	/* who sent the command being handled */
static struct { int known, hvac, preset, hold, program, preheat; double target; } logged;

void th_source(const char *src)
{
	cmd_source = src;
}

/*
 * The measurement log (setting "log_measurements", off by default): a CSV
 * line a minute with what the thermostat saw, asked for and learned, and
 * what the boiler said, for looking at how it works in a real house (Excel,
 * or sent to the developers). Kept in memory and written every
 * MEASURE_FLUSH seconds, so the flash is not written every minute; the
 * newest MEASURE_MAX bytes (lines of about 120 bytes: almost two days)
 * and one older file.
 */
#ifndef MEASURE_FILE
#define MEASURE_FILE	"/mnt/data/toon-os/thermostat-measurements.csv"
#endif
#define MEASURE_MAX	300000
#define MEASURE_FLUSH	600
static int measure_on;
static char mbuf[8192];
static size_t mlen;
static time_t m_flushed;

static void measure_flush(void)
{
	struct stat st;
	FILE *f;

	if (!mlen || data_rw())
		return;
	if (!stat(MEASURE_FILE, &st) && st.st_size > MEASURE_MAX)
		rename(MEASURE_FILE, MEASURE_FILE ".1");
	f = fopen(MEASURE_FILE, "a");
	if (f) {
		if (!ftell(f))
			fputs("time,room,target,working_target,preheating,flow_asked,heat_wanted,"
			      "heating,flame,flow_measured,return_measured,modulation,outside,"
			      "pulse_minutes,stop_for_afterheat,way,heat_rate_c_h,factor,loss,"
			      "k,afterheat,preheat_extra_min,offset\n", f);
		fwrite(mbuf, 1, mlen, f);
		fclose(f);
	}
	if (data_ro())
		fprintf(stderr, "toon-boilerd: the data partition stays writable\n");
	mlen = 0;
	m_flushed = time(NULL);
}

/* a number, or nothing when it is not known */
static int csv_num(char *p, size_t n, double v, int prec, int known)
{
	return known && isfinite(v) ? snprintf(p, n, ",%.*f", prec, v) : snprintf(p, n, ",");
}

static void log_write(const char *line)
{
	struct stat st;
	char ts[32];
	time_t now = time(NULL);
	struct tm tm;
	FILE *f;

	if (data_rw())
		return;
	if (!stat(LOG_FILE, &st) && st.st_size > LOG_MAX)
		rename(LOG_FILE, LOG_FILE ".1");
	f = fopen(LOG_FILE, "a");
	if (f) {
		localtime_r(&now, &tm);
		strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
		fprintf(f, "%s %s\n", ts, line);
		fclose(f);
	}
	if (data_ro())
		return;
}

/* what changed since the last look, as one line */
static void log_check(const char *src)
{
	char line[320], *p = line, *end = line + sizeof(line);
	const char *pname = th.preset < 0 ? "manual" : preset_name[th.preset];

	if (logged.known && logged.hvac == th.hvac && fabs(logged.target - th.target) < 0.05 &&
	    logged.preset == th.preset && logged.hold == hold && logged.program == program_on &&
	    logged.preheat == th.preheating)
		return;
	p += snprintf(p, end - p, "%s:", src);
	if (!logged.known) {
		p += snprintf(p, end - p, " %s, target %.1f, preset %s, hold %d, week program %s",
			      th.hvac ? "on" : "off", th.target, pname, hold, program_on ? "on" : "off");
	} else {
		if (logged.hvac != th.hvac)
			p += snprintf(p, end - p, " thermostat %s,", th.hvac ? "on" : "off");
		if (fabs(logged.target - th.target) >= 0.05)
			p += snprintf(p, end - p, " target %.1f -> %.1f,", logged.target, th.target);
		if (logged.preset != th.preset)
			p += snprintf(p, end - p, " preset %s -> %s,",
				      logged.preset < 0 ? "manual" : preset_name[logged.preset], pname);
		/* the week program switched off ends a hold: no news */
		if (logged.hold != hold && (program_on || logged.program == program_on))
			p += snprintf(p, end - p, " %s,", hold == 2 ? "locked" :
				      hold ? "temporary" : "follows the week program");
		if (logged.program != program_on)
			p += snprintf(p, end - p, " week program %s,", program_on ? "on" : "off");
		if (logged.preheat != th.preheating)
			p += snprintf(p, end - p, " pre-heating %s,", th.preheating ? "starts" : "ends");
		if (p > line && p[-1] == ',')
			*--p = 0;
	}
	logged.known = 1;
	logged.hvac = th.hvac;
	logged.target = th.target;
	logged.preset = th.preset;
	logged.hold = hold;
	logged.program = program_on;
	logged.preheat = th.preheating;
	if (log_on)
		log_write(line);
}
static time_t save_at, rates_saved;

/* one sample a minute, newest first */
static double h_room[HIST], h_out[HIST];
static double h_flow[HIST];		/* the water that minute: measured (id 25) or asked */
static int h_n;
static int full_minutes;		/* minutes in a row at full flow (or a steady one) */
static double full_sum;			/* ... their flow added up */
static double meas_acc;			/* the boiler's measured flow (id 25) this minute */
static int meas_n;
static int rest_minutes;		/* minutes in a row at the minimum */
static double meas[N_MEAS];		/* measured factors, newest first */
static int n_meas;
static double loss3[3];			/* the last three measured losses */
static int n_loss;
static double kloss, k3[3];		/* loss per degree to outside, the last three */
static int n_k, n_k3;			/* k measured so often; in k3 */
static int from_vendor;			/* 1: started from the original software's values */
static int own_offset;			/* saved by a version that never takes its offset */
static double after, a3[3];		/* after-heat per unit stored (this mode); the last three */
static int n_after, n_a3, after_mode;	/* after_mode: 1 flow, 2 pulses, 3 room band */
static double after_m[4], a3_m[4][3];	/* kept per mode */
static int n_after_m[4], n_a3_m[4];
static double stored;			/* heat in the radiators, fading (see above) */
static double burst_room0;		/* the room at the start of the burst now */
static int burst_min;			/* minutes of the burst now */
static int watch = -1;			/* minutes since a stop being followed, -1 none */
static double w_stored, w_room0, w_stop, w_peak;	/* that stop */
static int w_min, w_peak_t;
static double room_acc, room_m;		/* the room over this minute; the last minute's */
static int room_n;
static time_t heat_since;		/* this burst of heat started then */
static int pulse_cut;			/* the pulse of this period was stopped early */
static double pulse_done;		/* seconds of heat delivered in this period */
static double pulse_heat;		/* ... in the pulse now */
static int flow_on;			/* flow rule: heat asked for (with hysteresis) */
static int sensor_changed;		/* the room comes from another sensor now */

void th_sensor_changed(void)
{
	sensor_changed = 1;
}
static double ph_extra;			/* minutes added to starting early (learned) */
static int ph_n;			/* starts it was learned from */
static double ph_want;			/* the start now: the step's temperature */
static time_t ph_step_at, ph_reached;	/* ... its step, when the room got there */
static time_t late_since;		/* a start that was late: its step (0 none) */
static double late_want;
static int pend_n = -1;			/* the burst gave meas[0] over this many minutes */
static double heat_acc;			/* full minutes of heat since the last minute */
static time_t heat_tick;		/* the tick that counted last */
static double pend_max;
static int now_out_ok;			/* this tick's outside temperature */
static double now_outside;

static double clampd(double v, double lo, double hi);

/* the loss at room temperature t: by outside once k is known */
static double loss_at(double t)
{
	if (n_k >= 3 && now_out_ok)
		return clampd(-kloss * (t - now_outside), LOSS_MIN, LOSS_MAX);
	return th.loss;
}
static double off_sum;
static int off_n, minutes;
static int pulse_at = -1;		/* minute in the period, -1 = not running */
static int pulse_len;			/* minutes on, as worked out now */

static int find_preset(const char *name)
{
	for (int i = 0; i < N_PRESETS; i++)
		if (name && !strcmp(name, preset_name[i]))
			return i;
	return -1;
}

/* NaN (a broken file or sensor) ends up at the low end, never passes */
static double clampd(double v, double lo, double hi)
{
	return !(v >= lo) ? lo : v > hi ? hi : v;
}

/* ---- files ---------------------------------------------------------------- */

/* a preset's temperature on the side the thermostat is on */
static double preset_value(int p)
{
	return th.season ? preset_cool[p] : preset_temp[p];
}

static void load_cooling(cJSON *doc)
{
	static const double std_cool[N_PRESETS] = { 23.0, 24.0, 25.0, 28.0 };
	cJSON *o = cJSON_GetObjectItemCaseSensitive(doc, "presets_cool");
	const char *m;

	memcpy(preset_cool, std_cool, sizeof(preset_cool));
	for (int i = 0; i < N_PRESETS; i++) {
		cJSON *v = cJSON_GetObjectItemCaseSensitive(o, preset_name[i]);

		if (cJSON_IsNumber(v))
			preset_cool[i] = clampd(v->valuedouble, TH_MIN, TH_MAX);
	}
	o = cJSON_GetObjectItemCaseSensitive(doc, "cooling");
	th.cool_wanted = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "enabled"));
	m = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "mode"));
	cool_mode = m && !strcmp(m, "cool") ? 1 : m && !strcmp(m, "auto") ? 2 : 0;
	cool_above = 24.0;
	heat_below = 18.0;
	if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(o, "cool_above")))
		cool_above = cJSON_GetObjectItemCaseSensitive(o, "cool_above")->valuedouble;
	if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(o, "heat_below")))
		heat_below = cJSON_GetObjectItemCaseSensitive(o, "heat_below")->valuedouble;
}

static void load_hot_water(cJSON *o)
{
	cJSON *v = cJSON_GetObjectItemCaseSensitive(o, "setpoint"), *day, *step;
	int d = 0;

	if (cJSON_IsNumber(v) && v->valuedouble > 0)
		hw_setpoint = clampd(v->valuedouble, HW_MIN, HW_MAX);
	hw_program_on = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "program_enabled"));
	v = cJSON_GetObjectItemCaseSensitive(o, "keep_warm");
	if (cJSON_IsBool(v)) {
		hw_keep_warm = cJSON_IsTrue(v);
		hw_keep_set = 1;
	}
	cJSON_ArrayForEach(day, cJSON_GetObjectItemCaseSensitive(o, "program")) {
		cJSON_ArrayForEach(step, day) {
			const char *t = cJSON_GetStringValue(cJSON_GetArrayItem(step, 0));
			cJSON *temp = cJSON_GetArrayItem(step, 1);
			int h, m;

			if (!t || !cJSON_IsNumber(temp) || sscanf(t, "%d:%d", &h, &m) != 2 ||
			    h < 0 || h > 23 || m < 0 || m > 59 || hw_nsteps == MAX_STEPS || d > 6)
				continue;
			hw_steps[hw_nsteps].at = d * 1440 + h * 60 + m;
			hw_steps[hw_nsteps++].temp = clampd(temp->valuedouble, HW_MIN, HW_MAX);
		}
		d++;
	}
	for (int i = 1; i < hw_nsteps; i++)
		for (int j = i; j > 0 && hw_steps[j].at < hw_steps[j - 1].at; j--) {
			typeof(hw_steps[0]) tmp = hw_steps[j];

			hw_steps[j] = hw_steps[j - 1];
			hw_steps[j - 1] = tmp;
		}
}

static void take_vendor_dhw(void);

static void load_conf(void)
{
	cJSON *doc, *o, *day, *step;
	char *buf;
	FILE *f;
	long n;
	int d = 0;

	static const double std_temp[N_PRESETS] = { 20.0, 18.0, 15.0, 12.0 };

	/* without a file, or for what a file leaves out: the standard values */
	memcpy(preset_temp, std_temp, sizeof(preset_temp));
	program_on = nsteps = 0;
	preheat_on = modulation = 1;
	max_preheat = 180;
	frost = 5.0;
	fixed_flow = 0;
	flow_ignored = 0;
	max_flow = 0;
	max_mode = MAXF_BOILER;
	cv_cold = -10, cv_cold_flow = 45, cv_warm = 15, cv_warm_flow = 30;
	max_rate = RATE_STD;
	heat_start = OUT_HEAT;
	onoff_period = PERIOD_STD;
	hw_setpoint = 0;
	hw_program_on = hw_nsteps = 0;
	hw_keep_warm = 1;
	hw_keep_set = 0;
	th.cool_wanted = 0;
	th.external = 0;
	cool_mode = 0;
	last_step = -1;
	conf_mtime = conf_stamp();
	f = fopen(CONF_FILE, "r");
	if (!f)
		return;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	rewind(f);
	/* a settings file is a few kB: anything odd is read as no settings */
	buf = n >= 0 && n < (1 << 20) ? calloc(1, n + 1) : NULL;
	if (buf && fread(buf, 1, n, f) != (size_t)n)
		buf[0] = 0;
	fclose(f);
	doc = buf ? cJSON_Parse(buf) : NULL;
	free(buf);
	if (!cJSON_IsObject(doc)) {
		cJSON_Delete(doc);
		return;
	}
	o = cJSON_GetObjectItemCaseSensitive(doc, "presets");
	for (int i = 0; i < N_PRESETS; i++) {
		cJSON *v = cJSON_GetObjectItemCaseSensitive(o, preset_name[i]);

		if (cJSON_IsNumber(v))
			preset_temp[i] = clampd(v->valuedouble, TH_MIN, TH_MAX);
	}
	program_on = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(doc, "program_enabled"));
	th.external = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(doc, "external"));
	log_on = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(doc, "log_changes"));
	measure_on = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(doc, "log_measurements"));
	o = cJSON_GetObjectItemCaseSensitive(doc, "preheat");
	if (cJSON_IsBool(o))
		preheat_on = cJSON_IsTrue(o);
	o = cJSON_GetObjectItemCaseSensitive(doc, "modulation");
	if (cJSON_IsBool(o))
		modulation = cJSON_IsTrue(o);
	o = cJSON_GetObjectItemCaseSensitive(doc, "flow_ignored");
	if (cJSON_IsBool(o))
		flow_ignored = cJSON_IsTrue(o);
	o = cJSON_GetObjectItemCaseSensitive(doc, "flow");
	if (cJSON_IsNumber(o) && o->valuedouble > 0)
		fixed_flow = clampd(o->valuedouble, 20.0, 80.0);
	o = cJSON_GetObjectItemCaseSensitive(doc, "max_flow");
	if (cJSON_IsNumber(o) && o->valuedouble > 0)
		max_flow = clampd(o->valuedouble, 20.0, 80.0);
	o = cJSON_GetObjectItemCaseSensitive(doc, "max_flow_mode");
	if (cJSON_IsString(o))
		max_mode = !strcmp(o->valuestring, "curve") ? MAXF_CURVE :
			   !strcmp(o->valuestring, "fixed") && max_flow > 0 ? MAXF_FIXED : MAXF_BOILER;
	else if (max_flow > 0)
		max_mode = MAXF_FIXED;
	o = cJSON_GetObjectItemCaseSensitive(doc, "max_flow_curve");
	if (cJSON_IsObject(o)) {
		cJSON *v;

		if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(o, "cold")))
			cv_cold = clampd(v->valuedouble, -30, 30);
		if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(o, "warm")))
			cv_warm = clampd(v->valuedouble, -30, 30);
		if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(o, "cold_flow")))
			cv_cold_flow = clampd(v->valuedouble, 20, 80);
		if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(o, "warm_flow")))
			cv_warm_flow = clampd(v->valuedouble, 20, 80);
	}
	o = cJSON_GetObjectItemCaseSensitive(doc, "heat_start");
	if (cJSON_IsNumber(o) && o->valuedouble > 0)
		heat_start = clampd(o->valuedouble, 20.0, 40.0);
	o = cJSON_GetObjectItemCaseSensitive(doc, "onoff_period");
	if (cJSON_IsNumber(o) && o->valuedouble > 0)
		onoff_period = clampd(o->valuedouble, 10, 30);
	o = cJSON_GetObjectItemCaseSensitive(doc, "max_rate");
	if (cJSON_IsNumber(o) && o->valuedouble > 0)
		max_rate = clampd(o->valuedouble, 0.5, 10.0);
	o = cJSON_GetObjectItemCaseSensitive(doc, "max_preheat");
	if (cJSON_IsNumber(o))
		max_preheat = clampd(o->valuedouble, 0, 360);
	o = cJSON_GetObjectItemCaseSensitive(doc, "frost");
	if (cJSON_IsNumber(o))
		frost = clampd(o->valuedouble, 3, 12);
	load_hot_water(cJSON_GetObjectItemCaseSensitive(doc, "hot_water"));
	load_cooling(doc);

	nsteps = 0;
	cJSON_ArrayForEach(day, cJSON_GetObjectItemCaseSensitive(doc, "program")) {
		cJSON_ArrayForEach(step, day) {
			const char *t = cJSON_GetStringValue(cJSON_GetArrayItem(step, 0));
			int p = find_preset(cJSON_GetStringValue(cJSON_GetArrayItem(step, 1)));
			int h, m;

			if (!t || p < 0 || sscanf(t, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 ||
			    m < 0 || m > 59 || nsteps == MAX_STEPS || d > 6)
				continue;
			steps[nsteps].at = d * 1440 + h * 60 + m;
			steps[nsteps++].preset = p;
		}
		d++;
	}
	/* in order of time; the web interface sends them so, but be sure */
	for (int i = 1; i < nsteps; i++)
		for (int j = i; j > 0 && steps[j].at < steps[j - 1].at; j--) {
			typeof(steps[0]) tmp = steps[j];

			steps[j] = steps[j - 1];
			steps[j - 1] = tmp;
		}
	if (!nsteps)
		program_on = 0;
	last_step = -1;
	cJSON_Delete(doc);
}

static void load_saved(void)
{
	char line[96];
	FILE *f = fopen(SAVE_FILE, "r");

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *eq, *end;

		line[strcspn(line, "\r\n")] = 0;
		/* a number that is not one (nan, inf) is left out */
		eq = strchr(line, '=');
		if (eq && (strtod(eq + 1, &end), end != eq + 1) && !isfinite(strtod(eq + 1, NULL)))
			continue;
		if (!strncmp(line, "hvac=", 5))
			th.hvac = !strcmp(line + 5, "heat");
		else if (!strncmp(line, "target=", 7))
			th.target = clampd(atof(line + 7), TH_MIN, TH_MAX);
		else if (!strncmp(line, "preset=", 7))
			th.preset = find_preset(line + 7);
		else if (!strncmp(line, "hold=", 5))
			hold = atoi(line + 5);
		else if (!strncmp(line, "season=", 7))
			auto_season = atoi(line + 7);
		else if (!strncmp(line, "hold_until=", 11))
			hold_until = atoll(line + 11);
		else if (!strncmp(line, "factor=", 7))
			th.factor = clampd(atof(line + 7), 0.0001, 0.05);
		else if (!strncmp(line, "loss=", 5))
			th.loss = clampd(atof(line + 5), LOSS_MIN, LOSS_MAX);
		else if (!strncmp(line, "offset=", 7))
			th.offset = clampd(atof(line + 7), OFFSET_MIN, OFFSET_MAX);
		else if (!strncmp(line, "kloss=", 6))
			kloss = clampd(atof(line + 6), 0, K_MAX);
		else if (!strncmp(line, "kloss_n=", 8))
			n_k = atoi(line + 8);
		else if (!strncmp(line, "from_vendor=", 12))
			from_vendor = atoi(line + 12);
		else if (!strncmp(line, "own_offset=", 11))
			own_offset = atoi(line + 11);
		else if (!strncmp(line, "after=", 6))
			after = clampd(atof(line + 6), 0, AFTER_MAX);
		else if (!strncmp(line, "after_n=", 8))
			n_after = atoi(line + 8);
		else if (!strncmp(line, "after_mode=", 11))
			after_mode = atoi(line + 11);
		else if (!strncmp(line, "after_mode", 10) && line[10] >= '1' && line[10] <= '3' &&
			 line[11] == '=') {	/* after_mode2=<after> <n> */
			int m = line[10] - '0';

			after_m[m] = clampd(atof(line + 12), 0, AFTER_MAX);
			n_after_m[m] = atoi(strchr(line + 12, ' ') ? strchr(line + 12, ' ') + 1 : "0");
		}
		else if (!strncmp(line, "preheat_extra=", 14))
			ph_extra = clampd(atof(line + 14), PH_EXTRA_MIN, PH_EXTRA_MAX);
		else if (!strncmp(line, "preheat_n=", 10))
			ph_n = atoi(line + 10);
		else if (!strncmp(line, "measured=", 9)) {
			char *p = line + 9;

			for (n_meas = 0; n_meas < N_MEAS && *p; n_meas++) {
				meas[n_meas] = strtod(p, &p);
				if (!isfinite(meas[n_meas]))
					meas[n_meas] = FACTOR_START;
				if (*p == ' ')
					p++;
			}
		}
	}
	fclose(f);
	/* before 1.75 the original software's offset was taken over too; it
	   belongs to its own room reading, not to ours: start that one at 0 */
	if (from_vendor && !own_offset) {
		th.offset = 0;
		save_at = time(NULL);
	}
	own_offset = 1;
}

/* p4 is read-only in normal use: open it for the moment of writing */
static void save(void)
{
	FILE *f;

	save_at = 0;
	if (data_rw())
		return;
	f = fopen(SAVE_FILE ".new", "w");
	if (f) {
		fprintf(f, "hvac=%s\ntarget=%.1f\npreset=%s\nhold=%d\nhold_until=%lld\n"
			"season=%d\nfactor=%.6f\nloss=%.6f\noffset=%.3f\nmeasured=",
			th.hvac ? "heat" : "off", th.target,
			th.preset < 0 ? "manual" : preset_name[th.preset], hold,
			(long long)hold_until, auto_season, th.factor, th.loss, th.offset);
		for (int i = 0; i < n_meas; i++)
			fprintf(f, "%s%.6f", i ? " " : "", meas[i]);
		fprintf(f, "\nkloss=%.7f\nkloss_n=%d\nfrom_vendor=%d\nafter=%.7f\nafter_n=%d\nafter_mode=%d\n"
			"preheat_extra=%.1f\npreheat_n=%d\nown_offset=1\n",
			kloss, n_k, from_vendor, after, n_after, after_mode, ph_extra, ph_n);
		for (int m = 1; m <= 3; m++)
			if (m != after_mode && n_after_m[m])
				fprintf(f, "after_mode%d=%.7f %d\n", m, after_m[m], n_after_m[m]);
		/* only a file that was written completely replaces the old one */
		if (fclose(f) || rename(SAVE_FILE ".new", SAVE_FILE))
			remove(SAVE_FILE ".new");
	}
	if (data_ro())
		return;
	rates_saved = time(NULL);
}

/* ---- the program ------------------------------------------------------------ */

/* the step in force at "now" (minute of the week) and the one after it */
static void find_steps(int now, int *active, int *next, int *minutes_to_next)
{
	int a = nsteps - 1, n = 0;

	for (int i = 0; i < nsteps; i++)
		if (steps[i].at <= now)
			a = i;
	n = (a + 1) % nsteps;
	if (steps[a].at > now)		/* before the first step of the week */
		n = 0;
	*active = a;
	*next = n;
	*minutes_to_next = (steps[n].at - now + WEEK) % WEEK;
	if (!*minutes_to_next)
		*minutes_to_next = WEEK;
}

/* a choice made by hand: until the next step, or for good without a program
   or when locked; a locked choice stays locked */
static void take_hold(int lock)
{
	hold = lock || hold == 2 ? 2 : 1;
	hold_until = 0;		/* set at the next tick, when the clock is looked at */
	last_step = -1;
	save_at = time(NULL) + 5;
}

static void resume_program(void)
{
	hold = 0;
	hold_until = 0;
	last_step = -1;
	save_at = time(NULL) + 5;
}

/* a setting changed from the screen or Home Assistant (the week program, the
   sensor): in the settings file the web interface writes too (p4 is opened
   for the moment of writing); item becomes the file's. With parent, the
   key is in that object of the file ("hot_water"), made when missing. */
static void save_setting_in(const char *parent, const char *key, cJSON *item)
{
	cJSON *obj;
	FILE *f = fopen(CONF_FILE, "r");
	cJSON *doc = NULL;
	char *txt = NULL, *out;
	long len;

	if (f) {
		fseek(f, 0, SEEK_END);
		len = ftell(f);
		rewind(f);
		txt = calloc(1, len + 1);
		if (txt && fread(txt, 1, len, f) == (size_t)len)
			doc = cJSON_Parse(txt);
		free(txt);
		fclose(f);
	}
	if (!doc) {
		cJSON_Delete(item);
		return;		/* no settings yet: the web interface makes them */
	}
	obj = doc;
	if (parent) {
		obj = cJSON_GetObjectItemCaseSensitive(doc, parent);
		if (!cJSON_IsObject(obj)) {
			cJSON_DeleteItemFromObjectCaseSensitive(doc, parent);
			obj = cJSON_AddObjectToObject(doc, parent);
		}
	}
	cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
	cJSON_AddItemToObject(obj, key, item);
	out = cJSON_Print(doc);
	cJSON_Delete(doc);
	if (!out || data_rw()) {
		free(out);
		return;
	}
	f = fopen(CONF_FILE ".new", "w");
	if (f) {
		struct stat st;

		fputs(out, f);
		fclose(f);
		rename(CONF_FILE ".new", CONF_FILE);
		/* our own change: not read again as new settings (the log) */
		if (!stat(CONF_FILE, &st))
			conf_mtime = conf_stamp();
	}
	free(out);
	if (data_ro())
		return;
}

static void save_setting(const char *key, cJSON *item)
{
	save_setting_in(NULL, key, item);
}

static int command(const char *line);

int th_command(const char *line)
{
	int r = command(line);

	if (r && logged.known)
		log_check(cmd_source ? cmd_source : "other");
	return r;
}

static int command(const char *line)
{
	if (!strcmp(line, "hvac heat") || !strcmp(line, "hvac off")) {
		th.hvac = line[5] == 'h';
		save_at = time(NULL) + 5;
		return 1;
	}
	if (!strncmp(line, "target ", 7)) {
		double t = atof(line + 7);

		/* locked: the temperature stays too, from wherever it is asked */
		if (hold == 2 && th.program)
			return 1;
		if (t >= TH_MIN && t <= TH_MAX) {
			th.target = t;
			th.preset = -1;
			take_hold(0);
		}
		return 1;
	}
	if (!strncmp(line, "preset ", 7)) {
		/* "preset <name>" or "preset <name> lock" */
		char name[32], *sp;
		int p, lock;

		snprintf(name, sizeof(name), "%s", line + 7);
		sp = strchr(name, ' ');
		lock = sp && !strcmp(sp + 1, "lock");
		if (sp)
			*sp = 0;
		p = find_preset(name);
		if (p < 0)
			return 1;
		/* back from another preset to what the program has now: follow
		   the program again. The program's own preset chosen again is a
		   choice by hand (held until the next step, or locked). */
		if (!lock && th.program && p == prog_preset && hold == 1 && th.preset != p) {
			resume_program();
			return 1;
		}
		th.preset = p;
		th.target = preset_value(p);
		take_hold(lock);
		return 1;
	}
	if (!strcmp(line, "hold lock")) {
		/* what is in force now stays, also over the program's steps */
		take_hold(1);
		return 1;
	}
	if (!strncmp(line, "sensor ", 7)) {
		/* "average", "toon" or one of the chosen sensors: toon-ui
		   checks it against the list when it reads the file */
		save_setting("sensor_use", cJSON_CreateString(line + 7));
		return 1;
	}
	if (!strncmp(line, "max_flow ", 9)) {
		/* "max_flow 45": a fixed maximum water temperature (20-80, from
		   Home Assistant's heating curve for example); "max_flow boiler"
		   or 0: the boiler's own again. Kept in the settings, written
		   only when it changes. */
		double t = atof(line + 9);

		if (!strcmp(line + 9, "boiler") || (t == 0 && line[9] == '0')) {
			if (max_mode != MAXF_BOILER) {
				max_mode = MAXF_BOILER;
				save_setting("max_flow_mode", cJSON_CreateString("boiler"));
			}
		} else if (t >= 20 && t <= 80) {
			t = round(t);
			if (max_mode != MAXF_FIXED || max_flow != t) {
				max_flow = t;
				max_mode = MAXF_FIXED;
				save_setting("max_flow", cJSON_CreateNumber(t));
				save_setting("max_flow_mode", cJSON_CreateString("fixed"));
			}
		}
		return 1;
	}
	if (!strncmp(line, "hot_water ", 10)) {
		/* "hot_water 60": the hot water temperature (HW_MIN-HW_MAX, from
		   Home Assistant, by the outside temperature for example). It
		   stays, so the hot water week program goes off, as Fixed does
		   for max_flow. Kept in the settings, written only when it
		   changes; the boiler gets it within its own limits. */
		double t = round(atof(line + 10));

		if (t >= HW_MIN && t <= HW_MAX && (hw_setpoint != t || hw_program_on)) {
			hw_setpoint = t;
			save_setting_in("hot_water", "setpoint", cJSON_CreateNumber(t));
			if (hw_program_on) {
				hw_program_on = 0;
				save_setting_in("hot_water", "program_enabled", cJSON_CreateFalse());
			}
		}
		return 1;
	}
	if (!strcmp(line, "hot_water_keep_warm on") || !strcmp(line, "hot_water_keep_warm off")) {
		/* keep the tap water warm (the hot water switch in OpenTherm's
		   master status), from Home Assistant; kept in the settings */
		hw_keep_warm = line[strlen(line) - 1] == 'n';
		hw_keep_set = 1;
		save_setting_in("hot_water", "keep_warm", cJSON_CreateBool(hw_keep_warm));
		return 1;
	}
	if (!strcmp(line, "program resume")) {
		resume_program();
		return 1;
	}
	if (!strcmp(line, "program on") || !strcmp(line, "program off")) {
		program_on = line[9] == 'n';
		save_setting("program_enabled", cJSON_CreateBool(program_on));
		resume_program();	/* switched on: follow it at once */
		return 1;
	}
	return 0;
}

/* ---- once a minute: history, learning, the offset ------------------------------ */

static int cmp_double(const void *a, const void *b)
{
	double d = *(const double *)a - *(const double *)b;

	return d < 0 ? -1 : d > 0;
}

/* the factor in use: the start value until three stretches were measured */
static void choose_factor(void)
{
	double v[N_MEAS], sum = 0;
	int lo = 0, hi = n_meas;

	if (n_meas < 3) {
		th.factor = FACTOR_START;
		return;
	}
	memcpy(v, meas, sizeof(double) * n_meas);
	qsort(v, n_meas, sizeof(double), cmp_double);
	if (n_meas >= 5) {		/* without the lowest and the highest */
		lo = 1;
		hi = n_meas - 1;
	}
	for (int i = lo; i < hi; i++)
		sum += v[i];
	th.factor = clampd(sum / (hi - lo), 0.0001, 0.05);
}

/* out: the boiler's output this minute; asked: what the rule asks for */
static void every_minute(time_t now, double room, double out, double asked, double max)
{
	double avg = 0, minute_eq = heat_acc;

	heat_acc = 0;

	memmove(h_room + 1, h_room, sizeof(h_room) - sizeof(h_room[0]));
	memmove(h_out + 1, h_out, sizeof(h_out) - sizeof(h_out[0]));
	memmove(h_flow + 1, h_flow, sizeof(h_flow) - sizeof(h_flow[0]));
	h_room[0] = room;
	h_out[0] = out;
	h_flow[0] = meas_n ? meas_acc / meas_n : out;
	if (h_n < HIST)
		h_n++;
	minutes++;
	for (int i = 0; i < h_n; i++)
		avg += h_out[i] / h_n;

	/* the factor: a stretch at full flow that has just ended; without the
	   flow rule (pulses, the room band) every heating minute has the same
	   flow, so any stretch of heat counts, with that flow */
	if (th.by_flow ? out >= max - 1 : out > OUT_MIN + 0.01) {
		full_minutes++;
		full_sum += h_flow[0];	/* the whole stretch, for measuring at the peak */
	} else if (full_minutes >= FULL_MIN) {
		double fo;
		/* radiators first have to warm up themselves: a long stretch
		   leaves its first ten minutes out */
		int n = full_minutes > 15 ? full_minutes - 10 : full_minutes;

		if (n > h_n - 1)
			n = h_n - 1;
		/* the water over exactly the minutes the rise is taken over (the
		   room went from h_room[n] to h_room[0] with the heat of 1..n) */
		fo = 0;
		for (int i = 1; i <= n; i++)
			fo += h_flow[i] / n;
		if (n >= 5) {
			double t0 = h_room[n], t1 = h_room[0];
			double f = fo - (t0 + t1) / 2 < 5 ? 0 :
				   ((t1 - t0) - n * loss_at((t0 + t1) / 2)) / (fo - (t0 + t1) / 2) / n;

			if (f >= 0.0001 && f <= 0.05) {
				memmove(meas + 1, meas, sizeof(meas) - sizeof(meas[0]));
				meas[0] = f;
				if (n_meas < N_MEAS)
					n_meas++;
				choose_factor();
				save_at = now;
				/* measured again at the peak, if this was the whole burst */
				pend_n = full_minutes < h_n ? full_minutes : -1;
				pend_max = full_sum / full_minutes;
			}
		}
		full_minutes = 0;
		full_sum = 0;
	} else {
		full_minutes = 0;
		full_sum = 0;
	}

	/* after-heat: the heat in the radiators, and after a stop the room
	   followed to its peak (the heat counted to the second in th_tick) */
	stored = stored * exp(-1.0 / STORE_TAU) + minute_eq;
	if (minute_eq > 0.01) {
		if (!burst_min)
			burst_room0 = h_n > 1 ? h_room[1] : room;
		burst_min++;
		if (watch >= 0)
			pend_n = -1;	/* heat again before the peak: not this one */
		watch = -1;
	} else if (burst_min) {
		watch = 0;
		w_stored = stored;
		w_min = burst_min;
		w_room0 = burst_room0;
		w_stop = w_peak = room;
		w_peak_t = 0;
		burst_min = 0;
	} else if (watch >= 0) {
		watch++;
		if (room > w_peak) {
			w_peak = room;
			w_peak_t = watch;
		}
		if (room < w_peak - 0.15) {	/* a clear peak */
			if (w_stored >= AFTER_MIN_EQ) {
				double a = clampd((w_peak - w_stop) / w_stored, 0, AFTER_MAX), v[3];

				memmove(a3 + 1, a3, sizeof(a3) - sizeof(a3[0]));
				a3[0] = a;
				if (n_a3 < 3)
					n_a3++;
				memcpy(v, a3, sizeof(v));
				qsort(v, n_a3, sizeof(double), cmp_double);
				/* the median until there are three, then blended */
				after = n_after >= 3 ? 0.7 * after + 0.3 * v[n_a3 / 2] : v[n_a3 / 2];
				n_after++;
				save_at = now;
			}
			/* the factor of a long stretch at full temperature, up to the
			   peak (a short burst into cold radiators takes far more
			   per minute than steady heating: those are left as they are) */
			if (pend_n == w_min && w_min >= AFTER_LONG && n_meas) {
				int m = w_min + w_peak_t;
				double mid = (w_room0 + w_peak) / 2;
				double f = ((w_peak - w_room0) - m * loss_at(mid)) / (pend_max - mid) / w_min;

				if (f >= 0.0001 && f <= 0.05) {
					meas[0] = f;
					choose_factor();
				}
			}
			pend_n = -1;
			watch = -1;
		} else if (watch >= AFTER_WATCH) {
			pend_n = -1;	/* no clear peak (sun, others): not counted */
			watch = -1;
		}
	}

	/* the loss: after an hour of rest, and again each further hour */
	rest_minutes = h_n == HIST && avg <= OUT_MIN + 0.01 ? rest_minutes + 1 : 0;
	/* from 90 minutes after the last heat (the radiators are cold by then:
	   the 44 minutes start 45 after it), only a room that cools (sun or
	   other warmth would teach it that the house loses nothing) */
	double l_raw = h_n == HIST ? (h_room[1] - h_room[45]) / 44 : 0;

	if (rest_minutes && rest_minutes % 60 == 31 && watch < 0 && !th.frost_active &&
	    l_raw < LOSS_MAX) {
		double l = clampd(l_raw, LOSS_MIN, LOSS_MAX), v[3];

		memmove(loss3 + 1, loss3, sizeof(loss3) - sizeof(loss3[0]));
		loss3[0] = l;
		if (n_loss < 3)
			n_loss++;
		memcpy(v, loss3, sizeof(v));
		qsort(v, n_loss, sizeof(double), cmp_double);
		th.loss = clampd(0.7 * th.loss + 0.3 * v[n_loss / 2], LOSS_MIN, LOSS_MAX);

		/* and per degree to outside, when that is known and colder */
		double diff = (h_room[1] + h_room[45]) / 2 - now_outside;

		if (now_out_ok && diff >= K_MIN_DIFF) {
			double k = clampd(-l / diff, 0, K_MAX), w[3];

			memmove(k3 + 1, k3, sizeof(k3) - sizeof(k3[0]));
			k3[0] = k;
			if (n_k3 < 3)
				n_k3++;
			memcpy(w, k3, sizeof(w));
			qsort(w, n_k3, sizeof(double), cmp_double);
			kloss = n_k ? 0.7 * kloss + 0.3 * w[n_k3 / 2] : w[n_k3 / 2];
			n_k++;
			save_at = now;
		}
	}

	/* the offset: how far the room stays from the target in steady running */
	if (th.eff_target >= 18 && avg > OUT_MIN + 5 && avg < max - 5 &&
	    asked > OUT_MIN + 5 && asked < max - 5 && !th.preheating &&
	    !th.lag_stop && !th.lag_nostart) {
		off_sum += room - th.eff_target;
		off_n++;
	}
	if (minutes % OFFSET_EVERY == 0) {
		if (off_n > 60)
			th.offset = clampd(th.offset + off_sum / off_n, OFFSET_MIN, OFFSET_MAX);
		off_sum = 0;
		off_n = 0;
	}
	if (now - rates_saved > 12 * 3600 && !save_at)
		save_at = now;
}

/* ---- every few seconds ----------------------------------------------------------- */

/* which side the thermostat is on; once a minute for the automatic count */
static void choose_season(int can_cool, int out_ok, double outside, int minute)
{
	static int auto_known;		/* snapped to a valid reading since auto began */
	int was = th.season;

	th.auto_ok = out_ok;
	if (can_cool < 0)
		return;			/* the boiler has not said yet: as it was */
	th.cool_ok = th.cool_wanted && can_cool;
	if (cool_mode == 2 && out_ok) {
		if (!auto_known) {
			/* first valid reading: take the side it is clearly on,
			   without the switch-over wait (a stale saved season
			   must not keep us cooling in the cold for 30 min) */
			int before = auto_season;

			if (outside >= cool_above)
				auto_season = 1;
			else if (outside <= heat_below)
				auto_season = 0;
			auto_known = 1;
			auto_minutes = 0;
			if (auto_season != before)
				save_at = time(NULL) + 5;
		} else if (minute) {
			if (auto_season ? outside <= heat_below : outside >= cool_above) {
				if (++auto_minutes >= AUTO_MINUTES) {
					auto_season = !auto_season;
					auto_minutes = 0;
					save_at = time(NULL) + 5;
				}
			} else {
				auto_minutes = 0;
			}
		}
	} else {
		auto_known = 0;		/* not auto, or no reading: snap again on resume */
		auto_minutes = 0;
	}
	th.season = th.cool_ok && (cool_mode == 2 ? auto_season : cool_mode == 1);
	if (th.season != was && th.preset >= 0)
		th.target = preset_value(th.preset);
}

/* the user's maximum water temperature now: fixed, or by the outside */
static double user_max(int out_ok, double outside)
{
	double t, f;

	if (max_mode == MAXF_FIXED)
		return clampd(max_flow, heat_start + 2, 80);
	if (max_mode != MAXF_CURVE)
		return 0;
	t = out_ok ? outside : cv_cold;
	if (cv_warm <= cv_cold || t <= cv_cold)
		f = cv_cold_flow;
	else if (t >= cv_warm)
		f = cv_warm_flow;
	else
		f = cv_cold_flow + (t - cv_cold) * (cv_warm_flow - cv_cold_flow) / (cv_warm - cv_cold);
	return clampd(f, heat_start + 2, 80);
}

/* the on/off contact: the counter moves on once a minute, the rest each tick */
/* half of a start's error (minutes; late > 0) into the correction */
static void ph_learn(double err)
{
	ph_extra = clampd(ph_extra + 0.5 * err, PH_EXTRA_MIN, PH_EXTRA_MAX);
	ph_n++;
	save_at = time(NULL);
}

/*
 * The pulses count the heat the boiler really gave (toon-boilerd waits 3
 * minutes after a stop before it asks again): the minutes of a period are
 * the minutes heat was on, so a pulse that had to wait is not lost but
 * comes later in the period, and the shortest pulse is PULSE_MIN minutes
 * of real heat.
 */
static void pulse_step(double room, double max, time_t now, int minute)
{
	double duty = max > room + 1 ? (th.out - room) / (max - room) : 1;
	int len = (int)lround(clampd(duty, 0, 1) * onoff_period), on;
	int too_warm = room > th.eff_target + OVER_STOP;

	(void)now;
	if (pulse_at < 0)
		pulse_at = 0;
	else if (minute)
		pulse_at = (pulse_at + 1) % onoff_period;
	if (minute && pulse_at == 0) {
		pulse_done = 0;		/* a new period */
		if (!th.pulse_on)
			pulse_cut = 0;
	}
	if (len > 0 && len < PULSE_MIN)
		len = PULSE_MIN;
	if (len < onoff_period && onoff_period - len < PULSE_GAP)
		len = onoff_period;		/* no pause too short to keep */
	if (too_warm)
		len = 0;
	/* heat until the period has had its minutes (of real heat), and only
	   while that can still fit in what is left of the period */
	on = pulse_done < len * 60.0 &&
	     (th.pulse_on || (onoff_period - pulse_at) * 60.0 >= fmin(len * 60.0 - pulse_done, PULSE_MIN * 60.0));
	/* what is in the radiators will do the rest (and for the rest of this
	   period); a short one would overshoot */
	if (on && th.pulse_on && th.lag_stop && pulse_heat >= PULSE_MIN * 60)
		pulse_cut = 1;
	if (pulse_cut || (on && !th.pulse_on && th.lag_nostart))
		on = 0;
	/* a pulse that started runs until it gave PULSE_MIN minutes of heat */
	if (!on && th.pulse_on && !too_warm && pulse_heat < PULSE_MIN * 60)
		on = 1;
	if (on && !th.pulse_on)
		pulse_heat = 0;
	th.pulse_on = on;
	pulse_len = len;
}

/* one line of the measurement log; written out every MEASURE_FLUSH s */
static void measure_minute(time_t now, double room, int heating, int opentherm)
{
	char line[400], *p = line, *e = line + sizeof(line);
	struct tm tm;
	const char *way = th.by_flow ? "flow" : flow_ignored && opentherm ? "district" :
			  !opentherm ? "contact" : "band";

	localtime_r(&now, &tm);
	p += strftime(p, e - p, "%Y-%m-%d %H:%M", &tm);
	p += csv_num(p, e - p, room, 2, 1);
	p += csv_num(p, e - p, th.target, 1, 1);
	p += csv_num(p, e - p, th.eff_target, 1, th.active);
	p += snprintf(p, e - p, ",%d", th.preheating);
	p += csv_num(p, e - p, th.out, 1, th.by_flow || th.active);
	p += snprintf(p, e - p, ",%d,%d", th.want, heating);
	p += th.flame >= 0 ? snprintf(p, e - p, ",%d", th.flame) : snprintf(p, e - p, ",");
	p += csv_num(p, e - p, th.flow_meas, 1, th.flow_meas > 0);
	p += csv_num(p, e - p, th.ret_meas, 1, th.ret_meas > 0);
	p += csv_num(p, e - p, th.modulation, 0, th.modulation >= 0);
	p += csv_num(p, e - p, now_outside, 1, now_out_ok);
	p += csv_num(p, e - p, pulse_done / 60, 1, th.pulse && pulse_at >= 0);
	p += snprintf(p, e - p, ",%d,%s", th.lag_stop, way);
	p += csv_num(p, e - p, 3600 * th.factor, 2, 1);
	p += csv_num(p, e - p, th.factor, 6, 1);
	p += csv_num(p, e - p, th.loss, 6, 1);
	p += csv_num(p, e - p, kloss, 7, n_k >= 3);
	p += csv_num(p, e - p, after, 5, n_after >= 3);
	p += csv_num(p, e - p, ph_extra, 0, ph_n > 0);
	p += csv_num(p, e - p, th.offset, 2, 1);
	if (p < e - 1) {
		*p++ = '\n';
		*p = 0;
	}
	if (mlen + strlen(line) >= sizeof(mbuf))
		measure_flush();
	if (mlen + strlen(line) < sizeof(mbuf)) {
		memcpy(mbuf + mlen, line, strlen(line));
		mlen += strlen(line);
	}
	if (!m_flushed)
		m_flushed = now;
	if (now - m_flushed >= MEASURE_FLUSH)
		measure_flush();
}

void th_tick(int room_ok, double room, int heating, int opentherm, double max,
	     int can_cool, int out_ok, double outside)
{
	static time_t last_pulse;
	int minute_step;
	static time_t last_minute;
	static int was_preheating, from_settings, ph_noheat;
	double off;
	time_t now = time(NULL);

	double boiler_max = max;	/* what a source that ignores the flow delivers */
	static int last_season = -1;

	if (!isfinite(room))
		room_ok = 0;		/* no reading: never heat on it */
	if (!isfinite(outside))
		out_ok = 0;
	now_out_ok = out_ok;
	now_outside = outside;
	/* the user's own maximum, when below the boiler's (OpenTherm only) */
	max_now = !opentherm ? 0 : user_max(out_ok, outside);
	if (max_now > 0 && max_now < max)
		max = max_now;
	struct tm tm;
	int mow, a, n, to_next;

	if (conf_stamp() != conf_mtime) {
		load_conf();
		take_vendor_dhw();	/* a first settings file: it goes in there */
		from_settings = 1;
		/* a preset keeps its name; its temperature may be new */
		if (th.preset >= 0)
			th.target = preset_value(th.preset);
	}
	choose_season(can_cool, out_ok, outside, now - last_minute >= 60);

	th.program = 0;
	th.preheating = 0;
	th.next[0] = 0;
	th.until[0] = 0;
	th.eff_target = th.target;
	localtime_r(&now, &tm);
	if (program_on && nsteps && tm.tm_year > 123) {	/* needs a clock that is set */
		mow = ((tm.tm_wday + 6) % 7) * 1440 + tm.tm_hour * 60 + tm.tm_min;
		find_steps(mow, &a, &n, &to_next);
		th.program = 1;
		prog_preset = steps[a].preset;
		if (hold == 1 && !hold_until)
			hold_until = now - tm.tm_sec + to_next * 60;
		if (hold == 1 && now >= hold_until) {
			hold = 0;
			hold_until = 0;
			save_at = now + 5;
		}
		snprintf(th.next, sizeof(th.next), "%02d:%02d %s", steps[n].at % 1440 / 60,
			 steps[n].at % 60, preset_name[steps[n].preset]);
		if (hold == 2) {
			/* locked: the program does not change it */
		} else if (hold) {
			snprintf(th.until, sizeof(th.until), "%02d:%02d", steps[n].at % 1440 / 60,
				 steps[n].at % 60);
		} else {
			if (a != last_step) {
				last_step = a;
				th.preset = steps[a].preset;
				th.target = preset_value(th.preset);
			}
			th.eff_target = th.target;
			/* start early for a warmer step */
			if (preheat_on && room_ok && th.hvac && !th.season &&
			    preset_value(steps[n].preset) > th.target + 0.05) {
				double want = preset_value(steps[n].preset);
				/* the water it will warm up with: the maximum for the flow
				   rule and pulses (a source that ignores the flow: its
				   own), the room band's own flow on OpenTherm */
				double pf = flow_ignored ? boiler_max :
					    opentherm && !modulation ?
					    clampd(fixed_flow > 0 ? fixed_flow : 40 + 20 * (want - room),
						   heat_start, max) : max;
				double need = (want - room - to_next * loss_at((want + room) / 2)) /
					      (fmax(pf - room, 1) * th.factor) + 15 + ph_extra;

				/* once started, a little change does not stop it */
				if (was_preheating)
					need += 5;
				/* once started it holds until the step, also when the room
				   is there a little early */
				if ((want > room || was_preheating) && (to_next <= need || was_preheating) &&
				    to_next <= max_preheat) {
					th.eff_target = want;
					th.preheating = 1;
					/* for the correction: the step, and when the room got there */
					if (!was_preheating) {
						ph_reached = 0;
						ph_want = want;
						ph_noheat = 0;
					}
					/* no heat could come (the module was off): the
					   start says nothing about the house */
					if (!th.can_heat)
						ph_noheat = 1;
					ph_step_at = now + (time_t)to_next * 60 - (now % 60);
					/* the start of the last time it stayed warm, not the
					   first time it got there */
					if (room >= want - 0.05) {
						if (!ph_reached)
							ph_reached = now;
					} else if (room < want - 0.1) {
						ph_reached = 0;
					}
				}
			}
		}
	} else {
		prog_preset = -1;
		/* the program was switched off: what was chosen stays */
		if (hold && hold_until)
			hold_until = 0;
	}

	/* frost protection, also with the thermostat off */
	if (room_ok && room < frost)
		th.frost_active = 1;
	else if (!room_ok || room > frost + 1)
		th.frost_active = 0;
	th.active = th.hvac || th.frost_active;
	th.cooling = th.season && !th.frost_active;
	if (!th.hvac)
		th.eff_target = frost + 1;
	else if (th.frost_active && th.eff_target < frost + 1)
		th.eff_target = frost + 1;

	/* the correction of starting early: a start that ends at its step */
	if (was_preheating && !th.preheating) {
		if (labs((long)(now - ph_step_at)) <= 120 && th.target >= ph_want - 0.05 &&
		    !ph_noheat) {
			/* early only if it is still warm at the step; there before but
			   cooled off again counts as late */
			if (ph_reached && room_ok && room >= ph_want - 0.1) {
				ph_learn(-fmin((ph_step_at - ph_reached) / 60.0, 60));	/* early */
			} else {
				late_since = ph_step_at;	/* late: see when it gets there */
				late_want = ph_want;
			}
		}
	}
	if (late_since) {
		if (!room_ok || th.target < late_want - 0.05 || !th.hvac || !th.can_heat) {
			late_since = 0;			/* something else came in between */
		} else if (room >= late_want - 0.05 || now - late_since >= PH_LATE_MAX * 60) {
			ph_learn(fmin((now - late_since) / 60.0, PH_LATE_MAX));
			late_since = 0;
		}
	}
	was_preheating = th.preheating;
	/* the first look only remembers; a new settings file explains the rest */
	log_check(!logged.known ? "start" : from_settings ? "settings" : "program");
	from_settings = 0;

	/* the flow temperature, and with it whether heat is wanted */
	th.by_flow = opentherm && modulation && !flow_ignored;
	th.pulse = !opentherm || flow_ignored;
	if (room_ok && th.active) {
		off = th.eff_target >= 18 ? th.offset : 0;
		th.out = th.eff_target - loss_at(th.eff_target) / th.factor +
			 (th.eff_target - room - off) * max_rate / (60 * th.factor);
		if (th.frost_active && th.out < 45)
			th.out = 45;
		th.out = clampd(th.out, OUT_MIN, max);
	} else {
		th.out = OUT_MIN;
	}

	/* what learning counts as the water the radiators got: a source that
	   ignores the flow delivers its own maximum; the room band on
	   OpenTherm asks for its own flow; the contact is full while on */
	double lmax = flow_ignored ? boiler_max : max;
	double flow_used = th.by_flow ? th.out : opentherm && !flow_ignored ? th_flow(room, max) : lmax;

	/* the heat of this tick: for the pulses in seconds */
	if (heat_tick && now > heat_tick && now - heat_tick < 120 && heating) {
		pulse_done += now - heat_tick;
		pulse_heat += now - heat_tick;
	}
	/* the boiler's own flow reading while it heats, when it gives one */
	if (heating && isfinite(th.flow_meas) && th.flow_meas > room + 1 && th.flow_meas < 95) {
		meas_acc += th.flow_meas;
		meas_n++;
	}
	/* ... and for learning in full minutes */
	if (heat_tick && now > heat_tick && now - heat_tick < 120 && heating && !th.season)
		heat_acc += (now - heat_tick) / 60.0 *
			    (lmax > room + 1 ? clampd((flow_used - room) / (lmax - room), 0, 1) : 1);
	heat_tick = now;
	if (heating && !heat_since)
		heat_since = now;
	else if (!heating)
		heat_since = 0;
	if (room_ok) {
		room_acc += room;
		room_n++;
	}

	/* each way of heating has its own after-heat */
	int mode = th.by_flow ? 1 : th.pulse ? 2 : 3;

	if (after_mode != mode) {
		/* put this mode's away, take the other's out */
		if (after_mode >= 1 && after_mode <= 3) {
			after_m[after_mode] = after;
			n_after_m[after_mode] = n_after;
			memcpy(a3_m[after_mode], a3, sizeof(a3));
			n_a3_m[after_mode] = n_a3;
		}
		after = after_m[mode];
		n_after = n_after_m[mode];
		memcpy(a3, a3_m[mode], sizeof(a3));
		n_a3 = n_a3_m[mode];
		after_mode = mode;
		/* a burst or stop measured the other way is not this mode's */
		stored = 0;
		burst_min = 0;
		watch = -1;
		pend_n = -1;
	}

	/* the after-heat, once learned: stop when what is stored will bring
	   the room to the target, do not start when even a short burst on top
	   would pass it (on the last minute's average, not one reading) */
	th.lag_stop = th.lag_nostart = 0;
	if (n_after >= 3 && room_ok && th.active && !th.cooling && !th.frost_active &&
	    (!th.by_flow || th.out >= max - 1)) {
		double r = room_m > 0 ? room_m : room, s_now = stored + heat_acc;
		/* the swing of the shortest burst centred on the target */
		double limit = th.eff_target + fmax(LAG_ALLOW, after * PULSE_MIN / 2);

		th.lag_stop = heating && heat_since && now - heat_since >= PULSE_MIN * 60 &&
			      r + after * s_now >= limit;
		th.lag_nostart = !heating && r + after * (s_now + PULSE_MIN) >= limit;
	}

	/* a boiler whose own maximum is at or below the start point could never
	   be asked for heat (not even against frost): start 2 below its maximum */
	double hs = fmin(heat_start, max - 2);

	/* the flow rule: heat above the start point, until it is somewhat
	   below it: as far as 0.15 degree of room moves the flow (at least 2),
	   so a room right on a step of its sensor (0.1) does not switch the
	   boiler on and off every few minutes */
	double hyst = fmax(2, 0.15 * max_rate / (60 * th.factor));

	flow_on = flow_on ? th.out > hs - fmin(hyst, hs - OUT_MIN - 1) : th.out > hs;
	if (th.by_flow && (heating ? th.lag_stop : th.lag_nostart))
		flow_on = 0;
	th.want = flow_on;

	/* the on/off contact: pulses */
	if (!th.pulse || !room_ok || !th.active || th.cooling) {
		pulse_at = -1;		/* the next start is a fresh period */
		th.pulse_on = 0;
		pulse_done = pulse_heat = 0;
		pulse_cut = 0;
	} else {
		minute_step = pulse_at >= 0 && now - last_pulse >= 60;
		if (pulse_at < 0 || minute_step)
			last_pulse = now;
		pulse_step(room, max, now, minute_step);
	}
	if (th.pulse)
		th.want = th.pulse_on;

	/* after a gap (no room reading, the cooling season), another sensor
	   (or a new correction of it), or a jump no room makes in a minute,
	   what was being measured is stale: start those measurements again */
	int jump = room_ok && room_m > 0 && room_n > 0 && fabs(room_acc / room_n - room_m) > 0.4;

	if ((last_minute && now - last_minute > 120) || th.season != last_season || jump ||
	    sensor_changed) {
		h_n = 0;
		watch = -1;
		burst_min = 0;
		stored = 0;
		pend_n = -1;
		full_minutes = rest_minutes = 0;
		full_sum = 0;
		heat_acc = 0;
		room_acc = room_n = 0;
		room_m = 0;
		off_sum = 0;
		off_n = 0;
		sensor_changed = 0;
		if (last_minute && now - last_minute > 120)
			last_minute = now;
	}
	last_season = th.season;

	if (room_ok && now - last_minute >= 60) {
		last_minute = now;
		room_m = room_n ? room_acc / room_n : room;
		room_acc = room_n = 0;
		/* a boiler that is only switched counts as full while it is on */
		if (!th.season)		/* what it learns is about heating */
			every_minute(now, room_m, heating ? flow_used : OUT_MIN, th.out, lmax);
		meas_acc = 0;
		meas_n = 0;
		if (measure_on)
			measure_minute(now, room_m, heating, opentherm);
	}
	/* switched off: what is still in memory is written */
	if (!measure_on && mlen) {
		measure_flush();
	}
	if (save_at && now >= save_at)
		save();
}

double th_flow(double room, double max)
{
	double flow = th.by_flow ? th.out : flow_ignored ? max :
		      fixed_flow > 0 ? fixed_flow : 40.0 + 20.0 * (th.eff_target - room);

	if (max_now > 0 && max_now < max)
		max = max_now;
	return clampd(flow, heat_start < max ? heat_start : max, max);
}

int th_hot_water_keep_warm(void)
{
	return hw_keep_warm;
}

double th_hot_water(void)
{
	time_t now = time(NULL);
	struct tm tm;
	int mow;
	double temp;

	localtime_r(&now, &tm);
	if (!hw_program_on || !hw_nsteps || tm.tm_year <= 123)
		return hw_setpoint;
	mow = ((tm.tm_wday + 6) % 7) * 1440 + tm.tm_hour * 60 + tm.tm_min;
	temp = hw_steps[hw_nsteps - 1].temp;	/* before the first: last week's last */
	for (int i = 0; i < hw_nsteps && hw_steps[i].at <= mow; i++)
		temp = hw_steps[i].temp;
	return temp;
}

void th_state(FILE *f)
{
	fprintf(f, "cool_on=%d\nseason=%s\nauto_ok=%d\n", th.cool_ok,
		th.season ? "cool" : "heat", th.auto_ok);
	fprintf(f, "hw_keep_warm=%d\n", hw_keep_warm);
	fprintf(f, "preset=%s\nprogram=%d\nprogram_enabled=%d\nhold=%d\npreheat=%d\nfrost=%d\n",
		th.preset < 0 ? "manual" : preset_name[th.preset], th.program, program_on, hold,
		th.preheating, th.frost_active);
	/* factor and loss also as degrees an hour, at full flow in a room of 20 */
	fprintf(f, "eff_target=%.1f\nflow=%.1f\nfactor=%.5f\nloss=%.5f\noffset=%.2f\n"
		"measured=%d\nheat_rate=%.1f\ncool_rate=%.2f\n",
		th.eff_target, th.out, th.factor, th.loss, th.offset, n_meas,
		60 * th.factor * 60, th.loss * 60);
	if (max_now > 0)
		fprintf(f, "max_flow_now=%.1f\n", max_now);
	fprintf(f, "flow_ignored=%d\n", flow_ignored);
	/* the loss by outside: degrees an hour per 10 degrees difference */
	fprintf(f, "kloss_n=%d\nfrom_vendor=%d\nafter_n=%d\nlag_stop=%d\n", n_k, from_vendor,
		n_after, th.lag_stop);
	if (ph_n)
		fprintf(f, "preheat_extra=%.0f\npreheat_n=%d\n", ph_extra, ph_n);
	if (n_after >= 3) {
		/* what 10 minutes of full heat still bring after the stop */
		double s10 = 0;

		for (int i = 0; i < 10; i++)
			s10 += exp(-i / STORE_TAU);
		fprintf(f, "after=%.5f\nafter_10=%.2f\n", after, after * s10);
	}
	if (n_k >= 3)
		fprintf(f, "cool_per_10=%.2f\n", kloss * 60 * 10);
	if (n_k >= 3 && now_out_ok)
		fprintf(f, "cool_rate_now=%.2f\n", loss_at(th.eff_target) * 60);
	if (th.pulse && pulse_at >= 0)
		fprintf(f, "pulse=%d/%d\npulse_minute=%d\n", pulse_len, onoff_period, pulse_at);
	if (th.next[0])
		fprintf(f, "next=%s\n", th.next);
	if (th.until[0])
		fprintf(f, "until=%s\n", th.until);
}

/* a number from the original software's file: <name>value< */
static int vendor_num(const char *xml, const char *name, double *v)
{
	char tag[48];
	const char *p;
	char *end;

	snprintf(tag, sizeof(tag), "<%s>", name);
	p = strstr(xml, tag);
	if (!p)
		return -1;
	*v = strtod(p + strlen(tag), &end);
	return end == p + strlen(tag) || !isfinite(*v) ? -1 : 0;
}

/*
 * Once, with nothing learned yet: start from what the Toon's original
 * software learned. Its measured factors ("measuredHeatingFactors:[a,b,..]",
 * oldest first) and its loss are in the same units as ours. Its offset
 * (longTermOffset) is not taken: it corrects the original software's own
 * room reading (-0.84 on one Toon 1 made ReToon ask 35 degrees more flow);
 * ours learns its own. A Toon that never heated with it has no measured
 * factors: then nothing is taken.
 */
static void take_vendor(void)
{
	char *xml, *p, *end;
	double v[64], x;
	int n = 0;
	long len;
	FILE *f;

	if (from_vendor || n_meas)
		return;
	f = fopen(VENDOR_FILE, "r");
	if (!f)
		return;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	rewind(f);
	xml = len > 0 && len < (1 << 20) ? calloc(1, len + 1) : NULL;
	if (!xml || fread(xml, 1, len, f) != (size_t)len) {
		free(xml);
		fclose(f);
		return;
	}
	fclose(f);
	p = strstr(xml, "<measuredHeatingFactors>");
	p = p ? strchr(p, '[') : NULL;
	while (p && *p && *p != ']' && n < 64) {
		x = strtod(p + 1, &end);
		if (end == p + 1)
			break;
		if (x >= 0.0001 && x <= 0.05)
			v[n++] = x;
		p = end;		/* at the comma or the ] */
	}
	if (n) {
		/* ours are newest first, at most N_MEAS */
		for (n_meas = 0; n_meas < N_MEAS && n_meas < n; n_meas++)
			meas[n_meas] = v[n - 1 - n_meas];
		if (!vendor_num(xml, "outsideRate", &x))
			th.loss = clampd(x, LOSS_MIN, LOSS_MAX);
		from_vendor = 1;
		save_at = time(NULL);
		fprintf(stderr, "toon-boilerd: started from the original software's values "
			"(%d measurements, loss %.5f)\n", n_meas, th.loss);
	}
	free(xml);
}

/*
 * Once: "Kraanwater voorverwarmen" off in the original software
 * (<dhwEnabled>0</dhwEnabled>) becomes keep_warm false, for a Toon whose
 * settings do not say yet; written to the settings so it is not asked again
 * (and a later choice is the user's). Also on a Toon that runs ReToon
 * already, as long as the original software's file is there.
 */
static void take_vendor_dhw(void)
{
	char *xml, *p;
	long len;
	FILE *f;

	if (hw_keep_set)
		return;
	f = fopen(VENDOR_FILE, "r");
	if (!f)
		return;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	rewind(f);
	xml = len > 0 && len < (1 << 20) ? calloc(1, len + 1) : NULL;
	if (xml && fread(xml, 1, len, f) == (size_t)len) {
		p = strstr(xml, "<dhwEnabled>");
		if (p && p[12] == '0' && p[13] == '<') {
			hw_keep_warm = 0;
			hw_keep_set = 1;
			save_setting_in("hot_water", "keep_warm", cJSON_CreateFalse());
			fprintf(stderr, "toon-boilerd: tap water not kept warm, as in the original software\n");
		}
	}
	free(xml);
	fclose(f);
}

void th_init(void)
{
	load_conf();
	load_saved();
	take_vendor();
	take_vendor_dhw();
	choose_factor();
	th.season = th.cool_wanted && (cool_mode == 2 ? auto_season : cool_mode == 1);
	if (th.preset >= 0)
		th.target = preset_value(th.preset);
}
