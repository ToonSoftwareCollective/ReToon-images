// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * toon-ui: GIF pictures for el.image, animated ones too (Home Assistant's
 * Buienradar camera gives its rain radar as one). Every frame is put
 * together on a canvas of the GIF's own size, as a browser does it
 * (transparency, "restore to background", "restore to previous"), and
 * scaled into the size it is shown at.
 *
 * Every read is checked against the data's length: the data comes from
 * any URL an app gives, so a broken or cut-off file must not read past
 * its end. A frame that is cut off ends the animation there.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "gif.h"

#define LZW_MAX		4096

struct rd {
	const uint8_t *p;
	size_t len, pos;
};

static int rd_byte(struct rd *r)
{
	return r->pos < r->len ? r->p[r->pos++] : -1;
}

static int rd_u16(struct rd *r)
{
	int a = rd_byte(r), b = rd_byte(r);

	return a < 0 || b < 0 ? -1 : a | b << 8;
}

static int rd_skip(struct rd *r, size_t n)
{
	if (r->len - r->pos < n)
		return -1;
	r->pos += n;
	return 0;
}

/* skip a chain of data sub-blocks (ending with a 0 length) */
static int skip_blocks(struct rd *r)
{
	for (;;) {
		int n = rd_byte(r);

		if (n < 0)
			return -1;
		if (!n)
			return 0;
		if (rd_skip(r, n))
			return -1;
	}
}

/* the bits of the image data's sub-blocks, read as a stream */
struct bits {
	struct rd *r;
	int left;		/* bytes left in the current sub-block */
	uint32_t acc;
	int n;
	int end;		/* the 0-length block came */
};

static int get_code(struct bits *b, int size)
{
	while (b->n < size) {
		int c;

		if (b->end)
			return -1;
		if (!b->left) {
			b->left = rd_byte(b->r);
			if (b->left <= 0) {
				b->end = 1;
				return -1;
			}
		}
		c = rd_byte(b->r);
		if (c < 0) {
			b->end = 1;
			return -1;
		}
		b->left--;
		b->acc |= (uint32_t)c << b->n;
		b->n += 8;
	}
	int code = b->acc & ((1u << size) - 1);

	b->acc >>= size;
	b->n -= size;
	return code;
}

/*
 * LZW into idx (npx indexes); fewer is fine (the rest stays 0 = what was
 * there). Afterwards the reader stands after the image's sub-blocks.
 */
static int lzw(struct rd *r, int min, uint8_t *idx, size_t npx)
{
	static __thread uint16_t prefix[LZW_MAX];
	static __thread uint8_t suffix[LZW_MAX], stack[LZW_MAX + 1];
	struct bits b = { .r = r };
	int clear = 1 << min, eoi = clear + 1, next = clear + 2, size = min + 1;
	int prev = -1, first = 0;
	size_t out = 0;

	if (min < 2 || min > 8)
		return -1;
	for (int i = 0; i < clear; i++) {
		prefix[i] = 0;
		suffix[i] = i;
	}
	for (;;) {
		int code = get_code(&b, size), c, sp = 0;

		if (code < 0 || code == eoi)
			break;
		if (code == clear) {
			next = clear + 2;
			size = min + 1;
			prev = -1;
			continue;
		}
		if (prev < 0) {
			if (code >= clear)
				break;		/* a broken stream */
			first = code;
			if (out < npx)
				idx[out++] = code;
			prev = code;
			continue;
		}
		if (code > next || (code == next && next >= LZW_MAX))
			break;
		c = code;
		if (code == next) {	/* the KwKwK case */
			stack[sp++] = first;
			c = prev;
		}
		while (c >= clear && sp < LZW_MAX) {
			stack[sp++] = suffix[c];
			c = prefix[c];
		}
		if (c >= clear)
			break;
		stack[sp++] = c;
		first = c;
		while (sp)
			if (out < npx)
				idx[out++] = stack[--sp];
			else
				sp--;
		if (next < LZW_MAX) {
			prefix[next] = prev;
			suffix[next] = first;
			next++;
			if (next == 1 << size && size < 12)
				size++;
		}
		prev = code;
	}
	/* to the end of the sub-blocks, whatever happened above */
	if (!b.end) {
		if (b.left && rd_skip(r, b.left))
			return -1;
		if (skip_blocks(r))
			return -1;
	}
	return out ? 0 : -1;
}

/* the canvas (R, G, B, A) into one frame of dw x dh (B, G, R, A) */
static uint8_t *scaled(const uint8_t *cv, int sw, int sh, int dw, int dh)
{
	uint8_t *out = malloc((size_t)dw * dh * 4);

	if (!out)
		return NULL;
	for (int y = 0; y < dh; y++) {
		const uint8_t *row = cv + (size_t)(y * sh / dh) * sw * 4;

		for (int x = 0; x < dw; x++) {
			const uint8_t *q = row + (size_t)(x * sw / dw) * 4;
			uint8_t *d = out + ((size_t)y * dw + x) * 4;

			d[0] = q[2];
			d[1] = q[1];
			d[2] = q[0];
			d[3] = q[3];
		}
	}
	return out;
}

void gif_free(struct gif_frames *g)
{
	for (int i = 0; i < g->n; i++)
		free(g->px[i]);
	free(g->px);
	free(g->delay);
	memset(g, 0, sizeof(*g));
}

const char *gif_decode(const uint8_t *data, size_t len, int fit_w, int fit_h,
		       size_t max_pixels, struct gif_frames *g)
{
	struct rd r = { data, len, 0 };
	uint8_t gct[256 * 3], lct[256 * 3], *cv = NULL, *saved = NULL, *idx = NULL;
	int sw, sh, flags, gct_n = 0, trans = -1, disposal = 0, delay = 0;
	int prev_disp = 0, px_ = 0, py_ = 0, pw_ = 0, ph_ = 0;
	size_t bytes = 0;
	const char *err = NULL;
	double f;

	memset(g, 0, sizeof(*g));
	if (len < 13 || memcmp(data, "GIF8", 4) || (data[4] != '7' && data[4] != '9') || data[5] != 'a')
		return "not a GIF";
	r.pos = 6;
	sw = rd_u16(&r);
	sh = rd_u16(&r);
	flags = rd_byte(&r);
	if (sw <= 0 || sh <= 0 || flags < 0 || rd_skip(&r, 2))
		return "not a GIF it can read";
	if ((size_t)sw * sh > max_pixels)
		return "the GIF is too large";
	if (flags & 0x80) {
		gct_n = 2 << (flags & 7);
		if (len - r.pos < (size_t)gct_n * 3)
			return "the GIF is cut off";
		memcpy(gct, data + r.pos, gct_n * 3);
		r.pos += gct_n * 3;
	}
	/* the size it is shown at: the whole picture fits, never larger */
	f = (double)fit_w / sw < (double)fit_h / sh ? (double)fit_w / sw : (double)fit_h / sh;
	if (f > 1)
		f = 1;
	g->w = (int)(sw * f + 0.5);
	g->h = (int)(sh * f + 0.5);
	if (g->w < 1)
		g->w = 1;
	if (g->h < 1)
		g->h = 1;
	/* transparent to start with, as a browser shows it */
	cv = calloc((size_t)sw * sh, 4);
	g->px = calloc(GIF_MAX_FRAMES, sizeof(*g->px));
	g->delay = calloc(GIF_MAX_FRAMES, sizeof(*g->delay));
	if (!cv || !g->px || !g->delay) {
		err = "out of memory";
		goto out;
	}

	while (!err && g->n < GIF_MAX_FRAMES) {
		int b = rd_byte(&r);

		if (b < 0 || b == 0x3b)	/* the end (a missing trailer is common) */
			break;
		if (b == 0x21) {
			int label = rd_byte(&r);

			if (label == 0xf9) {	/* graphic control: for the next image */
				int n = rd_byte(&r), fl = rd_byte(&r), d = rd_u16(&r), t = rd_byte(&r);

				if (n != 4 || t < 0 || d < 0 || skip_blocks(&r))
					break;
				disposal = (fl >> 2) & 7;
				trans = fl & 1 ? t : -1;
				delay = d * 10;
			} else if (label < 0 || skip_blocks(&r)) {
				break;
			}
			continue;
		}
		if (b != 0x2c)
			break;	/* not something a GIF has here: what came so far */

		int x = rd_u16(&r), y = rd_u16(&r), w = rd_u16(&r), h = rd_u16(&r), fl = rd_byte(&r);
		const uint8_t *pal = gct;
		int pal_n = gct_n, min;

		if (x < 0 || y < 0 || w < 0 || h < 0 || fl < 0)
			break;
		if (fl & 0x80) {
			pal_n = 2 << (fl & 7);
			if (len - r.pos < (size_t)pal_n * 3)
				break;
			memcpy(lct, data + r.pos, pal_n * 3);
			r.pos += pal_n * 3;
			pal = lct;
		}
		min = rd_byte(&r);
		if (min < 0 || !w || !h || (size_t)w * h > max_pixels)
			break;

		/* what the frame before asked to be done with its rectangle */
		if (prev_disp == 2) {
			for (int yy = py_; yy < py_ + ph_ && yy < sh; yy++)
				if (px_ < sw)
					memset(cv + ((size_t)yy * sw + px_) * 4, 0,
					       (size_t)((px_ + pw_ > sw ? sw : px_ + pw_) - px_) * 4);
		} else if (prev_disp == 3 && saved) {
			memcpy(cv, saved, (size_t)sw * sh * 4);
		}
		if (disposal == 3) {
			if (!saved)
				saved = malloc((size_t)sw * sh * 4);
			if (saved)
				memcpy(saved, cv, (size_t)sw * sh * 4);
		}

		free(idx);
		idx = calloc((size_t)w * h, 1);
		if (!idx) {
			err = "out of memory";
			break;
		}
		if (lzw(&r, min, idx, (size_t)w * h))
			break;

		/* the indexes onto the canvas; interlaced rows come in 4 passes */
		for (int row = 0, pass = 0, yy = 0; row < h; row++) {
			int ty = yy;

			if (fl & 0x40) {
				static const int start[] = { 0, 4, 2, 1 }, step[] = { 8, 8, 4, 2 };

				ty = yy;
				yy += step[pass];
				while (pass < 3 && yy >= h)
					yy = start[++pass];
			} else {
				yy++;
			}
			if (y + ty >= sh)
				continue;
			for (int xx = 0; xx < w && x + xx < sw; xx++) {
				int c = idx[(size_t)row * w + xx];
				uint8_t *d = cv + ((size_t)(y + ty) * sw + x + xx) * 4;

				if (c == trans || c >= pal_n)
					continue;
				d[0] = pal[c * 3];
				d[1] = pal[c * 3 + 1];
				d[2] = pal[c * 3 + 2];
				d[3] = 0xff;
			}
		}

		if (bytes + (size_t)g->w * g->h * 4 > GIF_MAX_BYTES)
			break;	/* enough: the animation is this long */
		g->px[g->n] = scaled(cv, sw, sh, g->w, g->h);
		if (!g->px[g->n]) {
			err = "out of memory";
			break;
		}
		bytes += (size_t)g->w * g->h * 4;
		/* as browsers do: a delay under 20 ms is 100 ms */
		g->delay[g->n++] = delay < 20 ? 100 : delay;
		prev_disp = disposal;
		px_ = x;
		py_ = y;
		pw_ = w;
		ph_ = h;
		trans = -1;
		disposal = 0;
		delay = 0;
	}
	if (!err && !g->n)
		err = "not a GIF it can read";
out:
	free(cv);
	free(saved);
	free(idx);
	if (err)
		gif_free(g);
	return err;
}
