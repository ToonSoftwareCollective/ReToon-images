/* SPDX-License-Identifier: GPL-2.0-or-later
 * i.MX6SX Cortex-M4 (Toon 2 sensor firmware) -- runs from TCM.
 * Addresses from the i.MX6SX memory map and the kernel imx_rproc
 * address-translation table:
 *   TCML (code) 0x1FFF8000  32 KiB   (A9 alias 0x007F8000)
 *   TCMU (data) 0x20000000  32 KiB   (A9 alias 0x00800000, stack top 0x20008000)
 *
 * The first 256 bytes of TCMU are the ".shared" mailbox (see shared.h): a
 * fixed, NOLOAD region the A9 reads at 0x00800000. .data/.bss/stack follow.
 */
MEMORY
{
	TCML (rx) : ORIGIN = 0x1FFF8000, LENGTH = 32K
	TCMU (rw) : ORIGIN = 0x20000000, LENGTH = 32K
}

_estack = ORIGIN(TCMU) + LENGTH(TCMU);		/* 0x20008000 */

SECTIONS
{
	.vectors : { KEEP(*(.vectors)) } > TCML
	.text : {
		*(.text*)
		*(.rodata*)
		. = ALIGN(4);
		_etext = .;
	} > TCML

	/* fixed A9<->M4 mailbox at TCMU base 0x20000000 (256 bytes), the
	   diagnostics block at 0x20000100 (1 KiB reserved together) and the
	   4 KiB sound ring at 0x20000400. Not loaded, not zeroed by startup
	   (the M4 writes magic/heartbeat itself). */
	.shared (NOLOAD) : {
		. = ALIGN(4);
		KEEP(*(.shared))
		. = ORIGIN(TCMU) + 256;
		KEEP(*(.shared.diag))
		. = ORIGIN(TCMU) + 1024;
		KEEP(*(.shared.snd))
		. = ORIGIN(TCMU) + 1024 + 4096;
	} > TCMU

	.data : AT (_etext) {
		. = ALIGN(4);
		_sdata = .;
		*(.data*)
		. = ALIGN(4);
		_edata = .;
	} > TCMU
	_sidata = LOADADDR(.data);

	/* own load segment (LMA = VMA) so an ELF loader zeroes it in TCMU
	   instead of extending .data's TCML image */
	.bss : AT (ADDR(.bss)) {
		. = ALIGN(4);
		_sbss = .;
		*(.bss*)
		*(COMMON)
		. = ALIGN(4);
		_ebss = .;
	} > TCMU
}
