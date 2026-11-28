/* SPDX-License-Identifier: GPL-2.0-or-later
 * Minimal Cortex-M4 startup for the Toon 2 sensor firmware: vector table,
 * reset handler (copy .data, zero .bss, call main), default fault trap.
 */
#include <stdint.h>

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
extern int main(void);

#define SCB_CPACR	(*(volatile uint32_t *)0xE000ED88)

void Reset_Handler(void)
{
	uint32_t *src = &_sidata, *dst = &_sdata;

	/* Enable the FPU (CP10/CP11 full access) before anything hard-float
	 * runs: without this the first VFP instruction is a UsageFault NOCP
	 * escalated to HardFault (seen on hardware 2026-10-23). */
	SCB_CPACR |= (0xFu << 20);
	__asm__ volatile("dsb\n\tisb");

	while (dst < &_edata)
		*dst++ = *src++;
	for (dst = &_sbss; dst < &_ebss; )
		*dst++ = 0;
	main();
	for (;;)
		;
}

void Default_Handler(void)
{
	for (;;)
		;
}

/* the compiler may still emit calls to these for loops it recognises */
void *memset(void *d, int c, unsigned n)
{
	uint8_t *p = d;
	while (n--) *p++ = (uint8_t)c;
	return d;
}
void *memcpy(void *d, const void *s, unsigned n)
{
	uint8_t *p = d; const uint8_t *q = s;
	while (n--) *p++ = *q++;
	return d;
}

/* weak aliases so unimplemented handlers fall through to the trap */
#define ALIAS __attribute__((weak, alias("Default_Handler")))
void NMI_Handler(void) ALIAS;
void HardFault_Handler(void) ALIAS;
void MemManage_Handler(void) ALIAS;
void BusFault_Handler(void) ALIAS;
void UsageFault_Handler(void) ALIAS;
void SVC_Handler(void) ALIAS;
void DebugMon_Handler(void) ALIAS;
void PendSV_Handler(void) ALIAS;
void SysTick_Handler(void) ALIAS;

__attribute__((section(".vectors"), used))
void (*const vectors[])(void) = {
	(void (*)(void))&_estack,	/* initial SP  = 0x20008000 */
	Reset_Handler,			/* reset       -> code in TCML */
	NMI_Handler,
	HardFault_Handler,
	MemManage_Handler,
	BusFault_Handler,
	UsageFault_Handler,
	0, 0, 0, 0,
	SVC_Handler,
	DebugMon_Handler,
	0,
	PendSV_Handler,
	SysTick_Handler,
	/* IRQ handlers appended tomorrow (MU, ADC) as needed */
};
