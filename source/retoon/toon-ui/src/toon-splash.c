// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * toon-splash: what the panel shows while the system starts - the name on a
 * dark blue backdrop and a progress bar, drawn straight on the framebuffer.
 * Started as the first init script; S50toon-ui ends it (SIGTERM), on which
 * the bar runs full.
 *
 * The bar follows the start itself: the init scripts up to the screen
 * program are counted, and the one that runs (seen in /proc) says how far
 * the start is. Within a script it creeps on by time, never past the next
 * one, and what is shown moves smoothly towards that.
 *
 *   toon-splash                     on the framebuffer, until SIGTERM
 *   toon-splash --still [BMP]       the picture once and done (the initrd,
 *                                   right after the kernel took the screen):
 *                                   from U-Boot's BMP (a file, or the Toon
 *                                   1's flash partition) when this program
 *                                   made it, else nothing; without BMP,
 *                                   worked out. With the empty bar, which
 *                                   U-Boot's picture does not have
 *   toon-splash --stop              the picture with a bar going to and fro,
 *                                   while the system stops (S50toon-ui stop),
 *                                   until it is ended; U-Boot shows the same
 *                                   picture next
 *   toon-splash --bmp FILE [WxH]    the opening picture (no bar) as a 24-bit
 *                                   BMP, for U-Boot (toon-bootsplash); the
 *                                   panel's size unless WxH is given
 *   toon-splash --frame FILE WxH P  a frame with the bar at P (0..1) as PPM,
 *                                   to look at the design on a PC
 */
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "splash-logo.h"

#define C_TEXT		0xeef2f7	/* "Toon" */
#define C_ACCENT	0x5aa9ff	/* "Re" */
#define C_BG_IN		0x162131	/* the backdrop: lighter in the middle */
#define C_BG_OUT	0x05070b
#define C_GLOW		0x3d8bfd	/* a faint light behind the name */
#define C_TRACK		0x243044
#define C_FILL_A	0x3d8bfd	/* the bar, left to right */
#define C_FILL_B	0x6cc8ff
#define END_SCRIPT	"toon-ui"	/* the script whose program ends the splash */
#define BMP_MARK	"RTsp"		/* in the BMP header's unused bytes: made here */

static int W, H;
static uint32_t *scene;		/* the backdrop with the name, 0xRRGGBB */

/* ---- colours ---------------------------------------------------------- */

static uint32_t mix(uint32_t a, uint32_t b, int t)	/* t: 0..255 of b */
{
	uint32_t r = 0;

	if (t <= 0)
		return a;
	if (t >= 255)
		return b;
	for (int s = 0; s < 24; s += 8)
		r |= ((((a >> s) & 255) * (255 - t) + ((b >> s) & 255) * t + 127) / 255) << s;
	return r;
}

/* ---- the picture -------------------------------------------------------- */

static void geometry(int *lx, int *ly, int *bx, int *by, int *bw, int *bh)
{
	*lx = (W - LOGO_W) / 2;
	*ly = H * 44 / 100 - LOGO_H / 2;
	*bw = W * 24 / 100;
	*bh = H >= 600 ? 4 : 3;
	*bx = (W - *bw) / 2;
	*by = *ly + LOGO_H + H * 9 / 100;
}

/*
 * Backdrop + name, once. The Toon 1 has no floating point unit, so the
 * backdrop is worked out per pixel with integer squared distances looked up
 * in two small tables: the gradient (lighter in the middle) and the glow.
 */
#define LUT	4096
static void make_scene(void)
{
	int lx, ly, bx, by, bw, bh, cx = W / 2, cy;
	const unsigned char *p = logo_rle;
	static uint8_t bg_t[LUT], glow_t[LUT];
	int64_t bg_max, glow_max, s = W * 22 / 100;

	geometry(&lx, &ly, &bx, &by, &bw, &bh);
	cy = ly + LOGO_H / 2;
	scene = malloc((size_t)W * H * 4);
	if (!scene)
		exit(1);
	/* gradient over the distance from the name (wider than high), out to
	   the corners; the glow fades out by 2 x its size */
	bg_max = (int64_t)W * W * 7225 / 10000 + (int64_t)H * H + 1;
	glow_max = 4 * s * s;
	for (int i = 0; i < LUT; i++) {
		double d = sqrt((double)i / LUT * bg_max) / hypot(W / 2.0, H / 2.0);

		bg_t[i] = (uint8_t)(fmin(1, d * 1.15) * 255);
		glow_t[i] = (uint8_t)(exp(-(double)i / LUT * glow_max / ((double)s * s)) * 26);
	}
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			int64_t dx = x - cx, dy = y - cy;
			int64_t qb = dx * dx * 7225 / 10000 + dy * dy, qg = dx * dx + dy * dy * 324 / 100;
			uint32_t c = mix(C_BG_IN, C_BG_OUT, bg_t[qb * LUT / bg_max]);

			scene[y * W + x] = qg < glow_max ? mix(c, C_GLOW, glow_t[qg * LUT / glow_max]) : c;
		}
	/* the name: run-length coded coverage, "Re" in the accent colour */
	for (int x = 0, y = 0; y < LOGO_H && p < logo_rle + sizeof(logo_rle); p += 2)
		for (int n = p[0]; n; n--) {
			if (p[1] & 127 && lx + x >= 0 && lx + x < W && ly + y >= 0 && ly + y < H) {
				uint32_t *d = &scene[(ly + y) * W + lx + x];

				*d = mix(*d, p[1] & 128 ? C_ACCENT : C_TEXT, (p[1] & 127) * 2 + 1);
			}
			if (++x == LOGO_W) {
				x = 0;
				y++;
			}
		}
}

/* a pixel of the bar over the scene: filled from "lo" to "hi" (0..1), with
   a soft light moving along the filled part at "shine" (0..1 of its sweep;
   <0: none) */
static uint32_t bar_pixel(int x, int y, int bx, int by, int bw, int bh, double lo, double hi,
			  double shine)
{
	double r = bh / 2.0, px = x - bx + 0.5, py = y - by + 0.5;
	double ex = px < r ? r : px > bw - r ? bw - r : px;
	double dist = hypot(px - ex, py - r) - r;
	int cover = dist < -0.5 ? 255 : dist > 0.5 ? 0 : (int)((0.5 - dist) * 255);
	uint32_t base = scene[y * W + x], c;
	double from = lo * bw, fill = hi * bw;

	if (!cover)
		return base;
	if (hi > lo && px >= from - 0.5 && px <= fill + 0.5) {
		c = mix(C_FILL_A, C_FILL_B, (int)(px / bw * 255));
		if (shine >= 0) {
			double sx = from + shine * (fill - from + 60) - 30, k = exp(-pow((px - sx) / 22, 2));

			c = mix(c, 0xffffff, (int)(k * 110));
		}
		if (fill + 0.5 - px < 1)	/* soft ends */
			c = mix(C_TRACK, c, (int)((fill + 0.5 - px) * 255));
		if (from > 0 && px - (from - 0.5) < 1)
			c = mix(C_TRACK, c, (int)((px - from + 0.5) * 255));
	} else {
		c = C_TRACK;
	}
	return mix(base, c, cover);
}

/* ---- output: framebuffer ------------------------------------------------- */

static uint8_t *fb;
static int stride, bpp;

static const uint8_t bayer[4][4] = {
	{ 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 }
};

/* RGB565 with an ordered dither, so the backdrop has no bands (Toon 1) */
static uint16_t to565(int x, int y, uint32_t c)
{
	int t = bayer[y & 3][x & 3];
	int r = ((c >> 16) & 255) + t / 2, g = ((c >> 8) & 255) + t / 4, b = (c & 255) + t / 2;

	r = r > 255 ? 255 : r;
	g = g > 255 ? 255 : g;
	b = b > 255 ? 255 : b;
	return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}

static void put(int x, int y, uint32_t c)
{
	if (bpp == 16) {
		uint16_t v = to565(x, y, c);

		memcpy(fb + y * stride + x * 2, &v, 2);
	} else {
		memcpy(fb + y * stride + x * 4, &c, 4);
	}
}

/* a pixel on the screen, as 0xRRGGBB */
static uint32_t get(int x, int y)
{
	if (bpp == 16) {
		uint16_t v;

		memcpy(&v, fb + y * stride + x * 2, 2);
		return (uint32_t)((v >> 11) * 255 / 31) << 16 | (uint32_t)(((v >> 5) & 63) * 255 / 63) << 8 |
		       (uint32_t)((v & 31) * 255 / 31);
	}
	uint32_t c;

	memcpy(&c, fb + y * stride + x * 4, 4);
	return c & 0xffffff;
}

/* is the picture already on the screen (from the initrd, or U-Boot's)?
   A few points of the name and the backdrop, with room for dithering */
static int scene_shown(void)
{
	int lx, ly, bx, by, bw, bh, hit = 0, n = 0;

	geometry(&lx, &ly, &bx, &by, &bw, &bh);
	for (int i = 1; i < 8; i++)
		for (int j = 1; j < 4; j++) {
			int x = lx + LOGO_W * i / 8, y = ly + LOGO_H * j / 4;
			uint32_t a = get(x, y), b = scene[y * W + x];
			int d = 0;

			for (int k = 0; k < 24; k += 8)
				d += abs((int)((a >> k) & 255) - (int)((b >> k) & 255));
			hit += d < 40;
			n++;
		}
	return hit * 10 >= n * 9;
}

/* the whole scene onto the screen */
static void show_scene(void)
{
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			put(x, y, scene[y * W + x]);
}

/* the picture from a 24-bit BMP of the panel's size that this program made
   (U-Boot's, with BMP_MARK), instead of working it out: quicker in the
   initrd, and never the original software's picture. 0 when it was used. */
static int load_bmp(const char *path)
{
	FILE *f = fopen(path, "rb");
	uint8_t h[54], *row;
	int32_t w = 0, ht = 0;
	uint32_t off, rs = (W * 3 + 3) & ~3u;
	int ok = 0;

	if (!f)
		return 1;
	if (fread(h, 1, 54, f) == 54 && h[0] == 'B' && h[1] == 'M' && h[28] == 24 &&
	    !memcmp(h + 6, BMP_MARK, 4)) {
		memcpy(&off, h + 10, 4);
		memcpy(&w, h + 18, 4);
		memcpy(&ht, h + 22, 4);
		ok = w == W && (ht == H || ht == -H) && !fseek(f, off, SEEK_SET);
	}
	row = ok ? malloc(rs) : NULL;
	scene = ok ? malloc((size_t)W * H * 4) : NULL;
	for (int i = 0; ok && i < H; i++) {
		int y = ht > 0 ? H - 1 - i : i;	/* bottom row first, unless the height is negative */

		ok = row && scene && fread(row, 1, rs, f) == rs;
		for (int x = 0; ok && x < W; x++)
			scene[y * W + x] = (uint32_t)row[x * 3 + 2] << 16 | (uint32_t)row[x * 3 + 1] << 8 | row[x * 3];
	}
	free(row);
	fclose(f);
	if (!ok) {
		free(scene);
		scene = NULL;
	}
	return !ok;
}

/* ---- where the start is --------------------------------------------------- */

static char scripts[64][32];
static int nscripts;

static int by_name(const void *a, const void *b) { return strcmp(a, b); }

/* the init scripts up to the one that starts the screen program */
static void list_scripts(void)
{
	DIR *d = opendir("/etc/init.d");
	struct dirent *e;

	while (d && (e = readdir(d)) && nscripts < 64)
		if (e->d_name[0] == 'S' && isdigit((unsigned char)e->d_name[1]) &&
		    isdigit((unsigned char)e->d_name[2]) && strlen(e->d_name) < 32)
			strcpy(scripts[nscripts++], e->d_name);
	if (d)
		closedir(d);
	qsort(scripts, nscripts, sizeof(scripts[0]), by_name);
	for (int i = 0; i < nscripts; i++)
		if (strstr(scripts[i], END_SCRIPT)) {
			nscripts = i + 1;
			break;
		}
}

/*
 * The highest index of a script that runs now, -1 if none is seen. Only
 * processes started after this one count: at boot that is every script,
 * and a script's process that stays in the background (S41toon-broker's
 * keeper) only counts from its own turn, not before.
 */
static int running_script(void)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	int best = -1, self = getpid();

	while (d && (e = readdir(d))) {
		char path[300], buf[256];
		int fd;
		ssize_t n;

		if (!isdigit((unsigned char)e->d_name[0]) || atoi(e->d_name) <= self)
			continue;
		snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n <= 0)
			continue;
		buf[n] = 0;
		for (char *s = buf; s < buf + n; s += strlen(s) + 1) {
			char *k = strstr(s, "/etc/init.d/S");

			if (!k)
				continue;
			for (int i = 0; i < nscripts; i++)
				if (!strcmp(k + 12, scripts[i]) && i > best)
					best = i;
		}
	}
	if (d)
		closedir(d);
	return best;
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---- files ----------------------------------------------------------------- */

static int write_bmp(const char *path)
{
	FILE *f = fopen(path, "wb");
	uint32_t row = (W * 3 + 3) & ~3u, size = 54 + row * H, data = row * H;
	uint8_t h[54] = { 'B', 'M' }, pad[4] = { 0 };

	if (!f)
		return 1;
	memcpy(h + 2, &size, 4);
	memcpy(h + 6, BMP_MARK, 4);	/* bfReserved1/2: U-Boot does not look at them */
	h[10] = 54;
	h[14] = 40;
	memcpy(h + 18, &W, 4);
	memcpy(h + 22, &H, 4);
	h[26] = 1;
	h[28] = 24;
	memcpy(h + 34, &data, 4);
	fwrite(h, 1, 54, f);
	for (int y = H - 1; y >= 0; y--) {	/* bottom row first */
		for (int x = 0; x < W; x++) {
			uint32_t c = scene[y * W + x];
			uint8_t px[3];

			/* a 16-bit panel (Toon 1): already dithered to the 16-bit
			   colours, so U-Boot's own rounding (none) keeps them and
			   its picture is the splash's, without bands */
			if (bpp == 16) {
				uint16_t v = to565(x, y, c);
				int r = v >> 11, g = (v >> 5) & 63, b = v & 31;

				c = (uint32_t)(r << 3 | r >> 2) << 16 | (uint32_t)(g << 2 | g >> 4) << 8 |
				    (uint32_t)(b << 3 | b >> 2);
			}
			px[0] = c & 255;
			px[1] = (c >> 8) & 255;
			px[2] = (c >> 16) & 255;

			fwrite(px, 1, 3, f);
		}
		fwrite(pad, 1, row - W * 3, f);
	}
	return fclose(f) ? 1 : 0;
}

static int write_frame(const char *path, double frac)
{
	FILE *f = fopen(path, "wb");
	int lx, ly, bx, by, bw, bh;

	if (!f)
		return 1;
	geometry(&lx, &ly, &bx, &by, &bw, &bh);
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			uint32_t c = y >= by && y < by + bh && x >= bx && x < bx + bw ?
				     bar_pixel(x, y, bx, by, bw, bh, 0, frac, 0.55) : scene[y * W + x];
			uint8_t px[3] = { (c >> 16) & 255, (c >> 8) & 255, c & 255 };

			fwrite(px, 1, 3, f);
		}
	return fclose(f) ? 1 : 0;
}

static int size_arg(const char *s)
{
	return sscanf(s, "%dx%d", &W, &H) == 2 && W > 0 && H > 0 && W <= 4096 && H <= 4096;
}

/* ---- main ------------------------------------------------------------------ */

static volatile sig_atomic_t done;
static void on_term(int sig) { (void)sig; done = 1; }

int main(int argc, char **argv)
{
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	int fd, lx, ly, bx, by, bw, bh, step = -1;
	int bmp = argc >= 3 && !strcmp(argv[1], "--bmp");
	double t0, t_step, shown = 0, finish_at = 0;

	if (argc >= 4 && !strcmp(argv[1], "--frame")) {
		if (!size_arg(argv[3]))
			return 2;
		make_scene();
		return write_frame(argv[2], argc > 4 ? atof(argv[4]) : 0.6);
	}
	if (bmp && argc > 3) {
		if (!size_arg(argv[3]))
			return 2;
		make_scene();
		return write_bmp(argv[2]);
	}
	fd = open("/dev/fb0", O_RDWR);
	if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &var) || ioctl(fd, FBIOGET_FSCREENINFO, &fix) ||
	    (var.bits_per_pixel != 32 && var.bits_per_pixel != 16))
		return 1;
	W = var.xres;
	H = var.yres;
	bpp = var.bits_per_pixel;
	if (bmp) {
		make_scene();
		return write_bmp(argv[2]);
	}
	stride = fix.line_length;
	fb = mmap(NULL, (size_t)stride * H, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED)
		return 1;
	if (argc >= 2 && !strcmp(argv[1], "--still")) {
		/* from a BMP only when it is one of ours; without one, work it out */
		if (argc >= 3) {
			if (load_bmp(argv[2]))
				return 0;
		} else {
			make_scene();
		}
		show_scene();
		/* with the empty bar: past U-Boot (its picture has none), the
		   splash fills it next */
		geometry(&lx, &ly, &bx, &by, &bw, &bh);
		for (int y = by; y < by + bh; y++)
			for (int x = bx; x < bx + bw; x++)
				put(x, y, bar_pixel(x, y, bx, by, bw, bh, 0, 0, -1));
		return 0;
	}
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	make_scene();
	geometry(&lx, &ly, &bx, &by, &bw, &bh);
	t0 = now();

	if (argc >= 2 && !strcmp(argv[1], "--stop")) {
		/* stopping: no way to know how far, so a piece of bar goes to and
		   fro; ended by the system (SIGTERM, or SIGKILL at the very end),
		   leaving the picture */
		if (!scene_shown())
			show_scene();
		while (!done && now() - t0 < 300) {
			double ph = fmod(now() - t0, 2.4) / 2.4, c = 0.5 - 0.5 * cos(ph * 2 * M_PI);
			double lo = c * 0.72, hi = lo + 0.28;

			for (int y = by; y < by + bh; y++)
				for (int x = bx; x < bx + bw; x++)
					put(x, y, bar_pixel(x, y, bx, by, bw, bh, lo, hi, -1));
			usleep(33000);
		}
		return 0;
	}

	list_scripts();
	if (!scene_shown()) {
		/* the picture comes up from black in a quarter of a second (it is
		   already there when the initrd put it up) */
		for (double t = t0; t < t0 + 0.25; t = now()) {
			int k = (int)((t - t0) / 0.25 * 255);

			for (int y = 0; y < H; y++)
				for (int x = 0; x < W; x++)
					put(x, y, mix(0, scene[y * W + x], k));
			usleep(16000);
		}
		show_scene();
	}

	t_step = now();
	for (;;) {
		double t = now(), target, shine;
		int s = nscripts ? running_script() : -1;

		if (s > step) {		/* the next script: a step further */
			step = s;
			t_step = t;
		}
		if (nscripts > 1 && step >= 0)	/* within a script, creep on by time */
			target = (step + 0.85 * (1 - exp(-(t - t_step) / 3.0))) / nscripts;
		else				/* no scripts seen: by time only */
			target = 0.9 * (1 - exp(-(t - t0) / 6.0));
		if (done) {
			if (!finish_at)
				finish_at = t;
			target = 1;
		}
		/* towards it, smoothly; never back */
		if (target > shown)
			shown += fmax((target - shown) * 0.12, done ? 0.05 : 0.002);
		if (shown > target)
			shown = target;
		shine = done ? -1 : fmod(t - t0, 1.6) / 1.6;
		for (int y = by; y < by + bh; y++)
			for (int x = bx; x < bx + bw; x++)
				put(x, y, bar_pixel(x, y, bx, by, bw, bh, 0, shown, shine));
		if (done && shown >= 1 && t - finish_at > 0.15)
			break;
		if (t - t0 > 180)
			break;		/* nobody came: do not sit here for ever */
		usleep(33000);
	}
	return 0;
}
