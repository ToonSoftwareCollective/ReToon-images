// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * thsim: the thermostat (../src/thermostat.c, as it is) in a simulated house,
 * for test/run.sh. Time is simulated: 10-second ticks, as toon-boilerd runs.
 *
 * The house: a room (one temperature) that loses heat to outside, and
 * radiators (their own temperature) filled by the boiler's water and giving
 * heat to the room, so heat keeps coming after a stop. toon-boilerd's own
 * rules are copied here: its room band (modulation off), its 3-minute pause
 * after a stop, and a heat request only on a real reading.
 *
 *   thsim <mode> <days>        mode 0 flow rule (OpenTherm), 1 district heating
 *                              (OpenTherm, flow ignored), 2 on/off contact,
 *                              3 room band (OpenTherm, modulation off, flow 60)
 *
 * Environment: TO outside (5), NOISE sensor noise, Q sensor step (0.01),
 * SUN extra warmth 11-16 h from day 4, NANH sensor gives "nan" on day 2 from
 * that hour, JUMP degrees added to the reading from day 2 noon (another
 * sensor), NANRAW the "nan" goes to the thermostat as a reading (its own
 * guard), MAXF the boiler's maximum, PAUSE thermostat off on day 2
 * 12:00-12:30 (just after a pulse stopped for after-heat) and then 22, VENDOR an original-software file to start
 * from, STATE extra lines for the saved state, NOPROG no week program,
 * MEASURE the measurement log on.
 *
 * It prints a line per day and one with what was learned; run.sh checks them.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static time_t sim_now;
static time_t sim_time(time_t *t)
{
	if (t)
		*t = sim_now;
	return sim_now;
}
#define time sim_time
#include "../src/thermostat.c"
#undef time

/* the house: k_true per minute per degree to outside, as the tests expect */
#define K_TRUE		0.0005
static double room = 19, rad = 19, outside = 5;

static double rnd(void)
{
	return rand() / (double)RAND_MAX * 2 - 1;
}

static double env(const char *name, double def)
{
	const char *v = getenv(name);

	return v ? atof(v) : def;
}

int main(int argc, char **argv)
{
	int mode = argc > 1 ? atoi(argv[1]) : 0, days = argc > 2 ? atoi(argv[2]) : 3;
	double noise = env("NOISE", 0), q = env("Q", 0.01), sun_w = env("SUN", 0);
	double max = env("MAXF", mode == 2 ? 80 : 70), jump = env("JUMP", 0);
	int nan_h = getenv("NANH") ? atoi(getenv("NANH")) : -1, pause = !!getenv("PAUSE");
	int heating = 0, prev = 0, cycles = 0, nd = 0, nan_heat = 0, resume_wait = -1;
	long long released = -1000000;
	double rmin = 99, rmax = -99, dev = 0, at7 = 0;
	struct tm tm = { 0 };
	FILE *f;

	outside = env("TO", 5);
	srand(1);
	f = fopen(CONF_FILE, "w");
	fprintf(f, "{\"presets\":{\"comfort\":20,\"home\":18,\"sleep\":16,\"away\":12},"
		"\"program_enabled\":%s,\"program\":[", getenv("NOPROG") ? "false" : "true");
	for (int d = 0; d < 7; d++)
		fprintf(f, "%s[[\"07:00\",\"comfort\"],[\"23:00\",\"sleep\"]]", d ? "," : "");
	fprintf(f, "],\"modulation\":%s,\"flow_ignored\":%s,\"flow\":%s,\"log_measurements\":%s}",
		mode == 3 ? "false" : "true", mode == 1 ? "true" : "false", mode == 3 ? "60" : "0",
		getenv("MEASURE") ? "true" : "false");
	fclose(f);
	f = fopen(SAVE_FILE, "w");
	fprintf(f, "hvac=heat\ntarget=20\npreset=comfort\n%s", getenv("STATE") ? getenv("STATE") : "");
	fclose(f);
	if (getenv("VENDOR")) {
		char cmd[512];

		snprintf(cmd, sizeof(cmd), "cp '%s' '%s'", getenv("VENDOR"), VENDOR_FILE);
		if (system(cmd))
			return 2;
	} else {
		remove(VENDOR_FILE);
	}
	tm.tm_year = 126, tm.tm_mon = 10, tm.tm_mday = 9, tm.tm_isdst = -1;
	sim_now = mktime(&tm);
	th_init();

	for (long s = 0; s < days * 8640L; s++) {
		struct tm lt;
		long day = s / 8640;
		double reading, supply = 0, sun = 0;
		int want, ok;

		sim_now += 10;
		localtime_r(&sim_now, &lt);
		reading = round((room + noise * rnd()) / q) * q;
		if (day == 1 && nan_h >= 0 && lt.tm_hour >= nan_h)
			reading = NAN;
		if (jump && (day > 1 || (day == 1 && lt.tm_hour >= 12)))
			reading += jump;
		/* MAXCMD at 01:00 and MAXCMD2 at 02:00 of the first day (a maximum
		   from Home Assistant), and what is in force half an hour later */
		if (!day && lt.tm_min == 0 && lt.tm_sec == 0 &&
		    getenv(lt.tm_hour == 1 ? "MAXCMD" : "MAXCMD2") && (lt.tm_hour == 1 || lt.tm_hour == 2))
			th_command(getenv(lt.tm_hour == 1 ? "MAXCMD" : "MAXCMD2"));
		if (!day && lt.tm_min == 30 && lt.tm_sec == 0 && (lt.tm_hour == 1 || lt.tm_hour == 2))
			printf("maxflow hour %d now %.1f mode %d\n", lt.tm_hour, max_now, max_mode);
		if (pause && day == 1 && lt.tm_hour == 12 && lt.tm_min == 0 && lt.tm_sec == 0) {
			/* as right after a pulse that the after-heat stopped */
			pulse_cut = 1;
			pulse_done = 9999;
			th_command("hvac off");
		}
		if (pause && day == 1 && lt.tm_hour == 12 && lt.tm_min == 30 && lt.tm_sec == 0) {
			th_command("hvac heat");
			th_command("target 22");
			resume_wait = 0;
		}
		/* toon-boilerd: a reading that is not one is no reading (NANRAW:
		   pass it on anyway, to test the thermostat's own guard) */
		ok = isfinite(reading) && reading > -30 && reading < 60;
		if (getenv("NANRAW"))
			ok = 1;
		th.flow_meas = th.ret_meas = th.modulation = -1;
		th.flame = -1;
		/* NOHEAT: no boiler module answering, no heat comes */
		th.can_heat = !getenv("NOHEAT");
		th_tick(ok, reading, heating, mode != 2, max, 0, 1, outside);
		if (!th.active || (!ok && !getenv("NANRAW")) || !th.can_heat)
			want = 0;
		else if (th.by_flow || th.pulse)
			want = th.want;
		else if (heating ? th.lag_stop : th.lag_nostart)
			want = 0;
		else if (reading <= th.eff_target - 0.2)
			want = 1;
		else if (reading >= th.eff_target + 0.2)
			want = 0;
		else
			want = heating;
		if (heating && !want) {
			heating = 0;
			released = sim_now;
		}
		if (want && !heating && sim_now - released >= 180)
			heating = 1;
		if (heating)
			supply = mode == 0 || mode == 3 ? th_flow(reading, max) : max;
		if (sun_w && day >= 3 && lt.tm_hour >= 11 && lt.tm_hour < 16)
			sun = sun_w;
		if (!isfinite(reading) && heating)
			nan_heat++;
		if (resume_wait >= 0) {
			if (heating)
				printf("resume: heat after %d s\n", resume_wait * 10), resume_wait = -1;
			else if (++resume_wait > 360)
				printf("resume: no heat after 60 min\n"), resume_wait = -1;
		}
		/* 10 s in steps of a second: radiators (heat capacity 0.01 of the
		   room's) filled by the boiler's water, giving to the room */
		for (int i = 0; i < 10; i++) {
			double dt = 1 / 60.0, give = 0.0005 * (rad - room);

			rad += ((heating ? 0.02 * (supply - rad) : 0) - give) / 0.01 * dt;
			room += (give - K_TRUE * (room - outside) + sun) * dt;
		}
		if (heating && !prev)
			cycles++;
		prev = heating;
		if (lt.tm_hour == 7 && lt.tm_min == 0 && lt.tm_sec == 0)
			at7 = room;
		if (lt.tm_hour >= 10 && lt.tm_hour < 22) {
			rmin = fmin(rmin, room);
			rmax = fmax(rmax, room);
			dev += room - th.eff_target;
			nd++;
		}
		if (lt.tm_hour == 23 && lt.tm_min == 0 && lt.tm_sec == 0) {
			printf("day %ld cycles %d min %.2f max %.2f dev %.3f at7 %.2f nanheat %d\n",
			       day + 1, cycles, rmin, rmax, dev / nd, at7, nan_heat / 6);
			cycles = nd = nan_heat = 0;
			rmin = 99, rmax = -99, dev = 0;
		}
	}
	printf("learned factor %.6f measured %d loss %.6f k %.6f k_n %d offset %.3f after %.5f "
	       "after_n %d preheat_extra %.1f from_vendor %d\n", th.factor, n_meas, th.loss, kloss,
	       n_k, th.offset, after, n_after, ph_extra, from_vendor);
	return 0;
}
