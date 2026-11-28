################################################################################
# toon-m4 (Cortex-M4 sensor co-processor: ultrasonic wake + VOC)
################################################################################
TOON_M4_VERSION = 2.0
TOON_M4_SITE = $(TOON_M4_PKGDIR)/src
TOON_M4_SITE_METHOD = local
TOON_M4_LICENSE = GPL-2.0+

# The M4 firmware (src/firmware) is bare metal, so it is built with ARM's
# arm-none-eabi toolchain (Buildroot's host-arm-gnu-toolchain), not the Linux one.
TOON_M4_DEPENDENCIES = host-arm-gnu-toolchain toon-bus

define TOON_M4_BUILD_CMDS
	$(MAKE) -C $(@D)/firmware TCDIR=$(HOST_DIR)/opt/gcc-arm-none-eabi/bin
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -o $(@D)/m4load $(@D)/m4load.c $(TARGET_LDFLAGS)
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -o $(@D)/toon-m4d $(@D)/toon-m4d.c $(TARGET_LDFLAGS) -lm -ltoonbus -lmosquitto -lpthread
endef

define TOON_M4_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/m4load $(TARGET_DIR)/usr/bin/m4load
	$(INSTALL) -D -m 0755 $(@D)/toon-m4d $(TARGET_DIR)/usr/bin/toon-m4d
	$(INSTALL) -D -m 0644 $(@D)/firmware/toon2-m4.elf \
		$(TARGET_DIR)/lib/firmware/toon2-m4.elf
	$(INSTALL) -D -m 0644 $(@D)/firmware/toon2-m4.bin \
		$(TARGET_DIR)/usr/share/toon/toon2-m4.bin
	$(INSTALL) -D -m 0644 $(@D)/firmware/toon2-m4-bringup.bin \
		$(TARGET_DIR)/usr/share/toon/toon2-m4-bringup.bin
endef

define TOON_M4_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(TOON_M4_PKGDIR)/S45toon-m4 \
		$(TARGET_DIR)/etc/init.d/S45toon-m4
endef

$(eval $(generic-package))
