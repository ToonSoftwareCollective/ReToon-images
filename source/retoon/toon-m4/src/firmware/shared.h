/* SPDX-License-Identifier: GPL-2.0-or-later
 * Toon 2 M4 <-> A9 shared-memory mailbox.
 *
 * A fixed 256-byte struct at the very start of TCMU, followed by a 768-byte
 * diagnostics block (the ultrasonic range profile):
 *   M4 side : 0x20000000 / 0x20000100   (sections ".shared" / ".shared.diag")
 *   A9 side : 0x00800000 / 0x00800100   (same memory through the SoC
 *                                        address-translation table; map it
 *                                        with /dev/mem and poll)
 *
 * We use polled shared memory instead of the Messaging Unit: for a 1 Hz VOC
 * sample and the occasional wake event, Linux polling this region a few times
 * a second is simpler and more robust than MU interrupt plumbing.
 *
 * Single-writer discipline (no locks needed): the M4 owns every field up to
 * a9_ctrl and from 0x80 on; the A9 owns 0x50..0x7f. 32-bit aligned words are
 * written atomically on both cores. seq counters let the reader detect fresh
 * data. The M4 zeroes the A9 region once at reset, so a stale enable from a
 * previous run cannot start the sensors before Linux is ready.
 */
#ifndef TOON_M4_SHARED_H
#define TOON_M4_SHARED_H

#include <stdint.h>

#define M4_SHARED_M4ADDR	0x20000000u	/* M4 view (TCMU base)      */
#define M4_SHARED_A9ADDR	0x00800000u	/* A9 view (ATT alias)      */
#define M4_SHARED_SIZE		256u
#define M4_DIAG_M4ADDR		0x20000100u
#define M4_DIAG_A9ADDR		0x00800100u

#define M4_MAGIC		0xB007C0DEu	/* M4 writes this once alive */
#define M4_DIAG_MAGIC		0xD1A6B1A5u
#define M4_FAULT_FLAG		0xFA17FA17u	/* fault_flag: M4 took a HardFault */
#define M4_FW_VERSION		0x00020400u	/* 2.4.0: sound on the buzzer (PWM4 as a DAC, CTRL_SND_EN) */

struct m4_shared {
	/* ---- M4-owned: liveness ---- */
	volatile uint32_t magic;	/* 0x00  M4_MAGIC when running        */
	volatile uint32_t fw_version;	/* 0x04  M4_FW_VERSION                */
	volatile uint32_t heartbeat;	/* 0x08  M4 increments every loop     */
	volatile uint32_t uptime_ms;	/* 0x0c  coarse ms since M4 boot      */

	/* ---- M4-owned: VOC (air quality) ---- */
	volatile uint32_t voc_seq;	/* 0x10  ++ each new VOC sample       */
	volatile uint32_t voc_raw;	/* 0x14  latest ADC2 ch0 raw (12-bit) */

	/* ---- M4-owned: ultrasonic motion ---- */
	volatile uint32_t us_seq;	/* 0x18  ++ each block (CW) / ping (pulsed) */
	volatile int32_t  us_resid_m;	/* 0x1c  CW: residual*1000; pulsed: largest gated excess*1000 (ADC counts) */
	volatile int32_t  us_active_m;	/* 0x20  CW: sustain s*1000; pulsed: present pings of last 20, *1000/20 */
	volatile uint32_t us_wake;	/* 0x24  wake asserted (1)            */
	volatile uint32_t us_wake_cnt;	/* 0x28  total wake events since boot */
	volatile int32_t  us_thresh_m;	/* 0x2c  current threshold*1000       */

	/* ---- M4-owned: echo (bring-up handshake) ---- */
	volatile uint32_t echo_rsp;	/* 0x30  M4 writes a9_echo ^ 0xFFFF   */

	/* ---- M4-owned: fault report (HardFault handler fills these) ---- */
	volatile uint32_t fault_flag;	/* 0x34  M4_FAULT_FLAG when faulted   */
	volatile uint32_t fault_cfsr;	/* 0x38  SCB CFSR (bus/usage/mem)     */
	volatile uint32_t fault_hfsr;	/* 0x3c  SCB HFSR                     */
	volatile uint32_t fault_bfar;	/* 0x40  faulting bus address (if valid) */
	volatile uint32_t fault_pc;	/* 0x44  stacked PC                   */
	volatile uint32_t fault_lr;	/* 0x48  stacked LR                   */
	volatile int32_t  us_peak_m;	/* 0x4c  CW: max residual*1000 of the last ~1 s */

	/* ---- A9-owned: control / feedback / tuning (0x50..0x7f) ---- */
	volatile uint32_t a9_ctrl;	/* 0x50  CTRL_* bits                  */
	volatile uint32_t a9_echo;	/* 0x54  bring-up: A9 writes, M4 echoes */
	volatile uint32_t touch_seq;	/* 0x58  ++ by A9 on each screen touch */
	volatile uint32_t touch_ms;	/* 0x5c  A9 uptime_ms echo at touch   */
	volatile uint32_t a9_thresh_m;	/* 0x60  threshold override *1000 (0 = learned/adaptive) */
	volatile uint32_t a9_alpha_u;	/* 0x64  CW baseline EMA alpha *1e6 (0 = default) */
	volatile uint32_t a9_gate_min_cm;/* 0x68  pulsed: nearest range to look at (0 = default 35) */
	volatile uint32_t a9_gate_max_cm;/* 0x6c  pulsed: farthest range (0 = default 180) */
	volatile uint32_t a9_k_milli;	/* 0x70  pulsed: noise multiple K *1000 (0 = 4000) */
	volatile uint32_t a9_burst_cycles;/* 0x74 pulsed: 40 kHz cycles per burst (0 = 16) */
	volatile uint32_t a9_ping_ms;	/* 0x78  pulsed: ping period ms (0 = 50) */
	volatile uint32_t a9_test;	/* 0x7c  A9_TEST_*: deliberate fault/hang (supervisor tests) */

	/* ---- M4-owned: pulsed ranging results (0x80..) ---- */
	volatile uint32_t us_range_cm;	/* 0x80  nearest detected echo, cm (0 = none) */
	volatile int32_t  us_score_m;	/* 0x84  sum of gated excess *1000 (ADC counts) */
	volatile uint32_t us_mode;	/* 0x88  0 = off, 1 = CW coherent, 2 = pulsed ranging */
	volatile uint32_t us_ping_seq;	/* 0x8c  ++ per ping                  */
	volatile uint32_t us_near_bin;	/* 0x90  bin index of us_range_cm      */
	volatile uint32_t us_present_pct;/* 0x94 % of the last 20 pings with a detection */
	volatile uint32_t us_learned_m;	/* 0x98  learned minimum excess *1000 (touch learning) */
	volatile uint32_t us_own;	/* 0x9c  1 = firmware owns clocks/ADC init (CTRL_OWN_PERIPH) */

	/* ---- M4-owned: VOC sample quality ---- */
	volatile uint32_t voc_spread;	/* 0xa0  max - min of the VOC_N conversions behind voc_raw */

	/* ---- M4-owned: ultrasonic health ---- */
	volatile uint32_t us_relearns;	/* 0xa4  pulsed: profile re-learned after a stuck presence */
	volatile uint32_t us_tx_pp;	/* 0xa8  pulsed: raw peak-to-peak right after the burst (TX check) */
	volatile int32_t  us_carrier_m;	/* 0xac  CW: 40 kHz carrier amplitude *1000 (TX check) */

	/* ---- A9-owned tuning extension (the M4 only zeroes it at reset) ---- */
	volatile uint32_t a9_us_period;	/* 0xb0  PWM2 counts per cycle at 24 MHz (0 = 600 = 40.000 kHz); 400..1000 */

	/* ---- sound (CTRL_SND_EN): the ring at M4_SND_* ---- */
	volatile uint32_t a9_snd_wr;	/* 0xb4  A9: samples written (free-running) */
	volatile uint32_t snd_rd;	/* 0xb8  M4: samples played (free-running)  */
	volatile uint32_t snd_underruns;/* 0xbc  M4: ring ran dry mid-sound (a gap) */
	volatile uint32_t snd_rate;	/* 0xc0  M4: sample rate in Hz (0 = no sound) */
};

/* Sound: PWM4 drives the buzzer as a 1-bit DAC. 32 kHz carrier, each 8-bit
 * sample repeated twice, so exactly 16000 samples/s, paced by the PWM's own
 * FIFO. Samples are unsigned 8-bit (128 = middle); 0 = no current through the
 * buzzer, which is also what the M4 decays to when the ring is empty. The A9
 * writes ring[wr % M4_SND_SIZE] and then a9_snd_wr; it must stay at most
 * M4_SND_SIZE samples ahead of snd_rd. */
#define M4_SND_M4ADDR		0x20000400u
#define M4_SND_A9ADDR		0x00800400u
#define M4_SND_SIZE		4096u
#define M4_SND_RATE		16000u

/* a9_test values (anything else = no test) */
#define A9_TEST_FAULT	0xDEAD0001u	/* execute an undefined instruction -> HardFault */
#define A9_TEST_HANG	0xDEAD0002u	/* stop the main loop (heartbeat freezes) */

/* a9_ctrl bits */
#define CTRL_VOC_EN	(1u << 0)	/* sample the VOC sensor              */
#define CTRL_US_EN	(1u << 1)	/* run the ultrasonic detector        */
#define CTRL_US_PULSED	(1u << 2)	/* ... pulsed ranging instead of CW   */
#define CTRL_OWN_PERIPH	(1u << 3)	/* firmware ungates PWM clocks and initialises/calibrates the ADCs itself (Linux nodes disabled) */
#define CTRL_LEARN	(1u << 4)	/* touch learning on (off: fixed thresholds) */
#define CTRL_RX_IO16	(1u << 5)	/* pulsed: IO16 high while listening (the factory test's receive state) */
#define CTRL_SND_EN	(1u << 6)	/* sound on the buzzer: the firmware owns PWM4 (Linux pwm4 node disabled) */

/* Ultrasonic range profile, for tuning and tests (A9: m4load profile).
 * Amplitudes are 40 kHz amplitude per range bin in ADC counts * 100. */
#define M4_DIAG_NB	48
struct m4_diag {
	volatile uint32_t magic;	/* 0x100 M4_DIAG_MAGIC               */
	volatile uint32_t nbins;	/* 0x104 bins in use (<= M4_DIAG_NB) */
	volatile uint32_t bin_samples;	/* 0x108 ADC samples per bin         */
	volatile uint32_t fs_hz;	/* 0x10c assumed sample rate         */
	volatile uint32_t cm_per_bin_x100;/* 0x110 range per bin, cm*100     */
	volatile uint32_t ping_seq;	/* 0x114 profile of this ping        */
	volatile uint32_t gate_min_bin;	/* 0x118 */
	volatile uint32_t gate_max_bin;	/* 0x11c */
	volatile int16_t  amp[M4_DIAG_NB];	/* 0x120 this ping            */
	volatile int16_t  base[M4_DIAG_NB];	/* 0x180 slow baseline        */
	volatile int16_t  noise[M4_DIAG_NB];	/* 0x1e0 still-room deviation */
	volatile int16_t  excess[M4_DIAG_NB];	/* 0x240 amp - base           */
};						/* ends 0x2a0                 */

#endif /* TOON_M4_SHARED_H */
