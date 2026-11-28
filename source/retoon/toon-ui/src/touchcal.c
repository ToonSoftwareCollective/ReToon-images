// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Touch calibration, both Toons. CONFIG_DIR/touch.conf holds the raw values
 * at the screen edges, "min_x min_y max_x max_y" plus "swap" when the panel's
 * axes are crossed. Without it:
 *   - the original software's tslib pointercal (made in the factory, Toon 1)
 *     is converted and saved as touch.conf, once;
 *   - otherwise a resistive panel (raw range much larger than the screen)
 *     gets the calibration screen at once;
 *   - otherwise (Toon 2, capacitive) the driver's own range is used.
 * The calibration screen (also from the web interface, "calibrate" on
 * ui.cmd) shows four crosses; the new values are kept only when the Keep
 * button is then tapped, which can only be hit when they are right.
 */
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ui.h"

#define TOUCH_CONF	CONFIG_DIR "/touch.conf"
#define VENDOR_POINTERCAL "/mnt/ubi/etc/pointercal"
#define KEEP_S		15	/* seconds to confirm before the old values return */
#define IDLE_S		60	/* no tap for this long: stop (not when started by itself) */

struct cal {
	int mn_x, mn_y, mx_x, mx_y;
	bool swap;
};

static lv_indev_t *dev;
static struct cal raw;		/* the driver's range: no calibration */
static struct cal cur;		/* in use */
static struct cal prev;		/* before the calibration screen */

static void apply(const struct cal *c)
{
	cur = *c;
	if (!dev)
		return;
	lv_evdev_set_swap_axes(dev, c->swap);
	lv_evdev_set_calibration(dev, c->mn_x, c->mn_y, c->mx_x, c->mx_y);
}

static bool load(struct cal *c)
{
	FILE *f = fopen(TOUCH_CONF, "r");
	char swap[16] = "";
	int n;

	if (!f)
		return false;
	n = fscanf(f, "%d %d %d %d %15s", &c->mn_x, &c->mn_y, &c->mx_x, &c->mx_y, swap);
	fclose(f);
	c->swap = n == 5 && !strcmp(swap, "swap");
	return n >= 4;
}

static bool save(const struct cal *c)
{
	FILE *f;
	bool ok;

	if (system("mount -o remount,rw /mnt/data 2>/dev/null"))
		return false;
	mkdir(CONFIG_DIR, 0700);
	f = fopen(TOUCH_CONF ".tmp", "w");
	ok = f && fprintf(f, "%d %d %d %d%s\n", c->mn_x, c->mn_y, c->mx_x, c->mx_y,
			  c->swap ? " swap" : "") > 0;
	if (f)
		ok = !fclose(f) && ok;
	ok = ok && !rename(TOUCH_CONF ".tmp", TOUCH_CONF);
	sync();
	/* not while toon-update is writing its download there */
	if (access("/run/toon/update.lock", F_OK))
		ok = !system("mount -o remount,ro /mnt/data 2>/dev/null") && ok;
	return ok;
}

/* tslib: x = (a2 + a0 * raw_x + a1 * raw_y) / a6, y = (a5 + a3 * raw_x
 * + a4 * raw_y) / a6; the cross terms are tiny on these panels */
static bool from_pointercal(struct cal *c)
{
	FILE *f = fopen(VENDOR_POINTERCAL, "r");
	long a[7];
	bool ok;

	if (!f)
		return false;
	ok = fscanf(f, "%ld %ld %ld %ld %ld %ld %ld", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6]) == 7 &&
	     a[0] && a[4] && a[6];
	fclose(f);
	if (!ok)
		return false;
	c->mn_x = (int)(-a[2] / a[0]);
	c->mx_x = (int)((screen_w * a[6] - a[2]) / a[0]);
	c->mn_y = (int)(-a[5] / a[4]);
	c->mx_y = (int)((screen_h * a[6] - a[5]) / a[4]);
	c->swap = false;
	return true;
}

static bool resistive(void)
{
	return abs(raw.mx_x - raw.mn_x) > 2 * screen_w || abs(raw.mx_y - raw.mn_y) > 2 * screen_h;
}

/* ---- the calibration screen ---------------------------------------------- */

static lv_obj_t *win, *cross, *title, *text, *keep_box;
static lv_timer_t *timer;
static int step, samples;
static long sum_x, sum_y;
static int got[4][2];		/* raw x, y at each cross */
static bool by_itself;
static uint32_t t_last, t_keep;

static void cross_at(int i)
{
	int m_x = screen_w / 10, m_y = screen_h / 10;
	int x = i == 1 || i == 2 ? screen_w - 1 - m_x : m_x;
	int y = i >= 2 ? screen_h - 1 - m_y : m_y;

	lv_obj_set_pos(cross, x - S(30), y - S(30));
}

static void say(const char *msg)
{
	lv_label_set_text(text, msg);
}

static void ask_point(void)
{
	char buf[200];

	snprintf(buf, sizeof(buf), tr("Tap the centre of the cross, as precisely as you can (%d of 4)."),
		 step + 1);
	say(buf);
	lv_obj_remove_flag(cross, LV_OBJ_FLAG_HIDDEN);
	cross_at(step);
}

static void close_win(void)
{
	if (timer)
		lv_timer_delete(timer);
	timer = NULL;
	if (win)
		lv_obj_delete(win);
	win = cross = title = text = keep_box = NULL;
}

/* the raw value under a point, while the driver's range is in use */
static int to_raw(int p, int mn, int mx, int size)
{
	return mn + (int)((long)p * (mx - mn) / (size - 1));
}

static bool compute(struct cal *c)
{
	int m_x = screen_w / 10, m_y = screen_h / 10, i;
	int r[4][2];
	long along_x, along_y, left, right, top, bottom, span_x, span_y;
	double sx, sy;

	memcpy(r, got, sizeof(r));
	/* crosses 0..3: top left, top right, bottom right, bottom left. When
	 * the raw x follows the screen's y, the axes are crossed. */
	along_x = labs((long)r[1][0] + r[2][0] - r[0][0] - r[3][0]);
	along_y = labs((long)r[3][0] + r[2][0] - r[0][0] - r[1][0]);
	c->swap = along_y > along_x;
	if (c->swap)
		for (i = 0; i < 4; i++) {
			int t = r[i][0];

			r[i][0] = r[i][1];
			r[i][1] = t;
		}
	left = (r[0][0] + r[3][0]) / 2;
	right = (r[1][0] + r[2][0]) / 2;
	top = (r[0][1] + r[1][1]) / 2;
	bottom = (r[3][1] + r[2][1]) / 2;
	span_x = right - left;
	span_y = bottom - top;
	/* the two taps on each side must agree, and the sides lie apart */
	if (labs(span_x) < screen_w / 4 || labs(span_y) < screen_h / 4 ||
	    labs(r[0][0] - r[3][0]) > labs(span_x) / 8 || labs(r[1][0] - r[2][0]) > labs(span_x) / 8 ||
	    labs(r[0][1] - r[1][1]) > labs(span_y) / 8 || labs(r[3][1] - r[2][1]) > labs(span_y) / 8)
		return false;
	sx = (double)span_x / (screen_w - 1 - 2 * m_x);
	sy = (double)span_y / (screen_h - 1 - 2 * m_y);
	c->mn_x = (int)(left - sx * m_x);
	c->mx_x = (int)(c->mn_x + sx * (screen_w - 1));
	c->mn_y = (int)(top - sy * m_y);
	c->mx_y = (int)(c->mn_y + sy * (screen_h - 1));
	return true;
}

static void keep_clicked(lv_event_t *ev)
{
	(void)ev;
	if (!save(&cur))
		fprintf(stderr, "toon-ui: could not save " TOUCH_CONF "\n");
	close_win();
}

static void again_clicked(lv_event_t *ev);

static void ask_keep(void)
{
	lv_obj_t *b, *l;

	lv_obj_add_flag(cross, LV_OBJ_FLAG_HIDDEN);
	keep_box = lv_obj_create(win);
	lv_obj_remove_style_all(keep_box);
	lv_obj_set_size(keep_box, S(560), S(90));
	lv_obj_align(keep_box, LV_ALIGN_CENTER, 0, S(120));
	b = lv_button_create(keep_box);
	lv_obj_set_size(b, S(260), S(80));
	lv_obj_align(b, LV_ALIGN_LEFT_MID, 0, 0);
	lv_obj_set_style_bg_color(b, lv_color_hex(C_ACCENT), 0);
	lv_obj_add_event_cb(b, keep_clicked, LV_EVENT_CLICKED, NULL);
	l = lv_label_create(b);
	lv_obj_set_style_text_font(l, f_head, 0);
	lv_label_set_text(l, tr("Keep"));
	lv_obj_center(l);
	b = lv_button_create(keep_box);
	lv_obj_set_size(b, S(260), S(80));
	lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD_HI), 0);
	lv_obj_add_event_cb(b, again_clicked, LV_EVENT_CLICKED, NULL);
	l = lv_label_create(b);
	lv_obj_set_style_text_font(l, f_head, 0);
	lv_label_set_text(l, tr("Again"));
	lv_obj_center(l);
	t_keep = now_ms();
}

static void start_points(void)
{
	if (keep_box)
		lv_obj_delete(keep_box);
	keep_box = NULL;
	apply(&raw);
	step = 0;
	samples = 0;
	t_last = now_ms();
	ask_point();
}

static void again_clicked(lv_event_t *ev)
{
	(void)ev;
	start_points();
}

static void on_press(lv_event_t *ev)
{
	lv_event_code_t code = lv_event_get_code(ev);
	lv_point_t p;
	struct cal c;

	if (keep_box)
		return;
	lv_indev_get_point(lv_indev_active(), &p);
	t_last = now_ms();
	if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
		/* the panel is noisy while the finger settles: average */
		sum_x += to_raw(p.x, raw.mn_x, raw.mx_x, screen_w);
		sum_y += to_raw(p.y, raw.mn_y, raw.mx_y, screen_h);
		samples++;
		return;
	}
	if (code != LV_EVENT_RELEASED || !samples)
		return;
	got[step][0] = sum_x / samples;
	got[step][1] = sum_y / samples;
	sum_x = sum_y = samples = 0;
	if (++step < 4) {
		ask_point();
		return;
	}
	if (!compute(&c)) {
		start_points();
		say(tr("That did not work out. Tap each cross once, in its centre. Again (1 of 4)."));
		return;
	}
	apply(&c);
	say(tr("Tap Keep to use the new calibration. Without a tap the old one returns."));
	ask_keep();
}

static void tick(lv_timer_t *t)
{
	char buf[200];
	uint32_t now = now_ms();

	(void)t;
	if (keep_box) {
		int left = KEEP_S - (int)((now - t_keep) / 1000);

		if (left <= 0) {
			apply(&prev);
			close_win();
			return;
		}
		snprintf(buf, sizeof(buf), tr("Tap Keep to use the new calibration. Without a tap the old one "
					      "returns in %d s."), left);
		say(buf);
		return;
	}
	if (!by_itself && now - t_last > IDLE_S * 1000) {
		apply(&prev);
		close_win();
	}
}

void touchcal_open(bool automatic)
{
	lv_obj_t *h, *v;

	if (win || !dev)
		return;
	by_itself = automatic;
	prev = cur;
	win = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(win);
	lv_obj_set_size(win, screen_w, screen_h);
	lv_obj_set_style_bg_color(win, lv_color_hex(C_BG), 0);
	lv_obj_set_style_bg_opa(win, LV_OPA_COVER, 0);
	lv_obj_add_flag(win, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(win, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_event_cb(win, on_press, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(win, on_press, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(win, on_press, LV_EVENT_RELEASED, NULL);

	title = lv_label_create(win);
	lv_obj_set_style_text_font(title, f_mid, 0);
	lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
	lv_label_set_text(title, tr("Touch calibration"));
	lv_obj_align(title, LV_ALIGN_CENTER, 0, -S(110));

	text = lv_label_create(win);
	lv_obj_set_width(text, S(640));
	lv_obj_set_style_text_font(text, f_body, 0);
	lv_obj_set_style_text_color(text, lv_color_hex(C_TEXT), 0);
	lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(text, LV_ALIGN_TOP_MID, 0, screen_h / 2 - S(60));

	/* a cross of two bars, 60 px, its centre on the target */
	cross = lv_obj_create(win);
	lv_obj_remove_style_all(cross);
	lv_obj_set_size(cross, S(60), S(60));
	lv_obj_remove_flag(cross, LV_OBJ_FLAG_CLICKABLE);
	h = lv_obj_create(cross);
	lv_obj_remove_style_all(h);
	lv_obj_set_size(h, S(60), 2);
	lv_obj_center(h);
	lv_obj_remove_flag(h, LV_OBJ_FLAG_CLICKABLE);
	v = lv_obj_create(cross);
	lv_obj_remove_style_all(v);
	lv_obj_set_size(v, 2, S(60));
	lv_obj_center(v);
	lv_obj_remove_flag(v, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_bg_color(h, lv_color_hex(C_ACCENT), 0);
	lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(v, lv_color_hex(C_ACCENT), 0);
	lv_obj_set_style_bg_opa(v, LV_OPA_COVER, 0);

	timer = lv_timer_create(tick, 1000, NULL);
	lv_display_trigger_activity(NULL);
	start_points();
}

/* back to what a Toon has without touch.conf */
void touchcal_reset(void)
{
	struct cal c;

	if (win)
		close_win();
	if (!access(TOUCH_CONF, F_OK) && !system("mount -o remount,rw /mnt/data 2>/dev/null")) {
		unlink(TOUCH_CONF);
		sync();
		if (access("/run/toon/update.lock", F_OK))
			(void)!system("mount -o remount,ro /mnt/data 2>/dev/null");
	}
	if (from_pointercal(&c)) {
		apply(&c);
	} else {
		apply(&raw);
		if (resistive())
			touchcal_open(true);
	}
}

/* At the start, before the fonts are there: apply what there is. Returns
 * true when the calibration screen must be opened (touchcal_open(true)). */
bool touchcal_init(lv_indev_t *indev, const char *path)
{
	struct input_absinfo ax, ay;
	struct cal c;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	dev = indev;
	raw = (struct cal){ 0, 0, screen_w - 1, screen_h - 1, false };
	if (!dev)
		return false;	/* no touch screen (a bare board) */
	if (fd >= 0) {
		if (!ioctl(fd, EVIOCGABS(ABS_X), &ax) && !ioctl(fd, EVIOCGABS(ABS_Y), &ay))
			raw = (struct cal){ ax.minimum, ay.minimum, ax.maximum, ay.maximum, false };
		close(fd);
	}
	cur = raw;
	if (load(&c)) {
		apply(&c);
		return false;
	}
	if (from_pointercal(&c)) {
		apply(&c);
		if (!save(&c))
			fprintf(stderr, "toon-ui: could not save " TOUCH_CONF "\n");
		return false;
	}
	return resistive();
}
