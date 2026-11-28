/* SPDX-License-Identifier: GPL-2.0-or-later
 * i.MX6SX Cortex-M4 peripheral map for the Toon 2 sensor firmware.
 *
 * The M4 reaches the SoC peripherals through its AIPS alias at 0x42xxxxxx
 * (the A9 sees the same blocks at 0x02xxxxxx). Bases below follow the
 * i.MX6SX reference manual and the A9-side device tree.
 *
 * Memory map (the M4's tightly-coupled memory):
 *   TCML (code) 0x1FFF8000, 32 KiB   - .vectors/.text/.rodata
 *   TCMU (data) 0x20000000, 32 KiB   - .data/.bss/stack, SP = 0x20008000
 */
#ifndef IMX6SX_M4_H
#define IMX6SX_M4_H

#include <stdint.h>

#define REG32(a)	(*(volatile uint32_t *)(a))

/* --- peripheral bases, M4 (0x42xxxxxx) alias --- */
#define CCM_BASE	0x420c4000u	/* clock gating (CCGRn) */
#define SRC_BASE	0x420d8000u	/* reset / M4 control */
#define IOMUXC_BASE	0x420e0000u	/* pad mux + pad ctl */
#define GPIO5_BASE	0x420ac000u	/* IO16/17 ultrasonic enable, IO18 VOC strobe */
#define ADC1_BASE	0x42280000u	/* ultrasonic receiver (ch0) */
#define ADC2_BASE	0x42284000u	/* VOC / CCS801 (ch0) */
#define PWM2_BASE	0x42084000u	/* ultrasonic 40 kHz transmit */
#define PWM5_BASE	0x422a4000u	/* VOC heater 27.5 kHz */
#define MU_B_BASE	0x4229c000u	/* Messaging Unit, M4 (B) side - CONFIRM offset map */

/* --- ADC (vf610-adc style) register offsets --- */
#define ADC_HC0		0x00	/* channel/trigger */
#define ADC_HS		0x08	/* status (bit0 = conversion complete) */
#define ADC_R0		0x0c	/* result */
#define ADC_CFG		0x14
#define ADC_GC		0x18	/* bit6 continuous, bit5 average-enable */

/* --- PWM (i.MX) register offsets --- */
#define PWM_CR		0x00
#define PWM_SR		0x04
#define PWM_SAR		0x0c	/* sample (duty) */
#define PWM_PR		0x10	/* period */
#define PWM_CNR		0x14	/* counter (phase reference) */

/* --- GPIO register offsets --- */
#define GPIO_DR		0x00
#define GPIO_GDIR	0x04
#define GPIO_PSR	0x08

/* --- MU register offsets (to confirm against the M4-side map tomorrow) --- */
#define MU_ATR0		0x00	/* transmit registers (A side names; B side mirrors) */
#define MU_ARR0		0x10	/* receive registers */
#define MU_ASR		0x20	/* status */
#define MU_ACR		0x24	/* control */

#endif /* IMX6SX_M4_H */
