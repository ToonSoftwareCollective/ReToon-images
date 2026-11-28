################################################################################
# toon-zwave (the Z-Wave controller on a TCP port)
################################################################################
TOON_ZWAVE_VERSION = 1.0
TOON_ZWAVE_SITE = $(TOON_ZWAVE_PKGDIR)/src
TOON_ZWAVE_SITE_METHOD = local
TOON_ZWAVE_LICENSE = GPL-2.0+
TOON_ZWAVE_DEPENDENCIES = toon-bus

define TOON_ZWAVE_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -Wextra -o $(@D)/toon-zwaved $(@D)/toon-zwaved.c $(@D)/meter.c $(TARGET_LDFLAGS) -ltoonbus -lmosquitto -lpthread
endef

define TOON_ZWAVE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/toon-zwaved $(TARGET_DIR)/usr/bin/toon-zwaved
endef

define TOON_ZWAVE_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(TOON_ZWAVE_PKGDIR)/S61toon-zwaved $(TARGET_DIR)/etc/init.d/S61toon-zwaved
endef

$(eval $(generic-package))
