// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * m4load: load/start/stop the i.MX6SX Cortex-M4 and read its shared mailbox,
 * from Linux user space via /dev/mem. A minimal, self-contained replacement
 * for NXP's m4fwloader, using the exact SRC bits the kernel imx_rproc driver
 * uses for "fsl,imx6sx-cm4" (read-modify-write within M4_RST_MASK only, so no
 * other SoC reset domain is disturbed).
 *
 *   m4load <firmware.bin>   enable clocks, hold, load to TCML, start, verify
 *   m4load start | stop     release / hold the M4 core (no reload)
 *   m4load disable          full stop (clears ENABLE_M4)
 *   m4load status           dump the TCMU mailbox (0x00800000)
 *   m4load echo <n>         write a9_echo=n, read back echo_rsp (handshake)
 *   m4load clk|peek|poke    step-wise bring-up: clocks only / read one TCM
 *                           word / write+readback one TCM word
 *
 * All register writes are RMW; the only SRC_SCR bits touched are 0x00401018.
 * Loading to TCM requires the M4 clock CHAIN enabled first (pll3_pfd3 is
 * gated on ReToon -- see enable_clocks). RAM only -- nothing is flashed.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* ---- i.MX6SX physical addresses (A9 view) ---- */
#define CCM_BASE	0x020C4000u
#define CCM_CCGR3	0x074u		/* M4 clock gate = CG1 (bits 3:2) */
/* The M4 clock root is m4_podf <- m4_sel <- m4_pre_sel <- pll3_pfd3_454m.
 * ReToon's kernel has no consumer for pll3_pfd3, so that PFD is left
 * CLOCK-GATED. Ungating CCGR3 alone then feeds the M4 platform a dead clock
 * and the A9 hangs on its first TCM access (this, not ENABLE_M4, is what
 * froze .180 twice on 2026-10-23). Ungate the PFD first, like the kernel's
 * clk-pfd enable does: write the gate bit to the CLR register, wait STABLE. */
#define ANALOG_BASE	0x020C8000u
#define PFD_480		0x0F0u		/* CCM_ANALOG_PFD_480n (PFD3 = byte 3) */
#define PFD_480_CLR	0x0F8u
#define PFD3_CLKGATE	(1u << 31)
#define PFD3_STABLE	(1u << 30)
#define SRC_BASE	0x020D8000u
#define SRC_SCR		0x000u
#define TCML_A9		0x007F8000u	/* M4 TCML (code) alias, 32 KiB   */
#define TCMU_A9		0x00800000u	/* M4 TCMU (data/mailbox) alias   */
#define TCM_LEN		0x8000u

/* kernel imx_rproc "fsl,imx6sx-cm4" SRC bits */
#define ENABLE_M4		(1u << 22)
#define SW_M4P_RST		(1u << 12)
#define SW_M4C_NON_SCLR_RST	(1u << 4)
#define SW_M4C_RST		(1u << 3)
#define M4_START	(ENABLE_M4 | SW_M4P_RST | SW_M4C_RST)	/* 0x00401008 */
#define M4_STOP		(SW_M4C_NON_SCLR_RST)			/* 0x00000010 full stop */
/* HOLD = core held in reset but ENABLE_M4 KEPT SET. TCM is only accessible
 * from the A9 while ENABLE_M4 is set, so the load path must hold the core this
 * way, never with M4_STOP (which clears ENABLE_M4 and hangs the A9 bus on the
 * first TCM access -- confirmed the hard way, matches NXP's forum warning). */
#define M4_HOLD		(ENABLE_M4 | SW_M4C_NON_SCLR_RST)	/* 0x00400010 */
#define M4_RST_MASK	(ENABLE_M4 | SW_M4P_RST | SW_M4C_NON_SCLR_RST | SW_M4C_RST)

#define CCGR3_M4_GATE	0x0000000Cu	/* CG1 = bits 3:2 -> 11 = on */

/* mailbox field offsets (must match shared.h) */
#define OFF_MAGIC	0x00
#define OFF_FWVER	0x04
#define OFF_HEARTBEAT	0x08
#define OFF_UPTIME	0x0c
#define OFF_VOC_SEQ	0x10
#define OFF_VOC_RAW	0x14
#define OFF_US_SEQ	0x18
#define OFF_US_RESID	0x1c
#define OFF_US_ACTIVE	0x20
#define OFF_US_WAKE	0x24
#define OFF_US_WAKECNT	0x28
#define OFF_US_THRESH	0x2c
#define OFF_ECHO_RSP	0x30
#define OFF_FAULT	0x34		/* flag, cfsr, hfsr, bfar, pc, lr */
#define M4_FAULT_FLAG	0xFA17FA17u
#define OFF_A9_CTRL	0x50
#define OFF_A9_ECHO	0x54
#define OFF_TOUCH_SEQ	0x58
#define M4_MAGIC	0xB007C0DEu

static int g_fd = -1;

static volatile uint32_t *mapreg(uint32_t phys, uint32_t len)
{
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, phys);
	if (p == MAP_FAILED) { perror("mmap"); exit(1); }
	return (volatile uint32_t *)p;
}

static void msleep(int ms)
{
	struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };
	nanosleep(&t, NULL);
}

/* Enable the whole M4 clock chain, root first: pll3_pfd3 (ungate + wait
 * STABLE, as the kernel's clk-pfd does), then the CCGR3 M4 gate. Every
 * register access is announced before it happens (stdout is unbuffered) so a
 * hang on the console pinpoints the access. */
static void enable_clocks(void)
{
	volatile uint32_t *an = mapreg(ANALOG_BASE, 0x1000);
	volatile uint32_t *ccm = mapreg(CCM_BASE, 0x1000);
	uint32_t v;
	int i = 0;

	printf("  reading PFD_480 ... ");
	v = an[PFD_480 / 4];
	printf("%08x: pll3_pfd3 %s\n", v,
	       v & PFD3_CLKGATE ? "GATED -> ungating" : "already on");
	if (v & PFD3_CLKGATE) {
		printf("  writing PFD_480_CLR (clkgate) ... ");
		an[PFD_480_CLR / 4] = PFD3_CLKGATE;
		/* STABLE *toggles* when the new state takes effect (RM: a
		 * diagnostic bit) -- wait for the toggle, don't require 1 */
		uint32_t st = v & PFD3_STABLE;
		for (i = 0; i < 1000 && (an[PFD_480 / 4] & PFD3_STABLE) == st; i++)
			usleep(10);
		v = an[PFD_480 / 4];
		printf("%08x (gate clear; STABLE %s after %d polls)\n", v,
		       (v & PFD3_STABLE) != st ? "toggled" : "unchanged", i);
		usleep(200);
	}
	printf("  reading CCGR3 ... ");
	v = ccm[CCM_CCGR3 / 4];
	printf("%08x; writing M4 gate on ... ", v);
	ccm[CCM_CCGR3 / 4] = v | CCGR3_M4_GATE;
	printf("%08x\n", ccm[CCM_CCGR3 / 4]);
	usleep(200);
	munmap((void *)an, 0x1000);
	munmap((void *)ccm, 0x1000);
}

static void src_set(uint32_t val)
{
	volatile uint32_t *src = mapreg(SRC_BASE, 0x1000);
	uint32_t old = src[SRC_SCR / 4];
	uint32_t new = (old & ~M4_RST_MASK) | (val & M4_RST_MASK);
	const char *name = val == M4_START ? "START" :
			   val == M4_HOLD  ? "HOLD (enable kept)" :
			   val == M4_STOP  ? "STOP (disable)" : "?";
	src[SRC_SCR / 4] = new;
	printf("  SRC_SCR %08x -> %08x (%s)\n", old, src[SRC_SCR / 4], name);
	munmap((void *)src, 0x1000);
}

static void load_firmware(const char *path)
{
	FILE *f = fopen(path, "rb");
	volatile uint32_t *tcml;
	uint8_t buf[TCM_LEN];
	size_t n, i;

	if (!f) { perror(path); exit(1); }
	n = fread(buf, 1, sizeof buf, f);
	fclose(f);
	if (n < 8) { fprintf(stderr, "firmware too small (%zu)\n", n); exit(1); }
	printf("  firmware %zu bytes; vector SP=%08x reset=%08x\n",
	       n, ((uint32_t *)buf)[0], ((uint32_t *)buf)[1]);

	tcml = mapreg(TCML_A9, TCM_LEN);
	for (i = 0; i < (n + 3) / 4; i++)
		tcml[i] = ((uint32_t *)buf)[i];
	__sync_synchronize();
	printf("  copied to TCML @ 0x%08x\n", TCML_A9);
	munmap((void *)tcml, TCM_LEN);
}

static volatile uint32_t *map_mailbox(void)
{
	return mapreg(TCMU_A9, 0x1000);
}

static void dump_status(volatile uint32_t *mb)
{
	printf("  magic     %08x %s\n", mb[OFF_MAGIC / 4],
	       mb[OFF_MAGIC / 4] == M4_MAGIC ? "(ALIVE)" : "(no M4_MAGIC)");
	printf("  fw_version%08x\n", mb[OFF_FWVER / 4]);
	printf("  heartbeat %u\n", mb[OFF_HEARTBEAT / 4]);
	printf("  uptime_ms %u\n", mb[OFF_UPTIME / 4]);
	printf("  voc  seq=%u raw=%u spread=%u\n", mb[OFF_VOC_SEQ / 4], mb[OFF_VOC_RAW / 4],
	       mb[0xa0 / 4]);
	printf("  us   seq=%u resid=%d active=%d wake=%u wakes=%u thr=%d\n",
	       mb[OFF_US_SEQ / 4], (int32_t)mb[OFF_US_RESID / 4],
	       (int32_t)mb[OFF_US_ACTIVE / 4], mb[OFF_US_WAKE / 4],
	       mb[OFF_US_WAKECNT / 4], (int32_t)mb[OFF_US_THRESH / 4]);
	printf("  us   peak(1s)=%d  a9_thresh=%u a9_alpha_u=%u\n",
	       (int32_t)mb[0x4c / 4], mb[0x60 / 4], mb[0x64 / 4]);
	printf("  us   tx_pp=%u carrier=%d relearns=%u learn=%u\n", mb[0xa8 / 4],
	       (int32_t)mb[0xac / 4], mb[0xa4 / 4], !!(mb[OFF_A9_CTRL / 4] & 0x10));
	printf("  us   mode=%u(%s) range=%u cm score=%d present=%u%% learned=%u ping=%u own=%u\n",
	       mb[0x88 / 4], mb[0x88 / 4] == 2 ? "pulsed" : mb[0x88 / 4] == 1 ? "cw" : "off",
	       mb[0x80 / 4], (int32_t)mb[0x84 / 4], mb[0x94 / 4], mb[0x98 / 4],
	       mb[0x8c / 4], mb[0x9c / 4]);
	printf("  a9_ctrl   %08x\n", mb[OFF_A9_CTRL / 4]);
	if (mb[OFF_FAULT / 4] == M4_FAULT_FLAG)
		printf("  *** M4 HARDFAULT: cfsr=%08x hfsr=%08x bfar=%08x pc=%08x lr=%08x\n",
		       mb[OFF_FAULT / 4 + 1], mb[OFF_FAULT / 4 + 2],
		       mb[OFF_FAULT / 4 + 3], mb[OFF_FAULT / 4 + 4],
		       mb[OFF_FAULT / 4 + 5]);
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "status";

	setvbuf(stdout, NULL, _IONBF, 0);	/* every line out before the next access */
	g_fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (g_fd < 0) { perror("/dev/mem"); return 1; }

	/* step-wise bring-up: one access class per process, check alive between */
	if (!strcmp(cmd, "regs")) {	/* reads only: clock/reset state */
		volatile uint32_t *an = mapreg(ANALOG_BASE, 0x1000);
		volatile uint32_t *ccm = mapreg(CCM_BASE, 0x1000);
		volatile uint32_t *src = mapreg(SRC_BASE, 0x1000);
		uint32_t pfd = an[PFD_480 / 4], cg = ccm[CCM_CCGR3 / 4];
		uint32_t cd = ccm[0x3C / 4], scr = src[SRC_SCR / 4];
		printf("  PFD_480  %08x  pll3_pfd3 %s\n", pfd,
		       pfd & PFD3_CLKGATE ? "GATED" : "on");
		printf("  CSCDR3   %08x  m4_sel=%u m4_podf=%u m4_pre_sel=%u\n", cd,
		       cd & 7, (cd >> 3) & 7, (cd >> 6) & 7);
		printf("  CCGR3    %08x  M4 gate %s\n", cg,
		       (cg & CCGR3_M4_GATE) == CCGR3_M4_GATE ? "on" : "OFF");
		printf("  SRC_SCR  %08x  ENABLE_M4=%u NON_SCLR_RST=%u\n", scr,
		       !!(scr & ENABLE_M4), !!(scr & SW_M4C_NON_SCLR_RST));
		/* SRC_SRSR: sticky reset-cause bits (W1C): 0 POR, 2 user/reset
		 * button, 4 WDOG1, 7 WDOG3, 16 warm boot */
		uint32_t srsr = src[0x08 / 4];
		printf("  SRC_SRSR %08x  last reset:%s%s%s%s%s\n", srsr,
		       srsr & 1 ? " POR" : "", srsr & 4 ? " USER" : "",
		       srsr & 0x10 ? " WDOG1" : "", srsr & 0x80 ? " WDOG3" : "",
		       srsr & 0x10000 ? " WARM" : "");
		return 0;
	}
	if (!strcmp(cmd, "clk")) {
		enable_clocks();
		printf("M4 clocks enabled (pll3_pfd3 + CCGR3), nothing else touched.\n");
		return 0;
	}
	if (!strcmp(cmd, "peek")) {
		volatile uint32_t *tcml, *tcmu;
		printf("  mapping TCML/TCMU\n");
		tcml = mapreg(TCML_A9, 0x1000);
		tcmu = mapreg(TCMU_A9, 0x1000);
		printf("  reading TCML[0] @%08x ... ", TCML_A9);
		printf("%08x\n", tcml[0]);
		printf("  reading TCMU[0] @%08x ... ", TCMU_A9);
		printf("%08x\n", tcmu[0]);
		printf("TCM readable.\n");
		return 0;
	}
	if (!strcmp(cmd, "poke")) {
		volatile uint32_t *tcmu = mapreg(TCMU_A9, 0x1000);
		uint32_t old;
		printf("  reading TCMU[63] ... ");
		old = tcmu[63];
		printf("%08x; writing 5a5a1234 ... ", old);
		tcmu[63] = 0x5A5A1234u;
		printf("read back %08x %s\n", tcmu[63],
		       tcmu[63] == 0x5A5A1234u ? "OK" : "MISMATCH");
		tcmu[63] = old;
		return 0;
	}

	if (!strcmp(cmd, "stop")) {
		src_set(M4_HOLD);	/* hold core, keep enable (reloadable) */
		printf("M4 held in reset (enable kept; reloadable).\n");
		return 0;
	}
	if (!strcmp(cmd, "disable")) {
		src_set(M4_STOP);	/* full stop: clears ENABLE_M4 */
		printf("M4 disabled (ENABLE_M4 cleared; must reload to run).\n");
		return 0;
	}
	if (!strcmp(cmd, "start")) {
		enable_clocks();
		src_set(M4_START);
		printf("M4 started (no reload).\n");
		return 0;
	}
	if (!strcmp(cmd, "status")) {
		dump_status(map_mailbox());
		return 0;
	}
	if (!strcmp(cmd, "ctrl")) {	/* a9_ctrl: bit0 = VOC, bit1 = ultrasonic */
		volatile uint32_t *mb = map_mailbox();
		uint32_t v = argc > 2 ? strtoul(argv[2], NULL, 0) : 0;
		mb[OFF_A9_CTRL / 4] = v;
		printf("  a9_ctrl=%08x (VOC %s, ultrasonic %s)\n", v,
		       v & 1 ? "on" : "off", v & 2 ? "on" : "off");
		return 0;
	}
	if (!strcmp(cmd, "dump")) {	/* dump <M4 TCMU address> <n uint16>: raw samples (e.g. lbuf) */
		uint32_t addr = argc > 2 ? strtoul(argv[2], NULL, 0) : 0;
		uint32_t n = argc > 3 ? strtoul(argv[3], NULL, 0) : 256;
		if (addr < 0x20000000u || addr + 2 * n > 0x20008000u) { fprintf(stderr, "TCMU address needed (0x2000xxxx)\n"); return 1; }
		volatile uint16_t *p = (volatile uint16_t *)((char *)mapreg(TCMU_A9, 0x8000) + (addr - 0x20000000u));
		for (uint32_t i = 0; i < n; i++) printf("%u%c", p[i], (i % 16 == 15) ? '\n' : ' ');
		if (n % 16) printf("\n");
		return 0;
	}
	if (!strcmp(cmd, "profile")) {	/* pulsed mode: the range profile */
		volatile uint32_t *mb = map_mailbox();
		volatile int16_t *d = (volatile int16_t *)(mb + 0x120 / 4);
		if (mb[0x100 / 4] != 0xD1A6B1A5u) { printf("no profile (pulsed mode not running)\n"); return 1; }
		uint32_t nb = mb[0x104 / 4], cm100 = mb[0x110 / 4];
		uint32_t gmin = mb[0x118 / 4], gmax = mb[0x11c / 4];
		printf("  ping %u: %u bins x %u samples, %u.%02u cm/bin, gate bins %u..%u; "
		       "amplitudes in 40 kHz counts (x100 stored)\n", mb[0x114 / 4], nb, mb[0x108 / 4],
		       cm100 / 100, cm100 % 100, gmin, gmax);
		printf("  bin    cm     amp    base   noise  excess\n");
		for (uint32_t b = 0; b < nb && b < 48; b++) {
			double a = d[b] / 100.0, bs = d[48 + b] / 100.0;
			double n = d[96 + b] / 100.0, e = d[144 + b] / 100.0;
			printf("  %2u  %5u  %7.2f %7.2f %7.2f %7.2f %s%s\n", b, b * cm100 / 100,
			       a, bs, n, e, (b >= gmin && b <= gmax) ? "|" : " ",
			       (b >= gmin && b <= gmax && e > 4 * n + 0.15) ? " <-- echo" : "");
		}
		return 0;
	}
	if (!strcmp(cmd, "set")) {	/* set <mailbox byte offset> <value>: raw A9-side field write */
		volatile uint32_t *mb = map_mailbox();
		uint32_t off = argc > 2 ? strtoul(argv[2], NULL, 0) : 0;
		uint32_t v = argc > 3 ? strtoul(argv[3], NULL, 0) : 0;
		if (off < 0x50 || off > 0xfc || (off & 3)) { fprintf(stderr, "offset must be 0x50..0xfc, word aligned\n"); return 1; }
		mb[off / 4] = v;
		printf("  [0x%02x] = %u (0x%x)\n", off, v, v);
		return 0;
	}
	if (!strcmp(cmd, "echo")) {
		volatile uint32_t *mb = map_mailbox();
		uint32_t v = argc > 2 ? strtoul(argv[2], NULL, 0) : 0x1234;
		mb[OFF_A9_ECHO / 4] = v;
		msleep(100);
		printf("  a9_echo=%08x  echo_rsp=%08x  expect=%08x  %s\n",
		       v, mb[OFF_ECHO_RSP / 4], v ^ 0xFFFFu,
		       mb[OFF_ECHO_RSP / 4] == (v ^ 0xFFFFu) ? "OK" : "MISMATCH");
		return 0;
	}

	/* default: <firmware.bin> -> full load + start + verify */
	{
		const char *path = cmd;
		volatile uint32_t *mb;
		uint32_t hb0, hb1;
		int alive;

		printf("Loading %s onto the M4:\n", path);
		enable_clocks();
		src_set(M4_HOLD);		/* hold in reset, KEEP enable so
						   TCM stays A9-accessible       */
		msleep(10);

		mb = map_mailbox();
		for (int i = 0; i < 64; i++) mb[i] = 0;	/* clear stale mailbox   */
		__sync_synchronize();

		load_firmware(path);
		src_set(M4_START);		/* release: boots from TCML[0/1] */

		msleep(200);
		alive = (mb[OFF_MAGIC / 4] == M4_MAGIC);
		hb0 = mb[OFF_HEARTBEAT / 4];
		msleep(300);
		hb1 = mb[OFF_HEARTBEAT / 4];

		printf("\nResult:\n");
		dump_status(mb);
		printf("  heartbeat %u -> %u over 300ms\n", hb0, hb1);
		if (alive && hb1 > hb0) {
			printf("\n*** M4 BRING-UP PASS: core runs, mailbox live. ***\n");
			return 0;
		}
		printf("\n*** M4 BRING-UP FAIL: magic=%s heartbeat %s. ***\n",
		       alive ? "ok" : "absent", hb1 > hb0 ? "moving" : "frozen");
		return 2;
	}
}
