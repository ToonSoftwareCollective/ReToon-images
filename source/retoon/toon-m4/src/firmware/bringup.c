/* SPDX-License-Identifier: GPL-2.0-or-later
 * Toon 2 M4 PHASE-A bring-up firmware.
 *
 * Deliberately minimal: touches NO peripherals (no clocks, no ADC/PWM/MU) so
 * it isolates one question -- does the M4 run code from TCML and can the A9
 * see its writes to the TCMU mailbox? It writes M4_MAGIC, counts a heartbeat,
 * and echoes a9_echo back inverted. If `m4load` sees magic appear and the
 * heartbeat climb, the core + loader + shared memory all work, and Phase B
 * (the real VOC + ultrasonic firmware in main.c) is cleared to proceed.
 */
#include "shared.h"

/* The mailbox lives at TCMU base (0x20000000); A9 reads it at 0x00800000. */
struct m4_shared g_shared __attribute__((section(".shared")));

static void busy(volatile uint32_t n) { while (n--) __asm__ volatile(""); }

int main(void)
{
	uint32_t last_echo;

	g_shared.magic = M4_MAGIC;
	g_shared.fw_version = M4_FW_VERSION;
	g_shared.heartbeat = 0;
	g_shared.uptime_ms = 0;
	g_shared.echo_rsp = 0;
	last_echo = g_shared.a9_echo;

	for (uint32_t i = 1; ; i++) {
		g_shared.heartbeat = i;

		/* bring-up handshake: reflect whatever the A9 wrote, inverted */
		uint32_t e = g_shared.a9_echo;
		if (e != last_echo) {
			g_shared.echo_rsp = e ^ 0xFFFFu;
			last_echo = e;
		}

		/* ~10 ms/loop (uncalibrated; only the heartbeat delta matters) */
		g_shared.uptime_ms += 10;
		busy(300000);
	}
}
