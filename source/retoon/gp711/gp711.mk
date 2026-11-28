################################################################################
# gp711 (GreenPeak GP711 radio: test tool + Spinel RCP daemon)
################################################################################
GP711_VERSION = 0.1
GP711_SITE = $(GP711_PKGDIR)/src
GP711_SITE_METHOD = local
GP711_LICENSE = GPL-2.0+

define GP711_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -o $(@D)/gp711 $(@D)/gp711.c $(TARGET_LDFLAGS)
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -o $(@D)/gp711-rcpd $(@D)/gp711_rcpd.c $(TARGET_LDFLAGS)
endef

define GP711_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/gp711 $(TARGET_DIR)/usr/bin/gp711
	$(INSTALL) -D -m 0755 $(@D)/gp711-rcpd $(TARGET_DIR)/usr/bin/gp711-rcpd
endef

define GP711_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(GP711_PKGDIR)/S60gp711-rcpd $(TARGET_DIR)/etc/init.d/S60gp711-rcpd
endef

$(eval $(generic-package))
