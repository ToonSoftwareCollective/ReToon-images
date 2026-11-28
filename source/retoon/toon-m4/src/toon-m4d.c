// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * toon-m4d: Linux side of the Toon 2 Cortex-M4 sensor firmware.
 *
 * The M4 (firmware in src/firmware/) samples the VOC sensor and the ultrasonic motion
 * detector on its own core and writes results into a shared mailbox in TCMU,
 * which the A9 sees at 0x00800000. This daemon:
 *   - polls that mailbox a few times a second,
 *   - turns the raw VOC count into the air-quality index in retoon/voc,
 *     byte-for-byte compatible with the old toon-vocd (so toon-hub/HA are
 *     unchanged) PLUS improved fields (0-500 level, %-of-baseline, and an
 *     optional ENS210 temperature/humidity compensation),
 *   - publishes motion/wake state to retoon/motion and (optionally) wakes
 *     the backlight on an approach,
 *   - feeds screen-touch events back to the M4 (touch_seq) as free ground-truth
 *     labels for its online learning.
 *
 * The firmware itself is loaded by the kernel's remoteproc (or m4load) from the
 * init script before this daemon starts; the daemon maps the mailbox and runs
 * the policy.
 *
 * It also supervises the M4: a frozen heartbeat or a HardFault report gets it
 * restarted (remoteproc, or m4load), with a growing pause between attempts;
 * the state is in retoon/m4 (web Status page) and in syslog.
 *
 * Replaces toon-voc. For runtime testing before the device-tree handoff, stop
 * S45toon-vocd first (it owns ADC2/PWM5 until then).
 */
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <toonbus.h>

/* ---- mailbox layout (must match firmware/shared.h) ---- */
#define TCMU_A9		0x00800000u
#define M4_MAGIC	0xB007C0DEu
#define OFF_MAGIC	0x00
#define OFF_FW_VERSION	0x04
#define OFF_HEARTBEAT	0x08
#define OFF_VOC_SEQ	0x10
#define OFF_VOC_RAW	0x14
#define OFF_US_SEQ	0x18
#define OFF_US_RESID	0x1c
#define OFF_US_ACTIVE	0x20
#define OFF_US_WAKE	0x24
#define OFF_US_WAKECNT	0x28
#define OFF_US_THRESH	0x2c
#define OFF_FAULT	0x34		/* flag, cfsr, hfsr, bfar, pc, lr */
#define M4_FAULT_FLAG	0xFA17FA17u
#define OFF_A9_CTRL	0x50
#define OFF_TOUCH_SEQ	0x58
#define OFF_TOUCH_MS	0x5c
#define OFF_US_RANGE	0x80
#define OFF_US_SCORE	0x84
#define OFF_US_MODE	0x88
#define OFF_US_PRESENT	0x94
#define OFF_US_LEARNED	0x98
#define OFF_US_RELEARNS	0xa4
#define OFF_US_TX_PP	0xa8
#define OFF_US_CARRIER	0xac
#define CTRL_VOC_EN	(1u << 0)
#define CTRL_US_EN	(1u << 1)
#define CTRL_US_PULSED	(1u << 2)	/* pulsed ranging instead of CW (the default) */
#define CTRL_OWN_PERIPH	(1u << 3)	/* firmware owns PWM clocks / ADC init */
#define CTRL_LEARN	(1u << 4)	/* touch learning (default off: fixed thresholds) */
#define CTRL_SND_EN	(1u << 6)	/* sound on the buzzer: the M4 owns PWM4 (toon-sound) */

#define WARMUP_S	300
/* on the Toon's own MQTT broker (libtoonbus): retoon/<name> */
#define VOC_BUS		"voc"
#define MOTION_BUS	"motion"
#define CLIMATE_BUS	"climate"	/* toon-sensord: primary, temperature, humidity */
#define UI_CMD		"retoon/ui/set"	/* toon-ui's commands */
#define M4_BUS		"m4"		/* supervisor state, for the web Status page */
#define STATE_MIN_MS	2000		/* voc and motion at most this often, unless they change */

static long long mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}
#define RPROC		"/sys/class/remoteproc/remoteproc0"
#define M4_FIRMWARE	"/usr/share/toon/toon2-m4.bin"	/* m4load fallback without remoteproc */
#define STALL_S		3	/* heartbeat frozen this long = M4 stuck */
#define BACKOFF_MAX	300	/* s between restart attempts, at most */
#define HEALTHY_S	600	/* running this long resets the backoff */


static volatile sig_atomic_t stop;
static void on_signal(int s) { (void)s; stop = 1; }

static volatile uint32_t *map_mailbox(void)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	void *p;
	if (fd < 0) { perror("/dev/mem"); exit(1); }
	p = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, TCMU_A9);
	if (p == MAP_FAILED) { perror("mmap mailbox"); exit(1); }
	return (volatile uint32_t *)p;
}


/* temperature and humidity near the gas sensor, from toon-sensord: the raw
   in-case reading ("primary"), else the room temperature */
static int read_temp_rh(double *tc, double *rh)
{
	char v[32];
	int got = 0;

	if (!bus_value(CLIMATE_BUS, "primary", v, sizeof(v)) ||
	    !bus_value(CLIMATE_BUS, "temperature", v, sizeof(v))) {
		*tc = atof(v);
		got |= 1;
	}
	if (!bus_value(CLIMATE_BUS, "humidity", v, sizeof(v))) {
		*rh = atof(v);
		got |= 2;
	}
	return got == 3;
}

/* ---- the gas sensor as a resistance ----------------------------------------
 * The CCS801 sits in a divider with 200 kOhm to 3.3 V, read by the 12-bit
 * ADC: Rs = 200k * V / (3.3 - V), kept within 10 kOhm - 1.6 MOhm. Metal-oxide
 * sensors are described by their resistance against clean air (R0/Rs follows
 * a power of the gas concentration), so the air is judged by
 * d = log2(R0 / Rs): how many halvings the resistance is below clean air. */
#define RS_LOAD		200000.0
#define VREF		3.3

static double sensor_log2r(double adc)
{
	double v = adc * VREF / 4095.0, r;

	if (v > 2.93)
		r = 1.6e6;
	else if (v < 0.16)
		r = 1e4;
	else
		r = RS_LOAD * v / (VREF - v);
	return log2(r);
}

/* Warmer and damper air lower the resistance as well. Corrected toward 25 C
   and 50 %; halvings per degree and per % RH. 0 until fitted from logged
   quiet nights (lr, tempC and rh are in retoon/voc for that). */
#define COMP_T		0.0
#define COMP_RH		0.0

static double compensate(double lr, double tc, double rh, int have_trh)
{
	return have_trh ? lr + COMP_T * (tc - 25) + COMP_RH * (rh - 50) : lr;
}

/* Clean air: the highest corrected resistance of the last 24 hours, kept as
   one maximum per clock hour (so it survives a restart); cleaner air raises
   it at once. It assumes the room is clean (aired, or empty at night) at
   least once a day. */
static double base_max[24];
static long base_hour[24];		/* hour number (time / 3600) of each slot; 0 = empty */

static void base_add(double lr, time_t wall)
{
	long h = wall / 3600;
	int i = h % 24;

	if (base_hour[i] != h) {
		base_hour[i] = h;
		base_max[i] = lr;
	} else if (lr > base_max[i]) {
		base_max[i] = lr;
	}
}

static int base_get(time_t wall, double *b)
{
	long h = wall / 3600;
	int got = 0;

	for (int i = 0; i < 24; i++)
		if (base_hour[i] && h - base_hour[i] < 24 && h >= base_hour[i] &&
		    (!got || base_max[i] > *b)) {
			*b = base_max[i];
			got = 1;
		}
	return got;
}

/* ---- air quality as a word ------------------------------------------------
 * d has no fixed meaning across Toons, so the word comes from this Toon's own
 * history of d: a histogram of one value a minute that slowly forgets (half
 * the weight is gone after a week). Good is what is usual in this room (up to
 * the 80th percentile), fair up to the 95th, moderate up to the 99th, poor
 * above that. Each step is at least aq_floor and AQ_GAP above the one before,
 * so a room with very clean air is not called poor for a small change. Until
 * a day is learned the starting steps are used. History, the last word and
 * the hourly clean-air maxima are kept on the data partition twice a day and
 * when toon-m4d stops. */
#define AQ_BINS		120		/* 0.02 halving each: 0-2.4 */
#define AQ_BIN		0.02
#define AQ_GAP		0.2
#define AQ_TREND	0.15		/* halvings in 15 minutes that count as a trend */
#define AQ_DECAY	0.99993124	/* per minute: 0.5 after 7 days */
#define AQ_LEARNED	1300.0		/* about a day of minutes, as the history forgets */
#define AQ_SAVE_S	(12 * 3600)
#define AQ_FILE		"/mnt/data/toon-os/config/air-quality"

static double aq_hist[AQ_BINS], aq_sum;
static const double aq_start[3] = { 0.35, 0.9, 1.6 }, aq_floor[3] = { 0.2, 0.5, 0.9 };
static double aq_t[3] = { 0.35, 0.9, 1.6 };
static int aq_level = -1;		/* 0 good .. 3 poor */
static int aq_last = -1;		/* the word before the last stop, for the first minutes */
static double aq_ring[16];		/* the last minutes, for the trend */
static int aq_ring_n, aq_ring_i;
static const char *const aq_word[] = { "good", "fair", "moderate", "poor" };

static double aq_percentile(double p)
{
	double want = aq_sum * p, acc = 0;

	for (int i = 0; i < AQ_BINS; i++)
		if ((acc += aq_hist[i]) >= want)
			return (i + 1) * AQ_BIN;
	return AQ_BINS * AQ_BIN;
}

static void aq_thresholds(void)
{
	static const double at[3] = { 0.80, 0.95, 0.99 };

	if (aq_sum < AQ_LEARNED) {
		memcpy(aq_t, aq_start, sizeof(aq_t));
		return;
	}
	for (int i = 0; i < 3; i++) {
		double t = aq_percentile(at[i]);

		if (t < aq_floor[i])
			t = aq_floor[i];
		if (i && t < aq_t[i - 1] + AQ_GAP)
			t = aq_t[i - 1] + AQ_GAP;
		aq_t[i] = t;
	}
}

/* one value a minute into the history */
static void aq_minute(double d)
{
	int b = (int)(d / AQ_BIN);

	if (b < 0) b = 0;
	if (b >= AQ_BINS) b = AQ_BINS - 1;
	aq_sum = 0;
	for (int i = 0; i < AQ_BINS; i++)
		aq_sum += aq_hist[i] *= AQ_DECAY;
	aq_hist[b] += 1;
	aq_sum += 1;
	aq_thresholds();
	aq_ring[aq_ring_i] = d;
	aq_ring_i = (aq_ring_i + 1) % 16;
	if (aq_ring_n < 16)
		aq_ring_n++;
}

/* 1: the air got clearly worse over the last 15 minutes, -1: better */
static int aq_trend(void)
{
	double diff;

	if (aq_ring_n < 16)
		return 0;
	diff = aq_ring[(aq_ring_i + 15) % 16] - aq_ring[aq_ring_i];	/* newest - oldest */
	return diff > AQ_TREND ? 1 : diff < -AQ_TREND ? -1 : 0;
}

/* the word for now; a step is only taken 0.03 halving past it, so the word
   does not flicker when the air sits right at a step */
static int aq_classify(double d)
{
	int l = 0;

	while (l < 3 && d > aq_t[l])
		l++;
	if (aq_level >= 0 && l > aq_level && d < aq_t[l - 1] + 0.03)
		l = aq_level;
	else if (aq_level > 0 && l < aq_level && d > aq_t[aq_level - 1] - 0.03)
		l = aq_level;
	return aq_level = l;
}

/* version 2: d in halvings of resistance (1.13 kept % of the ADC voltage,
   which is not comparable: that history is dropped) */
static void aq_load(void)
{
	static char line[4096];
	FILE *f = fopen(AQ_FILE, "r");
	int n = 0, v2 = 0;

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "v=2", 3)) {
			v2 = 1;
		} else if (!strncmp(line, "last=", 5)) {
			for (int i = 0; i < 4; i++)
				if (!strncmp(line + 5, aq_word[i], strlen(aq_word[i])))
					aq_last = i;
		} else if (v2 && !strncmp(line, "base=", 5)) {
			char *p = line + 5, *end;

			for (int i = 0; i < 24; i++) {
				long h = strtol(p, &end, 10);

				if (end == p || *end != ':')
					break;
				base_hour[i] = h;
				base_max[i] = strtod(end + 1, &p);
			}
		} else if (v2 && !strncmp(line, "hist=", 5)) {
			char *p = line + 5, *end;

			for (n = 0; n < AQ_BINS; n++) {
				double v = strtod(p, &end);

				if (end == p || v < 0)
					break;
				aq_hist[n] = v;
				p = end;
			}
		}
	}
	fclose(f);
	if (n != AQ_BINS) {		/* damaged, older or missing: start again */
		memset(aq_hist, 0, sizeof(aq_hist));
		return;
	}
	aq_sum = 0;
	for (int i = 0; i < AQ_BINS; i++)
		aq_sum += aq_hist[i];
	aq_thresholds();
}

/* p4 is read-only in normal use: open it for the moment of writing */
static void aq_save(void)
{
	FILE *f;

	if (aq_sum <= 0 || system("mount -o remount,rw /mnt/data 2>/dev/null"))
		return;
	f = fopen(AQ_FILE ".new", "w");
	if (f) {
		fprintf(f, "v=2\n");
		if (aq_level >= 0 || aq_last >= 0)
			fprintf(f, "last=%s\n", aq_word[aq_level >= 0 ? aq_level : aq_last]);
		fprintf(f, "base=");
		for (int i = 0; i < 24; i++)
			fprintf(f, "%s%ld:%.4f", i ? " " : "", base_hour[i], base_max[i]);
		fprintf(f, "\nhist=");
		for (int i = 0; i < AQ_BINS; i++)
			fprintf(f, "%s%.3f", i ? " " : "", aq_hist[i]);
		fprintf(f, "\n");
		fclose(f);
		rename(AQ_FILE ".new", AQ_FILE);
	}
	if (system("sync; mount -o remount,ro /mnt/data 2>/dev/null")) { /* stays rw; the next writer closes it */ }
}

/* retoon/voc. index and pct_baseline: how far the resistance is below
   clean air, in %; d: the same in halvings; quality*: the word (above) */
static void publish_voc(int raw, double lr, double base, double d, const char *state,
			double tc, double rh, int have_trh)
{
	static long long sent;
	static char last_state[16];
	char *out = NULL;
	size_t out_len = 0;
	FILE *f;
	double pct = 100.0 * (1 - exp2(-d));

	if (!strcmp(state, last_state) && mono_ms() - sent < STATE_MIN_MS)
		return;
	sent = mono_ms();
	snprintf(last_state, sizeof(last_state), "%s", state);
	f = open_memstream(&out, &out_len);
	if (!f) return;
	fprintf(f, "raw=%d\nrs_kohm=%.0f\nstate=%s\n", raw, exp2(lr) / 1000, state);
	if (!strcmp(state, "ok")) {
		fprintf(f, "baseline_kohm=%.0f\nd=%.3f\nindex=%.0f\npct_baseline=%.1f\n",
			exp2(base) / 1000, d, pct, pct);
		fprintf(f, "quality=%s\nquality_learning=%d\nquality_trend=%d\n"
			"quality_steps=%.2f %.2f %.2f\nquality_learned_h=%.0f\n",
			aq_word[aq_classify(d)], aq_sum < AQ_LEARNED, aq_trend(),
			aq_t[0], aq_t[1], aq_t[2], aq_sum / 60);
	} else {
		fprintf(f, "index=0\n");
		if (aq_last >= 0)
			fprintf(f, "quality_last=%s\n", aq_word[aq_last]);
	}
	fprintf(f, "lr=%.4f\n", lr);
	if (have_trh)
		fprintf(f, "tempC=%.2f\nrh=%.1f\n", tc, rh);
	fprintf(f, "source=m4\n");
	fclose(f);
	bus_put(VOC_BUS, out);
	free(out);
}

static void publish_motion(volatile uint32_t *mb)
{
	static long long sent;
	static unsigned last_key = ~0u;
	unsigned mode = mb[OFF_US_MODE / 4];
	/* what matters at once: someone came or went, another mode */
	unsigned key = mb[OFF_US_WAKE / 4] << 8 | mb[OFF_US_PRESENT / 4] << 4 | mode;
	char *out = NULL;
	size_t out_len = 0;
	FILE *f;

	if (key == last_key && mono_ms() - sent < STATE_MIN_MS)
		return;
	sent = mono_ms();
	last_key = key;
	f = open_memstream(&out, &out_len);
	if (!f) return;
	fprintf(f, "wake=%u\nresid=%.3f\nactive=%.2f\nthresh=%.3f\nwakes=%u\n",
		mb[OFF_US_WAKE / 4], (int)mb[OFF_US_RESID / 4] / 1000.0,
		(int)mb[OFF_US_ACTIVE / 4] / 1000.0, (int)mb[OFF_US_THRESH / 4] / 1000.0,
		mb[OFF_US_WAKECNT / 4]);
	fprintf(f, "mode=%s\n", mode == 2 ? "pulsed" : mode == 1 ? "cw" : "off");
	if (mode == 2)
		fprintf(f, "range_cm=%u\nscore=%.3f\npresent=%u\nlearned=%.3f\ntx_pp=%u\nrelearns=%u\n",
			mb[OFF_US_RANGE / 4], (int)mb[OFF_US_SCORE / 4] / 1000.0,
			mb[OFF_US_PRESENT / 4], mb[OFF_US_LEARNED / 4] / 1000.0,
			mb[OFF_US_TX_PP / 4], mb[OFF_US_RELEARNS / 4]);
	else if (mode == 1)
		fprintf(f, "carrier=%.3f\n", (int)mb[OFF_US_CARRIER / 4] / 1000.0);
	fprintf(f, "source=m4\n");
	fclose(f);
	bus_put(MOTION_BUS, out);
	free(out);
}

/* does Linux still own PWM2 (a pwmchip for 2084000.pwm)? If not, the
   device tree handed the peripherals to the M4 and it must set them up. */
static int linux_has_pwm(const char *node)
{
	DIR *d = opendir("/sys/class/pwm");
	struct dirent *de;
	int found = 0;
	if (!d) return 0;
	while ((de = readdir(d))) {
		char path[300], real[300];
		if (strncmp(de->d_name, "pwmchip", 7)) continue;
		snprintf(path, sizeof(path), "/sys/class/pwm/%s/device", de->d_name);
		ssize_t n = readlink(path, real, sizeof(real) - 1);
		if (n > 0) { real[n] = 0; if (strstr(real, node)) found = 1; }
	}
	closedir(d);
	return found;
}

/* open all /dev/input/event* for touch detection (any EV_ABS/EV_KEY) */
static int open_touch_fds(int *fds, int max)
{
	DIR *d = opendir("/dev/input");
	struct dirent *de;
	int n = 0;
	if (!d) return 0;
	while (n < max && (de = readdir(d))) {
		char path[300];
		if (strncmp(de->d_name, "event", 5)) continue;
		snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
		int fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd >= 0) fds[n++] = fd;
	}
	closedir(d);
	return n;
}

static int poll_touch(int *fds, int n)
{
	struct input_event ev;
	int touched = 0;
	for (int i = 0; i < n; i++) {
		while (read(fds[i], &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
			if ((ev.type == EV_ABS) ||
			    (ev.type == EV_KEY && ev.code == BTN_TOUCH && ev.value))
				touched = 1;
		}
	}
	return touched;
}

/* ---- supervision -------------------------------------------------------
 * The M4 is watched through its heartbeat and its fault report. When it stops
 * (stuck loop, HardFault, or stopped from outside) it is restarted, with a
 * growing pause between attempts, and the state goes to retoon/m4.
 *
 * Never touch the mailbox while the M4 is stopped: remoteproc's stop clears
 * ENABLE_M4, and an A9 access to the TCM then hangs the whole bus. So the
 * mailbox is only read after checking the remoteproc state, and restart()
 * does stop + start without reading anything in between. */

static time_t mono(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec;
}

static int write_str(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY), ok;
	if (fd < 0) return -1;
	ok = write(fd, val, strlen(val)) == (ssize_t)strlen(val);
	close(fd);
	return ok ? 0 : -1;
}

static int rproc_present(void) { return access(RPROC "/state", F_OK) == 0; }

static int rproc_running(void)
{
	char buf[32] = "";
	FILE *f = fopen(RPROC "/state", "r");
	if (!f) return 0;
	if (!fgets(buf, sizeof(buf), f)) buf[0] = 0;
	fclose(f);
	return !strncmp(buf, "running", 7);
}

/* may the A9 read the mailbox now? */
static int m4_accessible(void)
{
	return !rproc_present() || rproc_running();
}

static int m4_restart(void)
{
	if (rproc_present()) {
		write_str(RPROC "/state", "stop");	/* fails when already offline */
		if (write_str(RPROC "/state", "start"))
			return -1;
		return rproc_running() ? 0 : -1;
	}
	/* m4load keeps ENABLE_M4 set while it holds and loads the core */
	return system("/usr/bin/m4load " M4_FIRMWARE " >/dev/null 2>&1") == 0 ? 0 : -1;
}

/* after a (re)start: alive = magic present and the heartbeat moving */
static int m4_wait_alive(volatile uint32_t *mb)
{
	uint32_t hb = 0;
	for (int i = 0; i < 30; i++) {
		usleep(100000);
		if (!m4_accessible()) continue;
		if (mb[OFF_MAGIC / 4] != M4_MAGIC) continue;
		if (hb && mb[OFF_HEARTBEAT / 4] != hb) return 0;
		hb = mb[OFF_HEARTBEAT / 4];
	}
	return -1;
}

static struct {
	const char *state;	/* running, restarting, waiting */
	int sound;		/* the M4 runs the buzzer DAC (CTRL_SND_EN) */
	unsigned restarts;
	char last[64];		/* "18:02 stopped responding" */
	char fault[128];
	unsigned fw;
	int retry_in;
} sup;

static void publish_m4(void)
{
	char *out = NULL;
	size_t out_len = 0;
	FILE *f = open_memstream(&out, &out_len);
	char text[200];
	if (!f) return;
	if (!strcmp(sup.state, "running")) {
		int n = snprintf(text, sizeof(text), "Running (firmware %u.%u)",
				 sup.fw >> 16, (sup.fw >> 8) & 0xff);
		if (sup.restarts)
			snprintf(text + n, sizeof(text) - n, ", restarted %u time%s, last %s",
				 sup.restarts, sup.restarts == 1 ? "" : "s", sup.last);
	} else if (!strcmp(sup.state, "waiting")) {
		snprintf(text, sizeof(text), "Not running, next try in %d s (%s)",
			 sup.retry_in, sup.last);
	} else {
		snprintf(text, sizeof(text), "Starting");
	}
	fprintf(f, "state=%s\ntext=%s\nrestarts=%u\nlast=%s\nfault=%s\nfirmware=%u.%u.%u\nsound=%s\n",
		sup.state, text, sup.restarts, sup.last, sup.fault,
		sup.fw >> 16, (sup.fw >> 8) & 0xff, sup.fw & 0xff,
		!strcmp(sup.state, "running") && sup.sound ? "on" : "off");
	fclose(f);
	bus_put(M4_BUS, out);
	free(out);
}

static void note(const char *why)
{
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	snprintf(sup.last, sizeof(sup.last), "%02d:%02d %s", tm.tm_hour, tm.tm_min, why);
}

int main(int argc, char **argv)
{
	volatile uint32_t *mb;
	double smooth = 0, tc = 0, rh = 0;
	time_t start, hb_t, up_since, retry_at = 0;
	uint32_t last_voc_seq = 0, last_us_seq = 0, hb_last = 0, ctrl;
	int touch_fds[16], n_touch, have_trh = 0, backoff = 0, alive = 0;
	/* Defaults since 1.9: continuous-wave detector, motion published only
	   (retoon/motion, MQTT); the transducer only sees a hand at the
	   bottom edge of a cased unit, so the screen wake is opt-in.
	   --wake-screen: an approach un-dims the screen through toon-ui;
	   --pulsed: pulsed ranging (not usable in a case: the still-room profile
	   is learned while the receiver still settles and then never matches);
	   --learn: let touches adjust the threshold. --no-wake-screen and --cw
	   are accepted for old configurations.
	   Sound (since 1.12): the M4 drives the buzzer as a DAC (toon-sound)
	   whenever Linux does not hold PWM4; --no-sound leaves it alone. */
	int wake_screen = 0, cw = 1, learn = 0, sound = 1;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--wake-screen")) wake_screen = 1;
		else if (!strcmp(argv[i], "--no-wake-screen")) wake_screen = 0;
		else if (!strcmp(argv[i], "--pulsed")) cw = 0;
		else if (!strcmp(argv[i], "--cw")) cw = 1;
		else if (!strcmp(argv[i], "--learn")) learn = 1;
		else if (!strcmp(argv[i], "--no-sound")) sound = 0;
	}
	int last_wake = -1;
	time_t wake_sent = 0;
	double aq_acc = 0;
	int aq_n = 0;
	time_t aq_min_t = 0, aq_saved = 0;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	bus_open("m4d");
	bus_watch(CLIMATE_BUS);
	aq_load();
	openlog("toon-m4d", LOG_PID, LOG_DAEMON);
	sup.state = "restarting";
	publish_m4();

	ctrl = CTRL_VOC_EN | CTRL_US_EN | (cw ? 0 : CTRL_US_PULSED) | (learn ? CTRL_LEARN : 0);
	if (!linux_has_pwm("2084000.pwm")) ctrl |= CTRL_OWN_PERIPH;
	if (sound && !linux_has_pwm("208c000.pwm")) ctrl |= CTRL_SND_EN;
	mb = map_mailbox();

	/* normally the init script has just started the M4 (waiting for its
	   settings); if not, or if it already runs with other settings (the
	   firmware reads them once), (re)start it here */
	if (!m4_accessible() || mb[OFF_MAGIC / 4] != M4_MAGIC) {
		syslog(LOG_NOTICE, "M4 not running, starting it");
		m4_restart();
	} else if (mb[OFF_A9_CTRL / 4] && mb[OFF_A9_CTRL / 4] != ctrl) {
		syslog(LOG_NOTICE, "M4 runs with ctrl=0x%x, restarting it for 0x%x",
		       mb[OFF_A9_CTRL / 4], ctrl);
		m4_restart();
	}
	if (m4_wait_alive(mb) == 0) {
		alive = 1;
		sup.fw = mb[OFF_FW_VERSION / 4];
		mb[OFF_A9_CTRL / 4] = ctrl;
		sup.state = "running";
		sup.sound = !!(ctrl & CTRL_SND_EN);
		syslog(LOG_INFO, "M4 firmware %u.%u.%u enabled, ctrl=0x%x (%s%s%s%s)",
		       sup.fw >> 16, (sup.fw >> 8) & 0xff, sup.fw & 0xff, ctrl,
		       cw ? "cw" : "pulsed", ctrl & CTRL_OWN_PERIPH ? ", M4 owns peripherals" : "",
		       learn ? ", learning" : "", ctrl & CTRL_SND_EN ? ", sound" : "");
	} else {
		note("did not start");
		syslog(LOG_ERR, "M4 did not start");
		sup.state = "waiting";
		retry_at = mono() + (backoff = 5);
	}
	publish_m4();

	n_touch = open_touch_fds(touch_fds, 16);
	start = up_since = hb_t = mono();

	while (!stop) {
		time_t now = mono();
		const char *why = NULL;

		/* ---- supervision ---- */
		if (!alive) {
			if (now < retry_at) {
				sup.retry_in = (int)(retry_at - now);
				publish_m4();
				sleep(1);
				continue;
			}
			why = "did not start";
		} else if (!m4_accessible()) {
			why = "was stopped";
		} else if (mb[OFF_FAULT / 4] == M4_FAULT_FLAG) {
			volatile uint32_t *f = mb + OFF_FAULT / 4;
			snprintf(sup.fault, sizeof(sup.fault),
				 "cfsr=%08x hfsr=%08x bfar=%08x pc=%08x lr=%08x",
				 f[1], f[2], f[3], f[4], f[5]);
			syslog(LOG_ERR, "M4 HardFault: %s", sup.fault);
			why = "crashed";
		} else {
			uint32_t hb = mb[OFF_HEARTBEAT / 4];
			if (hb != hb_last) { hb_last = hb; hb_t = now; }
			else if (now - hb_t >= STALL_S) why = "stopped responding";
		}
		if (why) {
			if (alive) {
				note(why);
				syslog(LOG_WARNING, "M4 %s, restarting", why);
				sup.restarts++;
			}
			sup.state = "restarting";
			publish_m4();
			if (m4_restart() == 0 && m4_wait_alive(mb) == 0) {
				sup.fw = mb[OFF_FW_VERSION / 4];
				mb[OFF_A9_CTRL / 4] = ctrl;
				alive = 1;
				sup.state = "running";
				hb_last = mb[OFF_HEARTBEAT / 4];
				hb_t = up_since = mono();
				last_voc_seq = last_us_seq = 0;
				syslog(LOG_INFO, "M4 running again");
			} else {
				alive = 0;
				backoff = backoff ? backoff * 2 : 5;
				if (backoff > BACKOFF_MAX) backoff = BACKOFF_MAX;
				retry_at = mono() + backoff;
				sup.state = "waiting";
				syslog(LOG_ERR, "M4 restart failed, next try in %d s", backoff);
			}
			publish_m4();
			continue;
		}
		if (backoff && now - up_since > HEALTHY_S)
			backoff = 0;


		/* ---- feed touch labels to the M4 ---- */
		if (poll_touch(touch_fds, n_touch)) {
			mb[OFF_TOUCH_MS / 4] = mb[OFF_HEARTBEAT / 4];
			mb[OFF_TOUCH_SEQ / 4] = mb[OFF_TOUCH_SEQ / 4] + 1;
		}

		/* ---- VOC: sensor resistance against the clean air of the last day ---- */
		uint32_t vs = mb[OFF_VOC_SEQ / 4];
		if (vs != last_voc_seq) {
			int raw = (int)mb[OFF_VOC_RAW / 4];
			double lr, base, d;
			time_t wall = time(NULL);

			last_voc_seq = vs;
			smooth = smooth ? smooth + (raw - smooth) * 0.3 : raw;
			have_trh = read_temp_rh(&tc, &rh);
			lr = compensate(sensor_log2r(smooth), tc, rh, have_trh);
			if (now - start < WARMUP_S) {	/* the heater settles first */
				publish_voc(raw, lr, lr, 0, "warmup", tc, rh, have_trh);
			} else {
				base_add(lr, wall);
				if (!base_get(wall, &base))
					base = lr;
				d = base - lr;
				if (d < 0)
					d = 0;
				aq_acc += d;
				aq_n++;
				if (!aq_min_t)
					aq_min_t = aq_saved = now;
				if (now - aq_min_t >= 60) {
					aq_minute(aq_acc / aq_n);
					aq_acc = 0;
					aq_n = 0;
					aq_min_t = now;
					if (now - aq_saved >= AQ_SAVE_S) {
						aq_save();
						aq_saved = now;
					}
				}
				publish_voc(raw, lr, base, d, "ok", tc, rh, have_trh);
			}
		}

		/* ---- motion/wake ---- */
		uint32_t us = mb[OFF_US_SEQ / 4];
		if (us != last_us_seq) {
			last_us_seq = us;
			int wake = (int)mb[OFF_US_WAKE / 4];
			publish_motion(mb);
			/* as if the screen was touched: toon-ui un-dims and its own
			   inactivity timer dims it again; never drive the backlight
			   from here (it would fight the UI). Repeated every 3 s while
			   someone is still detected, so the screen stays on for as
			   long as they are there and dims on the UI's timer after. */
			if (wake && wake_screen && (wake != last_wake || now - wake_sent >= 3)) {
				bus_send(UI_CMD, "wake");
				wake_sent = now;
			}
			last_wake = wake;
		}

		usleep(200000);		/* ~5 Hz poll */
	}
	aq_save();
	bus_put(VOC_BUS, NULL);
	bus_put(MOTION_BUS, NULL);
	bus_put(M4_BUS, NULL);
	bus_close();
	return 0;
}
