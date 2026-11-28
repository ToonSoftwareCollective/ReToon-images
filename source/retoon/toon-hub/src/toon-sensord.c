// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * toon-sensord: room climate of the Toon 2, for everything else to read.
 *
 * The Toon 2 has two ENS210 sensors: a primary one on I2C bus 3 and a
 * "compensation" one on bus 2 that sits closer to the electronics. The room
 * temperature is estimated from the pair with this fit:
 *
 *   x = primary - 0.49 * compensation
 *   estimate = 0.02199902 x^2 + 1.62213512 x + 1.80295257
 *
 * Relative humidity is then restated for the estimated temperature. Readings
 * are smoothed over a few samples. Light comes from the
 * LTR303. Some Toon 2 versions have an HTS221 (0x5f) in place of an ENS210
 * (0x43); each bus is checked for either, so the same code serves both.
 *
 * The Toon 1 has no ENS210: an ADT7410 inside the case (0-0049 on the units
 * seen so far; found by name through hwmon). The warmth of the board and the
 * screen is subtracted, each part smoothed with its own time constant
 * ("default" parameter set):
 *
 *   room = sensor - 3.76 - 0.88 * on_f - 2.16 * bright_f
 *   on_f:     screen on (1) / blanked (0), smoothed with tau = 1424 s
 *   bright_f: backlight 0..1, smoothed with tau = 1010 s
 *   smoothing: f += dt / (dt + tau) * (value - f), starting at the value
 *
 * Two more parts. After a cold start the board's own warmth (the fixed
 * 3.76) is not there yet either: a Toon 1 switched on from cold warmed
 * up with tau = 860 s, so that part is smoothed too. And the smoothed parts
 * survive a restart: they are kept in /run/toon/sensord.state, the init
 * script copies that to the data partition when it stops, and the next start
 * lets them cool off for the time the Toon was off (none saved = cold).
 * The sensor value itself is smoothed with tau = 180 s.
 *
 * The parameter set follows the Toon's own part number (pn= in
 * the U-Boot environment): 6500-1200-4901 and -4902 run warmer and get
 * offset 6.13, 1.27, 2.32 with tau 1171 / 955 s; every other number the
 * "default" set above. No humidity or light there.
 *
 * The user's correction ("temp_offset" in the thermostat settings, -5..5
 * in 0.1 steps) is added to the result on both Toons; on the Toon 2 the
 * humidity is restated for the corrected temperature too.
 *
 * Result on the Toon's own broker as retoon/climate (libtoonbus, retained
 * JSON), every 5 seconds.
 */

#include <dirent.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <toonbus.h>

#define PERIOD_S	5
#define STATE_RUN	"/run/toon/sensord.state"
#define STATE_SAVED	"/mnt/data/toon-os/sensord.state"
#define TAU_BOARD	860.0
#define TAU_SENSOR	180.0
#define TH_CONF		"/mnt/data/toon-os/config/thermostat.json"
#define OFFSET_MAX	5.0

static volatile sig_atomic_t stop;

/* the thermostat settings' time, to see a new correction between readings */
static long long conf_time(void)
{
	struct stat st;

	return stat(TH_CONF, &st) ? -1 :
	       st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec + st.st_size;
}

/* the user's correction, read again when the thermostat settings change */
static double temp_offset(void)
{
	static double off;
	static long long mtime = -2;
	long long now = conf_time();
	struct stat st;
	char *buf, *p;
	size_t n;
	FILE *f;

	if (now == mtime)
		return off;
	mtime = now;
	if (stat(TH_CONF, &st))
		return off = 0;
	off = 0;
	f = fopen(TH_CONF, "r");
	buf = f && st.st_size < 1 << 20 ? malloc(st.st_size + 1) : NULL;
	if (!buf) {
		if (f)
			fclose(f);
		return off;
	}
	n = fread(buf, 1, st.st_size, f);
	fclose(f);
	buf[n] = 0;
	p = strstr(buf, "\"temp_offset\"");
	if (p && (p = strchr(p, ':'))) {
		off = round(strtod(p + 1, NULL) * 10) / 10;
		if (!isfinite(off))
			off = 0;
		off = off > OFFSET_MAX ? OFFSET_MAX : off < -OFFSET_MAX ? -OFFSET_MAX : off;
	}
	free(buf);
	return off;
}

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static int read_long(const char *dir, const char *attr, long *val)
{
	char path[400], buf[32];
	FILE *f;
	int ok;

	snprintf(path, sizeof(path), "%s/%s", dir, attr);
	f = fopen(path, "r");
	if (!f)
		return -1;
	ok = fgets(buf, sizeof(buf), f) != NULL;
	fclose(f);
	if (!ok)
		return -1;
	*val = strtol(buf, NULL, 10);
	return 0;
}

static int read_double(const char *dir, const char *attr, double *val)
{
	char path[400], buf[32];
	FILE *f;
	int ok;

	snprintf(path, sizeof(path), "%s/%s", dir, attr);
	f = fopen(path, "r");
	if (!f)
		return -1;
	ok = fgets(buf, sizeof(buf), f) != NULL;
	fclose(f);
	if (!ok)
		return -1;
	*val = strtod(buf, NULL);
	return 0;
}

/*
 * A channel in milli-units ("temp", "humidityrelative"): the ENS210 gives
 * in_<chan>_input, the HTS221 raw + offset, times scale.
 */
static int read_chan(const char *dir, const char *chan, long *val)
{
	char attr[64];
	double raw, off = 0, scale = 1;

	snprintf(attr, sizeof(attr), "in_%s_input", chan);
	if (!read_long(dir, attr, val))
		return 0;
	snprintf(attr, sizeof(attr), "in_%s_raw", chan);
	if (read_double(dir, attr, &raw))
		return -1;
	snprintf(attr, sizeof(attr), "in_%s_offset", chan);
	read_double(dir, attr, &off);
	snprintf(attr, sizeof(attr), "in_%s_scale", chan);
	read_double(dir, attr, &scale);
	*val = lround((raw + off) * scale);
	return 0;
}

/* The IIO device directory below an I2C device, e.g. "3-0043". */
static int iio_of_i2c(const char *i2c, char *dir, size_t len)
{
	char path[100];
	struct dirent *de;
	DIR *d;
	int found = -1;

	snprintf(path, sizeof(path), "/sys/bus/i2c/devices/%s", i2c);
	d = opendir(path);
	if (!d)
		return -1;
	while (found && (de = readdir(d)))
		if (!strncmp(de->d_name, "iio:device", 10)) {
			snprintf(dir, len, "%s/%.60s", path, de->d_name);
			found = 0;
		}
	closedir(d);
	return found;
}

/* The climate sensor on an I2C bus: an ENS210 (0x43) or an HTS221 (0x5f). */
static int climate_of_bus(int bus, char *dir, size_t len)
{
	static const char *const addr[] = { "0043", "005f" };
	char i2c[16];
	unsigned int k;

	for (k = 0; k < sizeof(addr) / sizeof(addr[0]); k++) {
		snprintf(i2c, sizeof(i2c), "%d-%s", bus, addr[k]);
		if (!iio_of_i2c(i2c, dir, len))
			return 0;
	}
	return -1;
}

/* Find the first IIO device with this name. */
static int find_iio(const char *name, char *dir, size_t len)
{
	char path[100], buf[64];
	struct dirent *de;
	DIR *d = opendir("/sys/bus/iio/devices");
	int found = -1;

	if (!d)
		return -1;
	while (found && (de = readdir(d))) {
		FILE *f;

		if (strncmp(de->d_name, "iio:device", 10))
			continue;
		snprintf(path, sizeof(path), "/sys/bus/iio/devices/%.60s/name", de->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (fgets(buf, sizeof(buf), f) && !strncmp(buf, name, strlen(name))) {
			snprintf(dir, len, "/sys/bus/iio/devices/%.60s", de->d_name);
			found = 0;
		}
		fclose(f);
	}
	closedir(d);
	return found;
}

/*
 * Toon 1: the first hwmon temperature sensor by type, in this order (an
 * ADT7410 is the one the board has in every version seen so far). Returns
 * millidegrees in *val and the device ("0-0049") in dev.
 */
static int hwmon_temp(long *val, char *dev, size_t len)
{
	static const char *const want[] = { "adt7410", "tmp431", "tmp401" };
	char path[400], name[32];
	int best = -1;

	for (int i = 0; i < 16; i++) {
		FILE *f;
		long t;

		snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/name", i);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (!fgets(name, sizeof(name), f))
			name[0] = 0;
		fclose(f);
		name[strcspn(name, "\n")] = 0;
		for (int k = 0; k < 3; k++) {
			if (strcmp(name, want[k]) || (best >= 0 && k >= best))
				continue;
			snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d", i);
			if (read_long(path, "temp1_input", &t))
				continue;
			*val = t;
			best = k;
			snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/device", i);
			{
				char link[400];
				ssize_t n = readlink(path, link, sizeof(link) - 1);

				if (n > 0) {
					link[n] = 0;
					snprintf(dev, len, "%s", strrchr(link, '/') ? strrchr(link, '/') + 1 : link);
				} else {
					snprintf(dev, len, "%s", name);
				}
			}
		}
	}
	return best < 0 ? -1 : 0;
}

/* The two parameter sets of the Toon 1 heat correction */
struct tcorr {
	const char *name;
	double offset, k_on, k_bright, tau_on, tau_bright;
};
static const struct tcorr corr_default = { "toon1-default", 3.76, 0.88, 2.16, 1424, 1010 };
static const struct tcorr corr_4901 = { "toon1-4901", 6.13, 1.27, 2.32, 1171, 955 };

/* pn= from the U-Boot environment (read only), e.g. "6500-1400-2001" */
static void uboot_pn(char *pn, size_t len)
{
	char line[128], dev[32] = "", buf[8192];
	FILE *f = fopen("/proc/mtd", "r");
	size_t n = 0;

	pn[0] = 0;
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		int num;

		if (strstr(line, "\"u-boot-env\"") && sscanf(line, "mtd%d:", &num) == 1) {
			snprintf(dev, sizeof(dev), "/dev/mtd%d", num);
			break;
		}
	}
	fclose(f);
	if (!dev[0] || !(f = fopen(dev, "r")))
		return;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	/* 4 bytes CRC, then "name=value" strings ending in NUL, NUL NUL at the end */
	for (size_t i = 4; i < n && buf[i]; i += strlen(buf + i) + 1)
		if (!strncmp(buf + i, "pn=", 3)) {
			snprintf(pn, len, "%s", buf + i + 3);
			return;
		}
}

static const struct tcorr *toon1_corr(char *pn, size_t len)
{
	uboot_pn(pn, len);
	if (!strcmp(pn, "6500-1200-4901") || !strcmp(pn, "6500-1200-4902"))
		return &corr_4901;
	return &corr_default;
}

/* Toon 1 screen warmth: backlight 0..1 and whether the screen is on */
static void screen_state(double *bright, double *on)
{
	char path[300], name[64];
	long b = 0, max = 0, blank = 0;
	DIR *d = opendir("/sys/class/backlight");
	struct dirent *de;

	*bright = 0;
	if (d) {
		while ((de = readdir(d))) {
			if (de->d_name[0] == '.')
				continue;
			snprintf(name, sizeof(name), "%.60s", de->d_name);
			snprintf(path, sizeof(path), "/sys/class/backlight/%s", name);
			if (!read_long(path, "actual_brightness", &b) && !read_long(path, "max_brightness", &max) && max > 0)
				*bright = (double)b / max;
			break;
		}
		closedir(d);
	}
	read_long("/sys/class/graphics/fb0", "blank", &blank);
	*on = !blank && *bright > 0 ? 1 : 0;
}

/* Saturation vapour pressure (Magnus), only used as a ratio. */
static double vapour_pressure(double t)
{
	return exp(17.62 * t / (243.12 + t));
}

/* The smoothed heat parts of the Toon 1, kept across a restart. */
struct heat { double board, on, bright; };

static void heat_load(struct heat *h, const struct tcorr *c)
{
	FILE *f = fopen(STATE_SAVED, "r");
	long saved = 0, off;
	char line[64];

	memset(h, 0, sizeof(*h));	/* nothing saved: start cold */
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		sscanf(line, "time=%ld", &saved);
		sscanf(line, "board=%lf", &h->board);
		sscanf(line, "on=%lf", &h->on);
		sscanf(line, "bright=%lf", &h->bright);
	}
	fclose(f);
	off = time(NULL) - saved;
	if (!saved || off < 0) {	/* no time or the clock is wrong */
		memset(h, 0, sizeof(*h));
		return;
	}
	/* switched off: everything cools down towards 0 */
	h->board *= exp(-off / TAU_BOARD);
	h->on *= exp(-off / c->tau_on);
	h->bright *= exp(-off / c->tau_bright);
}

static void heat_keep(const struct heat *h)
{
	FILE *f = fopen(STATE_RUN ".tmp", "w");

	if (!f)
		return;
	fprintf(f, "time=%ld\nboard=%.4f\non=%.4f\nbright=%.4f\n",
		(long)time(NULL), h->board, h->on, h->bright);
	fclose(f);
	rename(STATE_RUN ".tmp", STATE_RUN);
}

int main(void)
{
	double t_avg = 0, h_avg = 0;
	int primed = 0;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	mkdir("/run/toon", 0755);
	bus_open("sensord");

	while (!stop) {
		char pdir[200], cdir[200], ldir[200];
		long tp, tc, hp, lux;
		double est, hum = -1, off = temp_offset();
		char *out = NULL;
		size_t out_len = 0;
		FILE *f = open_memstream(&out, &out_len);

		if (!f)
			return 1;

		if (!climate_of_bus(3, pdir, sizeof(pdir)) &&
		    !read_chan(pdir, "temp", &tp)) {
			char npath[220], name[32] = "";
			FILE *n;

			snprintf(npath, sizeof(npath), "%s/name", pdir);
			n = fopen(npath, "r");
			if (n) {
				if (fgets(name, sizeof(name), n))
					name[strcspn(name, "\n")] = 0;
				fclose(n);
			}
			est = tp / 1000.0;
			fprintf(f, "primary=%.2f\nsensor=%s\n", est, name);
			if (!climate_of_bus(2, cdir, sizeof(cdir)) &&
			    !read_chan(cdir, "temp", &tc)) {
				double x = tp / 1000.0 - 0.49 * (tc / 1000.0);

				est = 0.02199902 * x * x + 1.62213512 * x + 1.80295257;
				fprintf(f, "compensation=%.2f\n", tc / 1000.0);
			}
			if (!read_chan(pdir, "humidityrelative", &hp)) {
				hum = hp / 1000.0 * vapour_pressure(tp / 1000.0) /
				      vapour_pressure(est + off);
				if (hum > 100)
					hum = 100;
			}
			t_avg = primed ? t_avg + (est - t_avg) * 0.3 : est;
			if (hum >= 0)
				h_avg = primed ? h_avg + (hum - h_avg) * 0.3 : hum;
			primed = 1;
			/* the correction after the smoothing: a new one counts at once */
			fprintf(f, "temperature=%.2f\nuncorrected=%.2f\ntemp_offset=%.1f\n",
				t_avg + off, t_avg, off);
			if (hum >= 0)
				fprintf(f, "humidity=%.1f\n", h_avg);
		} else {
			char dev[64];

			if (!hwmon_temp(&tp, dev, sizeof(dev))) {	/* Toon 1 */
				static double raw_f, t_last;
				static struct heat h;
				static const struct tcorr *c;
				static char pn[40];
				struct timespec ts;
				double now, bright, on, dt;

				if (!c) {
					c = toon1_corr(pn, sizeof(pn));
					heat_load(&h, c);
				}
				clock_gettime(CLOCK_MONOTONIC, &ts);
				now = ts.tv_sec + ts.tv_nsec / 1e9;
				screen_state(&bright, &on);
				if (!primed) {
					raw_f = tp / 1000.0;
				} else {
					dt = now - t_last;
					raw_f += dt / (dt + TAU_SENSOR) * (tp / 1000.0 - raw_f);
					h.board += dt / (dt + TAU_BOARD) * (1 - h.board);
					h.on += dt / (dt + c->tau_on) * (on - h.on);
					h.bright += dt / (dt + c->tau_bright) * (bright - h.bright);
				}
				t_last = now;
				primed = 1;
				heat_keep(&h);
				t_avg = raw_f - c->offset * h.board - c->k_on * h.on - c->k_bright * h.bright;
				fprintf(f, "primary=%.2f\nsensor=%s\ncorrection=%s\npn=%s\n", tp / 1000.0, dev, c->name, pn[0] ? pn : "unknown");
				fprintf(f, "warm_board=%.3f\nscreen_on=%.3f\nbacklight=%.3f\n", h.board, h.on, h.bright);
				fprintf(f, "temperature=%.2f\nuncorrected=%.2f\ntemp_offset=%.1f\n",
					t_avg + off, t_avg, off);
			}
		}

		if (!find_iio("ltr303", ldir, sizeof(ldir)) &&
		    !read_long(ldir, "in_illuminance_input", &lux))
			fprintf(f, "illuminance=%ld\n", lux);

		fclose(f);
		bus_put("climate", out);
		free(out);
		/* a second at a time: new settings (a new correction) count at once */
		long long was = conf_time();

		for (int i = 0; i < PERIOD_S && !stop && conf_time() == was; i++)
			sleep(1);
	}
	bus_put("climate", NULL);
	bus_close();
	return 0;
}
