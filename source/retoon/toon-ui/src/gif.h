/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TOON_GIF_H
#define TOON_GIF_H

#include <stddef.h>
#include <stdint.h>

#define GIF_MAX_FRAMES	100
#define GIF_MAX_BYTES	(8u << 20)	/* all frames together, at the shown size */

/* the frames at the size they are shown, B, G, R, A, with their delays */
struct gif_frames {
	int n, w, h;
	uint8_t **px;
	int *delay;		/* ms */
};

/* NULL when it worked, else why not; g is empty then */
const char *gif_decode(const uint8_t *data, size_t len, int fit_w, int fit_h,
		       size_t max_pixels, struct gif_frames *g);
void gif_free(struct gif_frames *g);

#endif
