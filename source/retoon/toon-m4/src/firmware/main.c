// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Toon 2 Cortex-M4 sensor firmware (bare metal, runs from TCM).
 *
 *   ULTRASONIC approach detection  (ADC1 ch0 + PWM2 40 kHz + GPIO5_IO16/17)
 *     Two methods, chosen by the A9 (a9_ctrl CTRL_US_PULSED):
 *     - pulsed ranging (default): a burst of a few 40 kHz cycles, then
 *       ~11 ms of listening split into ~4.7 cm range bins, each demodulated
 *       coherently against the PWM counter; the profile is compared with a
 *       slowly learned profile of the still room, and a new echo inside the
 *       range gate (35..180 cm) is a person. The nearest such bin is the
 *       distance. Presence over most of the last 20 pings wakes the screen.
 *     - continuous-wave coherent residual (the first method, kept for
 *       comparison): transmit continuously, demodulate the receiver against
 *       the PWM phase, movement = residual against a slowly adapting I/Q
 *       baseline. Needs the gapless sampling only this core can do.
 *     Touch learning (both, off unless CTRL_LEARN): a touch without a
 *     preceding wake lowers the threshold toward the strongest signal of the
 *     seconds before it, within a fixed bound. Nothing raises it: a wake
 *     without a touch is someone who looked and left, not a mistake.
 *     Pulsed mode freezes the still-room profile around an echo, so a
 *     person standing still stays detected; a "presence" that lasts ten
 *     minutes without a touch is taken as a changed room and re-learned.
 *
 *   VOC air quality  (ADC2 ch0 + PWM5 27.5 kHz heater + GPIO5_IO18 strobe)
 *     Once a second: strobe IO18 low, wait for the
 *     PWM5 phase window, sample ADC2 ch0, publish the raw count. The A9
 *     does the slow baseline/index math.
 *
 * Everything goes through the shared mailbox (shared.h); results are polled
 * by toon-m4d. Bring up with bringup.c first; enable via a9_ctrl bits.
 * With CTRL_OWN_PERIPH the firmware also ungates the PWM clocks and
 * initialises/calibrates the ADCs itself (device tree without the Linux
 * ADC/PWM nodes); the pad muxes it always sets itself.
 */
#include <stdint.h>
#include "imx6sx.h"
#include "shared.h"

struct m4_shared g_shared __attribute__((section(".shared")));
struct m4_diag   g_diag   __attribute__((section(".shared.diag")));
volatile uint8_t g_snd[M4_SND_SIZE] __attribute__((section(".shared.snd")));

#define ADC1(o)		REG32(ADC1_BASE + (o))
#define ADC2(o)		REG32(ADC2_BASE + (o))
#define PWM2(o)		REG32(PWM2_BASE + (o))
#define PWM5(o)		REG32(PWM5_BASE + (o))
#define GPIO5(o)	REG32(GPIO5_BASE + (o))
#define IOMUXC(o)	REG32(IOMUXC_BASE + (o))
#define CCM(o)		REG32(CCM_BASE + (o))

/* ---------- small math (no libm) ---------- */
#define PI 3.14159265f
static float my_sinf(float x)
{
	while (x > PI) x -= 2 * PI;
	while (x < -PI) x += 2 * PI;
	float x2 = x * x;
	return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72))));
}
static float my_cosf(float x) { return my_sinf(x + PI / 2); }
static inline float vsqrtf(float x)
{
	float r;
	__asm__("vsqrt.f32 %0, %1" : "=t"(r) : "t"(x));
	return r;
}
static float my_hypotf(float a, float b) { return vsqrtf(a * a + b * b); }
static inline float my_fabsf(float x) { return x < 0 ? -x : x; }

/* ---------- sound: PWM4 as a DAC for the buzzer (CTRL_SND_EN) ---------- */
#define PWM4_BASE	0x4208c000u
#define PWM4(o)		REG32(PWM4_BASE + (o))
#define SND_PERIOD	750u		/* 24 MHz / 750 = 32 kHz carrier */
#define SND_CR		((1u << 1) | (2u << 16))	/* REPEAT=2 (16000 samples/s), CLKSRC=perclk 24 MHz */
static volatile int snd_on;
static uint32_t snd_cur;		/* sample on the output now (0..255) */
static int snd_playing;

static void snd_init(void)
{
	IOMUXC(0x01F0) = 2;  IOMUXC(0x0538) = 0x13058;	/* RGMII2_RD0 -> PWM4_OUT */
	CCM(0x78) |= 3u << 22;				/* PWM4 clock gate (CCGR4) */
	PWM4(PWM_CR) = 1u << 3;				/* software reset */
	for (int i = 0; i < 1000 && (PWM4(PWM_CR) & (1u << 3)); i++)
		;
	PWM4(PWM_CR) = SND_CR;
	PWM4(PWM_PR) = SND_PERIOD - 2;
	for (int i = 0; i < 4; i++)
		PWM4(PWM_SAR) = 0;
	PWM4(PWM_CR) = SND_CR | 1u;
	snd_cur = 0;
	g_shared.snd_rd = 0;
	g_shared.snd_underruns = 0;
	g_shared.snd_rate = M4_SND_RATE;
	snd_on = 1;
}
/* keep the PWM's 4-sample FIFO full: the PWM paces the sample rate */
static inline void snd_feed(void)
{
	uint32_t rd = g_shared.snd_rd;

	while ((PWM4(PWM_SR) & 7u) < 4u) {
		if (rd != g_shared.a9_snd_wr) {
			snd_cur = g_snd[rd % M4_SND_SIZE];
			rd++;
			snd_playing = 1;
		} else {
			/* nothing to play: slide to 0 (no current), no click. A sound
			   ends with toon-sound's own fade to 0; running dry above
			   that is a real gap */
			if (snd_playing) {
				if (snd_cur > 8)
					g_shared.snd_underruns++;
				snd_playing = 0;
			}
			if (snd_cur)
				snd_cur--;
		}
		PWM4(PWM_SAR) = (snd_cur * SND_PERIOD) >> 8;
	}
	g_shared.snd_rd = rd;
}

/* ---------- time ---------- */
/* SysTick at 32 kHz: feeds the sound FIFO; every 32nd tick is a millisecond */
static volatile uint32_t ms_ticks;
static uint32_t sub_ticks;
#define SYST_CSR	REG32(0xE000E010)
#define SYST_RVR	REG32(0xE000E014)
#define SYST_CVR	REG32(0xE000E018)
#define TICK_HZ		32000u
void SysTick_Handler(void)
{
	if (snd_on)
		snd_feed();
	if (++sub_ticks >= TICK_HZ / 1000u) {
		sub_ticks = 0;
		ms_ticks++;
	}
}
#define M4_HZ		227000000u	/* measured: uptime_ms tracks wall time */
static void systick_init(void)
{
	SYST_RVR = (M4_HZ / TICK_HZ) - 1u;
	SYST_CVR = 0;
	SYST_CSR = 7;			/* enable | tickint | core clock */
}
static uint32_t now_ms(void) { return ms_ticks; }

/* ---------- fault trap: report into the mailbox, then halt ---------- */
#define SCB_CFSR	REG32(0xE000ED28)
#define SCB_HFSR	REG32(0xE000ED2C)
#define SCB_BFAR	REG32(0xE000ED38)
void hardfault_c(uint32_t *frame)
{
	g_shared.fault_cfsr = SCB_CFSR;
	g_shared.fault_hfsr = SCB_HFSR;
	g_shared.fault_bfar = SCB_BFAR;
	g_shared.fault_lr = frame[5];
	g_shared.fault_pc = frame[6];
	g_shared.fault_flag = M4_FAULT_FLAG;
	for (;;)
		;
}
__attribute__((naked)) void HardFault_Handler(void)
{
	__asm__ volatile("mrs r0, msp\n\tb hardfault_c");
}

/* ---------- peripheral ownership ---------- */
/* Pad muxes: always ours (nothing in the Linux device tree claimed PWM2's
 * pad before the cm4 node; the prototypes set it through /dev/mem). Values
 * as the prototypes used. */
static void pads_init(int us, int voc)
{
	if (us) {
		IOMUXC(0x01F8) = 2;  IOMUXC(0x0540) = 0x11058;	/* RGMII2_RD2    -> PWM2_OUT   */
		IOMUXC(0x0200) = 5;  IOMUXC(0x0548) = 0x17059;	/* RGMII2_RX_CTL -> GPIO5_IO16 */
		IOMUXC(0x0204) = 5;  IOMUXC(0x054C) = 0x17059;	/* RGMII2_RXC    -> GPIO5_IO17 */
	}
	if (voc) {
		IOMUXC(0x0214) = 3;  IOMUXC(0x055C) = 0x110f1;	/* RGMII2_TD3    -> PWM5_OUT   */
		IOMUXC(0x0208) = 5;  IOMUXC(0x0550) = 0x17059;	/* RGMII2_TD0    -> GPIO5_IO18 */
	}
}
/* Clock gates, only when Linux no longer holds them (CTRL_OWN_PERIPH):
 * PWM2 = CCGR4 bits 19:18, PWM5 = CCGR6 bits 27:26 (kernel clk-imx6sx). */
static void clocks_own(int us, int voc)
{
	if (us)  CCM(0x78) |= 3u << 18;
	if (voc) CCM(0x80) |= 3u << 26;
}
/* ADC set-up as the Linux vf610 driver leaves it: CFG 0x000103E8 = 12-bit,
 * ipg/8, long sample time, low power, overwrite; then calibrate. */
#define ADC_GS		0x1c
static int adc_init_own(uint32_t base)
{
	REG32(base + ADC_HC0) = 0x1f;			/* disable conversions */
	REG32(base + ADC_CFG) = 0x000103E8;
	REG32(base + ADC_GC) = 0x80;			/* CAL */
	for (int i = 0; i < 2000000; i++)
		if (!(REG32(base + ADC_GC) & 0x80)) break;
	(void)REG32(base + ADC_R0);
	return (REG32(base + ADC_GS) & 0x2) ? -1 : 0;	/* CALF = failed */
}

/* ---------- ultrasonic: common ---------- */
#define US_HOLD		6.0f		/* seconds wake held after presence   */
static uint32_t us_period = 600;	/* PWM2 counts per 40 kHz cycle       */
static int   us_on;
static float off_timer;
static uint32_t us_wakes;
static uint32_t last_touch_seq;
static int   learn;			/* CTRL_LEARN: adapt the threshold from touches */
static float cos_tab[1024], sin_tab[1024];
static uint16_t rx[2560], cnt[2560];	/* CW block (2048) / pulsed listen window */

static void us_pwm_setup(void)
{
	/* GPIO5 IO16/17 = outputs, driven LOW (transducer enable) */
	GPIO5(GPIO_GDIR) |= (3u << 16);
	GPIO5(GPIO_DR)   &= ~(3u << 16);
	/* PWM2: 40 kHz 50 %. CLKSRC=2 = perclk 24 MHz (as the kernel's pwm
	 * driver): 600 counts = 40 kHz. Always runs: it is the phase reference
	 * for both methods. */
	PWM2(PWM_CR) = 0;
	PWM2(PWM_CR) = (2u << 16);
	PWM2(PWM_PR) = us_period - 2;
	PWM2(PWM_SAR) = us_period / 2;
	PWM2(PWM_CR) |= 1u;
}
/* transmit gate: the PWM keeps counting, only the pad is switched between
 * PWM2_OUT (ALT2) and GPIO (ALT5, nothing drives the transducer) */
static inline void us_tx(int on) { IOMUXC(0x01F8) = on ? 2 : 5; }

static void us_adc_init(void)
{
	/* ADC1 ch0 continuous, no averaging */
	uint32_t gc = ADC1(ADC_GC);
	ADC1(ADC_GC) = (gc & ~0x20u) | 0x40u;
	ADC1(ADC_HC0) = 0;
}
static void us_build_tables(void)
{
	us_period = (PWM2(PWM_PR) & 0xffff) + 2;
	if (us_period < 2 || us_period > 1024) us_period = 600;
	for (uint32_t i = 0; i < us_period; i++) {
		float w = 2 * PI * (float)i / (float)us_period;
		cos_tab[i] = my_cosf(w);
		sin_tab[i] = my_sinf(w);
	}
}

/* ---------- ultrasonic: CW coherent residual (method 1) ---------- */
#define US_BLOCK	2048
#define US_ALPHA	0.02f
#define US_THRESH_MIN	0.12f		/* 2026-10-23: 0.25 missed a hand at 5 cm; floor on two units 0.03-0.07 */
#define US_THRESH_K	3.5f		/* threshold = K * still-room floor (~0.15 on production units) */
#define US_SUSTAIN	0.7f		/* seconds of motion to wake (2026-10-23: was 1.0) */
#define US_NOISE_A	0.01f
static float bI, bQ, noise_floor = 0.05f, cw_thresh = US_THRESH_MIN, active, prev_resid;

static int cw_first = 1;
/* live retune from the A9 (resonance search): new PWM period, new tables,
 * baseline restarted */
static void us_retune(void)
{
	uint32_t p = g_shared.a9_us_period ? g_shared.a9_us_period : 600;
	if (p < 400 || p > 1000 || p == us_period) return;
	us_period = p;
	us_pwm_setup();
	us_build_tables();
	cw_first = 1;
}

static void cw_block(void)
{
	uint32_t i, t0 = now_ms();
	float I = 0, Q = 0, dc = 0, resid;

	us_retune();

	for (i = 0; i < US_BLOCK; i++) {
		while (!(ADC1(ADC_HS) & 1u)) ;
		cnt[i] = (uint16_t)PWM2(PWM_CNR);
		rx[i] = (uint16_t)ADC1(ADC_R0);
	}
	for (i = 0; i < US_BLOCK; i++) dc += rx[i];
	dc /= US_BLOCK;
	for (i = 0; i < US_BLOCK; i++) {
		uint32_t p = cnt[i] % us_period;
		I += (rx[i] - dc) * cos_tab[p];
		Q -= (rx[i] - dc) * sin_tab[p];
	}
	I = 2 * I / US_BLOCK; Q = 2 * Q / US_BLOCK;

	if (cw_first) { bI = I; bQ = Q; cw_first = 0; active = 0; }
	resid = my_hypotf(I - bI, Q - bQ);
	float alpha = g_shared.a9_alpha_u ? g_shared.a9_alpha_u * 1e-6f : US_ALPHA;
	bI += alpha * (I - bI);
	bQ += alpha * (Q - bQ);
	g_shared.us_carrier_m = (int32_t)(my_hypotf(bI, bQ) * 1000);	/* TX check */

	static float peak; static uint32_t peak_t0;
	if (resid > peak) peak = resid;
	if (now_ms() - peak_t0 >= 1000) {
		g_shared.us_peak_m = (int32_t)(peak * 1000);
		peak = 0; peak_t0 = now_ms();
	}

	float dt = (now_ms() - t0) / 1000.0f;
	if (dt <= 0 || dt > 1) dt = 0.02f;

	if (g_shared.a9_thresh_m) cw_thresh = g_shared.a9_thresh_m / 1000.0f;
	int moving = resid > cw_thresh;
	int rising = resid > prev_resid * 1.05f;
	prev_resid = resid;
	if (moving) {
		active += dt * (rising ? 1.5f : 1.0f);
	} else {
		active -= dt;			/* symmetric: a room with <50 % "moving" blocks cannot climb */
		noise_floor += US_NOISE_A * (resid - noise_floor);
		float t = US_THRESH_K * noise_floor;
		if (!g_shared.a9_thresh_m)
			cw_thresh = t > US_THRESH_MIN ? t : US_THRESH_MIN;
	}
	if (active < 0) active = 0;
	if (active > 2) active = 2;

	if (!us_on && active > US_SUSTAIN) { us_on = 1; us_wakes++; off_timer = US_HOLD; }
	if (us_on) {
		if (moving) off_timer = US_HOLD; else off_timer -= dt;
		if (off_timer <= 0) { us_on = 0; active = 0; }
	}
	/* learning (CTRL_LEARN only): a touch without a wake before it was a
	   missed approach -> lower the threshold toward the last second's peak,
	   never below US_THRESH_MIN. A wake without a touch is NOT taken as a
	   mistake: people walk up to read the display and leave. */
	if (g_shared.touch_seq != last_touch_seq) {
		last_touch_seq = g_shared.touch_seq;
		if (learn && !us_on && !g_shared.a9_thresh_m) {
			float want = g_shared.us_peak_m / 1000.0f * 0.8f;
			if (want < cw_thresh) cw_thresh = 0.7f * cw_thresh + 0.3f * want;
			if (cw_thresh < US_THRESH_MIN) cw_thresh = US_THRESH_MIN;
		}
	}

	g_shared.us_resid_m = (int32_t)(resid * 1000);
	g_shared.us_active_m = (int32_t)(active * 1000);
	g_shared.us_thresh_m = (int32_t)(cw_thresh * 1000);
	g_shared.us_wake = us_on;
	g_shared.us_wake_cnt = us_wakes;
	g_shared.us_seq++;
}

/* ---------- ultrasonic: pulsed ranging (method 2) ---------- */
/* A burst of a few 40 kHz cycles, then listen: each range bin is demodulated
 * coherently against the PWM counter (same units as the CW residual: 40 kHz
 * amplitude in ADC counts, still-room floor ~0.06, a person 0.3-1.8), a few
 * pings are averaged, and the result is compared with a slowly learned
 * profile of the still room. A new echo inside the range gate is a person;
 * its nearest bin is the distance. */
#define NB		40		/* range bins (<= M4_DIAG_NB)          */
#define BIN		64		/* samples per bin: 0.27 ms, 4.66 cm   */
#define LISTEN		(NB * BIN)	/* 2560 samples, ~10.9 ms, ~1.86 m     */
#define FS_HZ		235500u		/* measured ADC1 continuous rate       */
#define CM_PER_BIN	(34300.0f * BIN / FS_HZ / 2)	/* round trip -> 4.66 */
#define P_AVG_A		0.125f		/* ping-to-ping averaging (~8 pings, 0.4 s) */
#define P_BASE_A	0.002f		/* profile baseline adapt per ping     */
#define P_NOISE_A	0.01f		/* still-room deviation adapt (no echo)*/
#define P_MINEXC	0.15f		/* minimum excess, counts              */
#define P_MINEXC_FLOOR	(P_MINEXC / 2)	/* learning may lower it to half, no more */
#define P_HIST		20		/* pings in the presence vote          */
#define P_VOTE		12		/* present pings (of P_HIST) to wake   */
#define P_FREEZE	2		/* bins either side of an echo kept out of the profile update */
#define P_STUCK_PINGS	(10 * 60 * 20)	/* "presence" this long without a touch: re-learn */
static float amp[NB], base[NB], pnoise[NB], exc[NB];
static uint8_t hot[NB];			/* this ping's echo bins               */
static int   ping_init;
static float min_exc = P_MINEXC;	/* lowered by touches (CTRL_LEARN only) */
static uint32_t present_hist, stuck_pings, relearns;
static float ring[64];			/* strongest gated excess, last ~3 s   */
static int   ring_i;

/* The factory test reads the receiver with IO16 HIGH and the transmitter
 * off, and transmits with IO16 LOW: IO16 looks like a transmit/receive
 * switch. With CTRL_RX_IO16 the listen window is done in that receive state. */
static int rx_io16;
static void burst(uint32_t cycles)
{
	uint32_t prev = PWM2(PWM_CNR) & 0xffff, n = 0, guard = 0;
	if (rx_io16) GPIO5(GPIO_DR) &= ~(1u << 16);	/* transmit state */
	us_tx(1);
	while (n < cycles && guard++ < 200000) {
		uint32_t c = PWM2(PWM_CNR) & 0xffff;
		if (c < prev) n++;
		prev = c;
	}
	us_tx(0);
	if (rx_io16) GPIO5(GPIO_DR) |= (1u << 16);	/* receive state */
}

static void ping(void)
{
	uint32_t b, i;
	uint32_t cycles = g_shared.a9_burst_cycles ? g_shared.a9_burst_cycles : 16;
	float dt = (g_shared.a9_ping_ms ? g_shared.a9_ping_ms : 50) / 1000.0f;

	burst(cycles);
	(void)ADC1(ADC_R0);				/* drop the stale sample */
	for (i = 0; i < LISTEN; i++) {
		while (!(ADC1(ADC_HS) & 1u)) ;
		cnt[i] = (uint16_t)PWM2(PWM_CNR);
		rx[i] = (uint16_t)ADC1(ADC_R0);
	}
	{	/* TX check: the burst's own ringing right after it (~1000 counts
		   peak-to-peak when transmitting, ~6 at rest) */
		uint16_t mn = rx[0], mx = rx[0];
		for (i = 1; i < BIN; i++) { if (rx[i] < mn) mn = rx[i]; if (rx[i] > mx) mx = rx[i]; }
		g_shared.us_tx_pp = mx - mn;
	}
	for (b = 0; b < NB; b++) {
		float dc = 0, I = 0, Q = 0;
		uint16_t *x = rx + b * BIN, *c = cnt + b * BIN;
		for (i = 0; i < BIN; i++) dc += x[i];
		dc /= BIN;
		for (i = 0; i < BIN; i++) {
			uint32_t p = c[i] % us_period;
			I += (x[i] - dc) * cos_tab[p];
			Q -= (x[i] - dc) * sin_tab[p];
		}
		float a = 2 * my_hypotf(I, Q) / BIN;
		amp[b] = ping_init ? amp[b] + P_AVG_A * (a - amp[b]) : a;
	}
	if (!ping_init) {
		for (b = 0; b < NB; b++) { base[b] = amp[b]; pnoise[b] = 0.1f; exc[b] = 0; }
		ping_init = 1;
		return;
	}

	uint32_t gmin = (g_shared.a9_gate_min_cm ? g_shared.a9_gate_min_cm : 35) / CM_PER_BIN;
	uint32_t gmax = (g_shared.a9_gate_max_cm ? g_shared.a9_gate_max_cm : 180) / CM_PER_BIN;
	if (gmax >= NB) gmax = NB - 1;
	if (gmin > gmax) gmin = gmax;
	float K = g_shared.a9_k_milli ? g_shared.a9_k_milli / 1000.0f : 3.5f;
	float me = g_shared.a9_thresh_m ? g_shared.a9_thresh_m / 1000.0f : min_exc;

	int present = 0, near = -1;
	float score = 0, maxexc = 0;
	for (b = 0; b < NB; b++) { exc[b] = amp[b] - base[b]; hot[b] = 0; }
	for (b = gmin; b <= gmax; b++) {
		if (exc[b] > K * pnoise[b] + me) {
			present = 1;
			hot[b] = 1;
			if (near < 0) near = b;
			score += exc[b];
			if (exc[b] > maxexc) maxexc = exc[b];
		}
	}
	/* The still-room profile follows slow changes (temperature, a chair
	   moved) but not a person: bins at and around an echo are frozen, so
	   someone standing still does not fade into the background. */
	for (b = 0; b < NB; b++) {
		int frozen = 0;
		for (int k = -P_FREEZE; k <= P_FREEZE; k++)
			if ((int)b + k >= 0 && (int)b + k < NB && hot[b + k]) frozen = 1;
		if (!frozen)
			base[b] += P_BASE_A * (amp[b] - base[b]);
		if (!present)
			pnoise[b] += P_NOISE_A * (my_fabsf(exc[b]) - pnoise[b]);
	}
	present_hist = (present_hist << 1) | (present ? 1u : 0u);
	int votes = 0;
	for (i = 0; i < P_HIST; i++) if (present_hist & (1u << i)) votes++;
	ring[ring_i] = maxexc; ring_i = (ring_i + 1) & 63;

	/* a "presence" that lasts ten minutes without anyone touching the
	   screen is the room that changed (door, furniture): re-learn it */
	stuck_pings = votes >= P_VOTE ? stuck_pings + 1 : 0;
	if (stuck_pings > P_STUCK_PINGS) {
		for (b = 0; b < NB; b++) base[b] = amp[b];
		stuck_pings = 0; present_hist = 0; relearns++;
	}

	if (!us_on && votes >= P_VOTE) { us_on = 1; us_wakes++; off_timer = US_HOLD; }
	if (us_on) {
		if (present) off_timer = US_HOLD; else off_timer -= dt;
		if (off_timer <= 0) us_on = 0;
	}
	if (g_shared.touch_seq != last_touch_seq) {
		last_touch_seq = g_shared.touch_seq;
		stuck_pings = 0;		/* someone is really there */
		/* learning (CTRL_LEARN only): a touch without a wake before it
		   was a missed approach -> lower the minimum excess toward the
		   strongest echo of the last ~3 s, to half its start at most. A
		   wake without a touch is NOT taken as a mistake: people walk
		   up to read the display and leave. */
		if (learn && !us_on && !g_shared.a9_thresh_m) {
			float peak = 0;
			for (i = 0; i < 64; i++) if (ring[i] > peak) peak = ring[i];
			float want = peak * 0.8f;
			if (want > 0 && want < min_exc) min_exc = 0.7f * min_exc + 0.3f * want;
			if (min_exc < P_MINEXC_FLOOR) min_exc = P_MINEXC_FLOOR;
		}
	}
	g_shared.us_relearns = relearns;

	/* publish */
	g_shared.us_resid_m = (int32_t)(maxexc * 1000);
	g_shared.us_score_m = (int32_t)(score * 1000);
	g_shared.us_active_m = votes * 1000 / P_HIST;
	g_shared.us_present_pct = votes * 100 / P_HIST;
	g_shared.us_thresh_m = (int32_t)(me * 1000);
	g_shared.us_learned_m = (uint32_t)(min_exc * 1000);
	g_shared.us_range_cm = near >= 0 ? (uint32_t)(near * CM_PER_BIN) : 0;
	g_shared.us_near_bin = near >= 0 ? (uint32_t)near : 0;
	g_shared.us_wake = us_on;
	g_shared.us_wake_cnt = us_wakes;
	g_shared.us_ping_seq++;
	g_shared.us_seq++;
	/* profile for tuning (m4load profile), amplitudes * 100 (int16: up to 327 counts) */
	g_diag.nbins = NB; g_diag.bin_samples = BIN; g_diag.fs_hz = FS_HZ;
	g_diag.cm_per_bin_x100 = (uint32_t)(CM_PER_BIN * 100);
	g_diag.gate_min_bin = gmin; g_diag.gate_max_bin = gmax;
	for (b = 0; b < NB; b++) {
		g_diag.amp[b] = (int16_t)(amp[b] * 100);
		g_diag.base[b] = (int16_t)(base[b] * 100);
		g_diag.noise[b] = (int16_t)(pnoise[b] * 100);
		g_diag.excess[b] = (int16_t)(exc[b] * 100);
	}
	g_diag.ping_seq = g_shared.us_ping_seq;
	g_diag.magic = M4_DIAG_MAGIC;
}

/* ---------- VOC ---------- */
#define VOC_IO18	(1u << 18)
static void voc_init(void)
{
	/* PWM5 heater: 27.5 kHz, 18 % (at the 66 MHz ipg clock) */
	PWM5(PWM_CR) = 0;
	PWM5(PWM_CR) = (1u << 16);
	PWM5(PWM_PR) = 0x95e;
	PWM5(PWM_SAR) = 0x1b0;
	PWM5(PWM_CR) |= 1u;
	GPIO5(GPIO_GDIR) |= VOC_IO18;
	GPIO5(GPIO_DR)   |= VOC_IO18;			/* idle high */
}
/* One conversion: strobe low, settle, wait
 * for the heater PWM's phase window in a fresh period, convert, strobe high. */
#define VOC_WIN_LO	0x77f
#define VOC_WIN_HI	0x8f9
static int voc_in_window(void)
{
	uint32_t c = PWM5(PWM_CNR) & 0xffff;
	return c >= VOC_WIN_LO && c <= VOC_WIN_HI;
}
static int voc_convert(uint16_t *v)
{
	int g;
	GPIO5(GPIO_DR) &= ~VOC_IO18;			/* strobe low */
	for (volatile int d = 0; d < 2000; d++) ;
	for (g = 0; g < 100000 && voc_in_window(); g++) ;	/* leave this window */
	for (g = 0; g < 100000 && !voc_in_window(); g++) ;	/* the next one */
	ADC2(ADC_HC0) = 0;
	for (g = 0; g < 1000000 && !(ADC2(ADC_HS) & 1u); g++) ;
	*v = (uint16_t)ADC2(ADC_R0);
	GPIO5(GPIO_DR) |= VOC_IO18;
	return g < 1000000;
}
/* The published sample is the median of VOC_N such conversions (one heater
 * period apart, ~1 ms in all): a single conversion now and then catches a
 * spike, the median does not move for it. */
#define VOC_N		7
static uint16_t voc_sample(void)
{
	uint16_t s[VOC_N], t;
	int n = 0;

	ADC2(ADC_GC) = ADC2(ADC_GC) & ~0x40u;		/* software trigger mode */
	for (int i = 0; i < VOC_N; i++)
		if (voc_convert(&s[n])) n++;
	if (!n)
		return 0;
	for (int i = 1; i < n; i++)			/* insertion sort */
		for (int j = i; j > 0 && s[j - 1] > s[j]; j--) { t = s[j]; s[j] = s[j - 1]; s[j - 1] = t; }
	g_shared.voc_spread = s[n - 1] - s[0];
	return s[n / 2];
}

/* ---------- main ---------- */
int main(void)
{
	uint32_t voc_next, ping_next, ctrl;

	/* a stale enable from a previous run must not start the sensors
	   before Linux is ready: the A9 region starts clean */
	g_shared.a9_ctrl = 0; g_shared.a9_echo = 0; g_shared.touch_seq = 0; g_shared.a9_us_period = 0;
	g_shared.a9_snd_wr = 0; g_shared.snd_rate = 0;
	g_shared.a9_thresh_m = 0; g_shared.a9_alpha_u = 0;
	g_shared.a9_gate_min_cm = 0; g_shared.a9_gate_max_cm = 0;
	g_shared.a9_k_milli = 0; g_shared.a9_burst_cycles = 0; g_shared.a9_ping_ms = 0;
	g_shared.a9_test = 0;
	g_shared.us_mode = 0; g_shared.fault_flag = 0;
	g_diag.magic = 0;
	g_shared.magic = M4_MAGIC;
	g_shared.fw_version = M4_FW_VERSION;
	g_shared.heartbeat = 0;
	systick_init();

	while (!((ctrl = g_shared.a9_ctrl) & (CTRL_VOC_EN | CTRL_US_EN | CTRL_SND_EN))) {
		g_shared.heartbeat++;
		g_shared.uptime_ms = now_ms();
		g_shared.echo_rsp = g_shared.a9_echo ^ 0xFFFFu;
	}
	int us = !!(ctrl & CTRL_US_EN), voc = !!(ctrl & CTRL_VOC_EN);
	int pulsed = !!(ctrl & CTRL_US_PULSED), own = !!(ctrl & CTRL_OWN_PERIPH);
	learn = !!(ctrl & CTRL_LEARN);
	rx_io16 = !!(ctrl & CTRL_RX_IO16);

	pads_init(us, voc);
	if (own) {
		clocks_own(us, voc);
		if (us)  adc_init_own(ADC1_BASE);
		if (voc) {
			adc_init_own(ADC2_BASE);
			ADC2(ADC_GC) |= 0x20u;	/* hardware averaging, as Linux had it */
		}
	}
	g_shared.us_own = own;
	if (us) {
		us_pwm_setup();
		us_adc_init();
		us_build_tables();
		bI = bQ = 0;
		if (pulsed) us_tx(0);		/* transmit only during bursts */
		g_shared.us_mode = pulsed ? 2 : 1;
	}
	if (voc)
		voc_init();
	if (ctrl & CTRL_SND_EN)
		snd_init();

	voc_next = now_ms() + 1000;
	ping_next = now_ms() + 100;

	for (;;) {
		g_shared.heartbeat++;
		g_shared.uptime_ms = now_ms();

		/* deliberate failures, so the A9 supervisor can be tested */
		if (g_shared.a9_test == A9_TEST_FAULT)
			__asm__ volatile("udf #0");
		if (g_shared.a9_test == A9_TEST_HANG)
			for (;;)
				;

		if (us) {
			if (pulsed) {
				if ((int32_t)(now_ms() - ping_next) >= 0) {
					ping();
					ping_next += g_shared.a9_ping_ms ? g_shared.a9_ping_ms : 50;
					if ((int32_t)(now_ms() - ping_next) > 500)
						ping_next = now_ms();
				}
			} else {
				cw_block();
			}
		}
		if (voc && (int32_t)(now_ms() - voc_next) >= 0) {
			g_shared.voc_raw = voc_sample();
			g_shared.voc_seq++;
			voc_next += 1000;
		}
	}
}
