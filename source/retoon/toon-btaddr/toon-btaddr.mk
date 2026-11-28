################################################################################
# toon-btaddr (set the Bluetooth public address at boot)
################################################################################
TOON_BTADDR_VERSION = 1.0
TOON_BTADDR_SITE = $(TOON_BTADDR_PKGDIR)/src
TOON_BTADDR_SITE_METHOD = local
TOON_BTADDR_LICENSE = GPL-2.0+

define TOON_BTADDR_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -o $(@D)/toon-btaddr $(@D)/toon-btaddr.c $(TARGET_LDFLAGS)
endef

define TOON_BTADDR_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/toon-btaddr $(TARGET_DIR)/usr/bin/toon-btaddr
endef

define TOON_BTADDR_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(TOON_BTADDR_PKGDIR)/S35toon-btaddr $(TARGET_DIR)/etc/init.d/S35toon-btaddr
endef

$(eval $(generic-package))
