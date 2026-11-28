/*
 * toon-sound: play sounds on a ReToon, the same way on both Toons.
 *
 *   toon-sound play FILE.wav    PCM WAV, 8 or 16 bit, mono or stereo, any rate
 *   toon-sound tone HZ MS       a tone
 *   toon-sound beep             a short beep
 *   toon-sound test             a three-note chime
 *   -v N                        volume 0..100 (default: volume= in
 *                               /mnt/data/toon-os/config/sound.conf, else
 *                               30 on a Toon 1, 70 on a Toon 2: about the
 *                               same loudness)
 *
 * Toon 1: the TLV320AIC3111 codec through ALSA (aplay, 48 kHz).
 * Toon 2: the buzzer, driven by the Cortex-M4 as a DAC (16 kHz, 8 bit): the
 * samples go into the M4's ring buffer in TCMU (see toon-m4's shared.h).
 * One sound at a time: a second call waits for the first.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define CONF		"/mnt/data/toon-os/config/sound.conf"
#define LOCKFILE	"/run/toon/sound.lock"

/* Toon 2: M4 mailbox (shared.h) */
#define M4_A9ADDR	0x00800000u
#define M4_MAPLEN	0x2000u
#define OFF_MAGIC	0x00
#define OFF_FW		0x04
#define OFF_SND_WR	0xb4
#define OFF_SND_RD	0xb8
#define OFF_SND_RATE	0xc0
#define M4_MAGIC	0xB007C0DEu
#define SND_RING_OFF	0x400u
#define SND_SIZE	4096u

static int toon1;
static int out_rate;
static float volume = 0.7f;

static void die(const char *fmt, const char *arg)
{
	fprintf(stderr, "toon-sound: ");
	fprintf(stderr, fmt, arg);
	fputc('\n', stderr);
	exit(1);
}

/* ---------- output: a stream of mono float samples at out_rate ---------- */
static FILE *alsa;			/* Toon 1 */
static volatile uint32_t *mb;		/* Toon 2 */
static volatile uint8_t *ring;
static uint32_t wr;
static float level;			/* Toon 2: DC level 0..1 (ramps in/out) */

static void out_open(void)
{
	if (toon1) {
		char cmd[128];
		snprintf(cmd, sizeof(cmd), "aplay -q -D hw:0,0 -B 500000 -t raw -f S16_LE -r %d -c 2", out_rate);
		if (!(alsa = popen(cmd, "w")))
			die("cannot start %s", "aplay");
		return;
	}
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0)
		die("cannot open %s", "/dev/mem");
	void *p = mmap(NULL, M4_MAPLEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, M4_A9ADDR);
	close(fd);
	if (p == MAP_FAILED)
		die("cannot map the %s mailbox", "M4");
	mb = p;
	if (mb[OFF_MAGIC / 4] != M4_MAGIC || mb[OFF_FW / 4] < 0x00020400u || !mb[OFF_SND_RATE / 4])
		die("the co-processor does not run sound (%s)", "toon-m4d, firmware 2.4 or later");
	ring = (volatile uint8_t *)p + SND_RING_OFF;
	wr = mb[OFF_SND_WR / 4];
	if (wr != mb[OFF_SND_RD / 4])	/* a killed player left samples: drop them */
		wr = mb[OFF_SND_RD / 4];
}

static void m4_put(uint8_t v)
{
	while (wr - mb[OFF_SND_RD / 4] >= SND_SIZE - 1) {
		mb[OFF_SND_WR / 4] = wr;
		usleep(10000);
	}
	ring[wr % SND_SIZE] = v;
	wr++;
	if (!(wr & 255))
		mb[OFF_SND_WR / 4] = wr;
}

static int vol_q8;			/* volume * 256, for the integer path */


/* one sample, -32768..32767 (integer: the Toon 1 has no FPU) */
static void out_sample_i(int v)
{
	v = v * vol_q8 >> 8;
	if (v > 32767) v = 32767;
	if (v < -32768) v = -32768;
	if (toon1) {
		int16_t f[2] = { (int16_t)v, (int16_t)v };
		fwrite(f, sizeof(f), 1, alsa);
		return;
	}
	m4_put((uint8_t)(level * (128 + v / 258)));
}

static void out_sample(float x)
{
	x *= volume;
	if (x > 1) x = 1;
	if (x < -1) x = -1;
	if (toon1) {
		int16_t s = (int16_t)(x * 32000);
		int16_t f[2] = { s, s };
		fwrite(f, sizeof(f), 1, alsa);
		return;
	}
	/* Toon 2: the buzzer sits at 0 % duty when idle, so the middle (50 %)
	   is reached with a ramp, then the sound swings around it */
	m4_put((uint8_t)(level * (128 + x * 127)));
}

static void out_ramp(int up)
{
	if (toon1)
		return;
	int n = out_rate / 50;		/* 20 ms */
	for (int i = 0; i < n; i++) {
		level = up ? (float)(i + 1) / n : 1.0f - (float)(i + 1) / n;
		m4_put((uint8_t)(level * 128));
	}
	level = up ? 1.0f : 0.0f;
}

static void out_close(void)
{
	if (toon1) {
		for (int i = 0; i < out_rate / 20; i++)	/* 50 ms silence so the end is heard */
			out_sample(0);
		pclose(alsa);
		return;
	}
	mb[OFF_SND_WR / 4] = wr;
	for (int i = 0; i < 300 && mb[OFF_SND_RD / 4] != wr; i++)
		usleep(10000);
}

/* ---------- sounds ---------- */
static void tone(float hz, int ms, int square)
{
	int n = out_rate * ms / 1000;
	int att = n > out_rate / 10 ? out_rate * 15 / 1000 : out_rate / 200;	/* 15 ms, short tones 5 ms */
	int rel = n > out_rate / 10 ? out_rate * 40 / 1000 : out_rate / 200;	/* 40 ms, short tones 5 ms */
	/* sine by rotation: no sinf() per sample (the Toon 1 has no FPU) */
	float w = 2 * (float)M_PI * hz / out_rate, c = cosf(w), sn = sinf(w), re = 1, im = 0;
	for (int i = 0; i < n; i++) {
		float x = im, t = re * c - im * sn;
		im = re * sn + im * c;
		re = t;
		if (!(i & 1023)) {	/* keep the amplitude at 1 */
			float a = 1.5f - 0.5f * (re * re + im * im);
			re *= a; im *= a;
		}
		if (square)
			x = x > 0 ? 0.8f : -0.8f;
		float env = 1;
		if (i < att) env = (float)i / att;
		if (n - i < rel) env = (float)(n - i) / rel;
		out_sample(x * env);
	}
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }

static void play_wav(const char *path)
{
	FILE *f = fopen(path, "rb");
	uint8_t h[12], ch[8];
	int fmt = 0, chans = 0, bits = 0, rate = 0;
	uint32_t datalen = 0;

	if (!f)
		die("cannot open %s", path);
	if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
		die("%s is not a WAV file", path);
	while (fread(ch, 1, 8, f) == 8) {
		uint32_t len = rd32(ch + 4);
		if (!memcmp(ch, "fmt ", 4)) {
			uint8_t b[40];
			if (len < 16 || len > sizeof(b) || fread(b, 1, len, f) != len)
				die("%s: bad format block", path);
			fmt = rd16(b); chans = rd16(b + 2); rate = rd32(b + 4); bits = rd16(b + 14);
			if (len & 1) fgetc(f);
		} else if (!memcmp(ch, "data", 4)) {
			datalen = len;
			break;
		} else {
			fseek(f, len + (len & 1), SEEK_CUR);
		}
	}
	if (fmt != 1 || (bits != 8 && bits != 16) || chans < 1 || chans > 2 || rate < 4000 || rate > 96000 || !datalen)
		die("%s: only PCM WAV, 8 or 16 bit, mono or stereo", path);

	int frame = chans * bits / 8;
	uint8_t buf[4096];
	/* linear resampling to out_rate in 16.16 fixed point */
	uint32_t pos = 0, step = (uint32_t)(((uint64_t)rate << 16) / out_rate);
	int prev = 0, cur = 0, have = 0;
	uint32_t left = datalen;
	size_t n = 0, i = 0;

	for (;;) {
		while (pos >= 65536 || !have) {
			if (i + frame > n) {
				size_t want = left < sizeof(buf) ? left : sizeof(buf);
				want -= want % frame;
				if (!want || (n = fread(buf, 1, want, f)) < (size_t)frame)
					goto done;
				left -= n;
				i = 0;
			}
			int v = 0;
			for (int c = 0; c < chans; c++)
				v += bits == 16 ? (int16_t)rd16(buf + i + c * 2) : (buf[i + c] - 128) << 8;
			i += frame;
			prev = cur;
			cur = v / chans;
			if (have) pos -= 65536;
			have = 1;
		}
		out_sample_i(prev + (int)(((int64_t)(cur - prev) * pos) >> 16));
		pos += step;
	}
done:
	fclose(f);
}

static float conf_volume(void)
{
	FILE *f = fopen(CONF, "r");
	char line[64];
	int v = toon1 ? 30 : 70;	/* the Toon 1's speaker is much louder than the buzzer */
	if (f) {
		while (fgets(line, sizeof(line), f))
			if (!strncmp(line, "volume=", 7))
				v = atoi(line + 7);
		fclose(f);
	}
	if (v < 0) v = 0;
	if (v > 100) v = 100;
	return v / 100.0f;
}

int main(int argc, char **argv)
{
	FILE *m;
	char model[64] = "";
	int a = 1;

	if ((m = fopen("/proc/device-tree/model", "r"))) {
		if (!fgets(model, sizeof(model), m)) model[0] = 0;
		fclose(m);
	}
	toon1 = strstr(model, "Toon 1") != NULL;
	out_rate = toon1 ? 48000 : 16000;
	volume = conf_volume();
	if (a + 1 < argc && !strcmp(argv[a], "-v")) {
		volume = atoi(argv[a + 1]) / 100.0f;
		if (volume < 0) volume = 0;
		if (volume > 1) volume = 1;
		a += 2;
	}
	vol_q8 = (int)(volume * 256);
	if (a >= argc) {
		fprintf(stderr, "usage: toon-sound [-v 0..100] play FILE.wav | tone HZ MS | beep | test\n");
		return 2;
	}
	/* one sound at a time; the next waits */
	mkdir("/run/toon", 0755);
	int lk = open(LOCKFILE, O_CREAT | O_RDWR, 0644);
	if (lk >= 0)
		flock(lk, LOCK_EX);
	signal(SIGPIPE, SIG_IGN);

	if (!strcmp(argv[a], "play") && a + 1 < argc) {
		FILE *t = fopen(argv[a + 1], "rb");
		if (!t) die("cannot open %s", argv[a + 1]);
		fclose(t);
		out_open(); out_ramp(1); play_wav(argv[a + 1]);
	} else if (!strcmp(argv[a], "tone") && a + 2 < argc) {
		out_open(); out_ramp(1); tone(atof(argv[a + 1]), atoi(argv[a + 2]), 0);
	} else if (!strcmp(argv[a], "beep")) {
		/* at the buzzer's resonance; the same soft beep on the Toon 1 */
		out_open(); out_ramp(1); tone(2400, 80, 0);
	} else if (!strcmp(argv[a], "test")) {
		/* C-E-G; on the Toon 2 two octaves up, where the buzzer is loud */
		float t = toon1 ? 1 : 4;
		out_open(); out_ramp(1);
		tone(523.25f * t, 180, 0); tone(659.25f * t, 180, 0); tone(783.99f * t, 360, 0);
	} else {
		fprintf(stderr, "usage: toon-sound [-v 0..100] play FILE.wav | tone HZ MS | beep | test\n");
		return 2;
	}
	out_ramp(0);
	out_close();
	return 0;
}
