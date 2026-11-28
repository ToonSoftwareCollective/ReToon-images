################################################################################
# toon-bus (libtoonbus: the ReToon programs on the Toon's own MQTT broker)
################################################################################
TOON_BUS_VERSION = 1.0
TOON_BUS_SITE = $(TOON_BUS_PKGDIR)/src
TOON_BUS_SITE_METHOD = local
TOON_BUS_DEPENDENCIES = mosquitto
TOON_BUS_LICENSE = GPL-2.0+
TOON_BUS_INSTALL_STAGING = YES

define TOON_BUS_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -c -o $(@D)/toonbus.o $(@D)/toonbus.c
	$(TARGET_AR) rcs $(@D)/libtoonbus.a $(@D)/toonbus.o
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -o $(@D)/toon-bus $(@D)/toon-bus.c $(@D)/serve.c \
		$(@D)/libtoonbus.a $(TARGET_LDFLAGS) -lmosquitto -lpthread
endef

define TOON_BUS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/toon-bus $(TARGET_DIR)/usr/bin/toon-bus
	# the same calls for the web interface (linked into its folder by toon-ui)
	$(INSTALL) -D -m 0644 $(TOON_BUS_PKGDIR)/web/toonbus.js \
		$(TARGET_DIR)/usr/share/toon-bus/toonbus.js
endef

define TOON_BUS_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(TOON_BUS_PKGDIR)/S42toon-bus $(TARGET_DIR)/etc/init.d/S42toon-bus
endef

define TOON_BUS_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/libtoonbus.a $(STAGING_DIR)/usr/lib/libtoonbus.a
	$(INSTALL) -D -m 0644 $(@D)/toonbus.h $(STAGING_DIR)/usr/include/toonbus.h
endef

$(eval $(generic-package))
