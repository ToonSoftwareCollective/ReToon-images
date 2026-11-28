################################################################################
# toon-sound (sounds on both Toons: codec on the Toon 1, M4 buzzer DAC on the Toon 2)
################################################################################
TOON_SOUND_VERSION = 1.0
TOON_SOUND_SITE = $(TOON_SOUND_PKGDIR)/src
TOON_SOUND_SITE_METHOD = local
TOON_SOUND_LICENSE = GPL-2.0+

define TOON_SOUND_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -Wextra -o $(@D)/toon-sound $(@D)/toon-sound.c $(TARGET_LDFLAGS) -lm
endef

define TOON_SOUND_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/toon-sound $(TARGET_DIR)/usr/bin/toon-sound
endef

$(eval $(generic-package))
