################################################################################
# toon-esphome (ESPHome native API with a Bluetooth proxy for Home Assistant)
################################################################################
TOON_ESPHOME_VERSION = 1.0
TOON_ESPHOME_SITE = $(TOON_ESPHOME_PKGDIR)/src
TOON_ESPHOME_SITE_METHOD = local
TOON_ESPHOME_DEPENDENCIES = avahi bluez5_utils toon-bus
TOON_ESPHOME_LICENSE = GPL-2.0+

define TOON_ESPHOME_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -o $(@D)/toon-esphomed $(@D)/toon-esphomed.c \
		$(TARGET_LDFLAGS) -lavahi-client -lavahi-common -ltoonbus -lmosquitto -lpthread
endef

define TOON_ESPHOME_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/toon-esphomed $(TARGET_DIR)/usr/bin/toon-esphomed
endef

define TOON_ESPHOME_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(TOON_ESPHOME_PKGDIR)/S71toon-esphomed $(TARGET_DIR)/etc/init.d/S71toon-esphomed
endef

$(eval $(generic-package))
