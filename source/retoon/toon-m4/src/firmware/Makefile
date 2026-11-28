# SPDX-License-Identifier: GPL-2.0-or-later
# Toon 2 Cortex-M4 sensor firmware. Bare-metal, runs from TCM.
# Toolchain: ARM GNU arm-none-eabi 13.2.rel1 (set TCDIR to its bin, or put it
# on PATH). The Buildroot package fetches and unpacks it itself (toon-m4.mk).

TCDIR ?=
CROSS  = $(if $(TCDIR),$(TCDIR)/)arm-none-eabi-
CC     = $(CROSS)gcc
OBJCOPY= $(CROSS)objcopy
SIZE   = $(CROSS)size

CPU    = -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
CFLAGS = $(CPU) -Os -ffreestanding -fno-tree-loop-distribute-patterns \
         -ffunction-sections -fdata-sections -Wall -Wextra -std=gnu11 -g
LDFLAGS= $(CPU) -nostdlib -nostartfiles -Wl,--gc-sections -T linker.ld

# Phase A bring-up firmware (startup + bringup.c) and Phase B real firmware.
BRINGUP = toon2-m4-bringup
FIRMWARE= toon2-m4

all: $(BRINGUP).bin $(FIRMWARE).bin

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BRINGUP).elf: startup.o bringup.o linker.ld
	$(CC) $(LDFLAGS) -Wl,-Map=$(BRINGUP).map startup.o bringup.o -o $@
	$(SIZE) $@

$(FIRMWARE).elf: startup.o main.o linker.ld
	$(CC) $(LDFLAGS) -Wl,-Map=$(FIRMWARE).map startup.o main.o -o $@
	$(SIZE) $@

%.bin: %.elf
	$(OBJCOPY) -O binary $< $@
	@echo "built $@ ($$(stat -c%s $@) bytes), loads to TCML 0x1FFF8000 (A9 0x007F8000)"

startup.o: startup.c
bringup.o: bringup.c shared.h imx6sx.h
main.o:    main.c shared.h imx6sx.h

clean:
	rm -f *.o *.elf *.bin *.map

.PHONY: all clean
