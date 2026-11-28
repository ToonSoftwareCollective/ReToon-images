################################################################################
# toon-hub (room climate daemon + MQTT link to Home Assistant)
################################################################################
TOON_HUB_VERSION = 1.0
TOON_HUB_SITE = $(TOON_HUB_PKGDIR)/src
TOON_HUB_SITE_METHOD = local
TOON_HUB_DEPENDENCIES = mosquitto toon-bus
TOON_HUB_LICENSE = GPL-2.0+

define TOON_HUB_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -o $(@D)/toon-sensord $(@D)/toon-sensord.c $(TARGET_LDFLAGS) -lm \
		-ltoonbus -lmosquitto -lpthread
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -o $(@D)/toon-mqttd $(@D)/toon-mqttd.c $(TARGET_LDFLAGS) -ltoonbus -lmosquitto -lpthread
endef

define TOON_HUB_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/toon-sensord $(TARGET_DIR)/usr/bin/toon-sensord
	$(INSTALL) -D -m 0755 $(@D)/toon-mqttd $(TARGET_DIR)/usr/bin/toon-mqttd
endef

define TOON_HUB_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(TOON_HUB_PKGDIR)/S41toon-broker $(TARGET_DIR)/etc/init.d/S41toon-broker
	$(INSTALL) -D -m 0755 $(TOON_HUB_PKGDIR)/udhcpc-toon-broker \
		$(TARGET_DIR)/usr/share/udhcpc/default.script.d/toon-broker
	$(INSTALL) -D -m 0755 $(TOON_HUB_PKGDIR)/S44toon-sensord $(TARGET_DIR)/etc/init.d/S44toon-sensord
	$(INSTALL) -D -m 0755 $(TOON_HUB_PKGDIR)/S49toon-hostkeys $(TARGET_DIR)/etc/init.d/S49toon-hostkeys
	$(INSTALL) -D -m 0755 $(TOON_HUB_PKGDIR)/S70toon-mqttd $(TARGET_DIR)/etc/init.d/S70toon-mqttd
	$(INSTALL) -D -m 0755 $(TOON_HUB_PKGDIR)/S90toon-bootscripts $(TARGET_DIR)/etc/init.d/S90toon-bootscripts
endef

# SSH host keys: only the ones S49toon-hostkeys makes and keeps; dropbear
# without -R, so it never offers (and on a Toon 1 slowly makes) an RSA key
define TOON_HUB_DROPBEAR_KEYS
	$(INSTALL) -D -m 0644 $(TOON_HUB_PKGDIR)/dropbear.default $(TARGET_DIR)/etc/default/dropbear
	if [ -f $(TARGET_DIR)/etc/init.d/S50dropbear ]; then \
		$(SED) '/DROPBEAR_ARGS="$$DROPBEAR_ARGS -R"/d' $(TARGET_DIR)/etc/init.d/S50dropbear; fi
endef
TOON_HUB_TARGET_FINALIZE_HOOKS += TOON_HUB_DROPBEAR_KEYS

# the broker is started by S41toon-broker with its own settings: not by the
# package's S50mosquitto, whose default would listen on every address
# without a password. mosquitto_ctrl is for a plugin that is not used.
define TOON_HUB_BROKER_CLEANUP
	rm -f $(TARGET_DIR)/etc/init.d/S50mosquitto $(TARGET_DIR)/usr/bin/mosquitto_ctrl
endef
TOON_HUB_TARGET_FINALIZE_HOOKS += TOON_HUB_BROKER_CLEANUP

$(eval $(generic-package))
