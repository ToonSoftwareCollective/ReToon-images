################################################################################
# toon-boiler (Quby boiler module daemon)
################################################################################
TOON_BOILER_VERSION = 1.0
TOON_BOILER_SITE = $(TOON_BOILER_PKGDIR)/src
TOON_BOILER_SITE_METHOD = local
TOON_BOILER_LICENSE = GPL-2.0+
TOON_BOILER_DEPENDENCIES = cjson toon-bus openssl

define TOON_BOILER_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -o $(@D)/toon-boilerd $(@D)/toon-boilerd.c \
		$(@D)/thermostat.c $(@D)/esphome.c $(TARGET_LDFLAGS) -lcjson -lm -ltoonbus \
		-lmosquitto -lpthread -lcrypto
endef

define TOON_BOILER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/toon-boilerd $(TARGET_DIR)/usr/bin/toon-boilerd
endef

define TOON_BOILER_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(TOON_BOILER_PKGDIR)/S46toon-boilerd $(TARGET_DIR)/etc/init.d/S46toon-boilerd
endef

$(eval $(generic-package))
