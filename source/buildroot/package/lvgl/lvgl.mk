################################################################################
#
# lvgl
#
################################################################################

LVGL_VERSION = 9.2.2
LVGL_SITE = $(call github,lvgl,lvgl,v$(LVGL_VERSION))
LVGL_LICENSE = MIT
LVGL_LICENSE_FILES = LICENCE.txt
LVGL_INSTALL_STAGING = YES
LVGL_INSTALL_TARGET = NO

LVGL_CFLAGS = $(TARGET_CFLAGS) -O2 -DLV_CONF_INCLUDE_SIMPLE -I$(@D)
# The Toon 2's SoloX has NEON, but the toolchain default (vfpv3) does not
# enable it; the Toon 1's ARM926 has none.
ifeq ($(BR2_ARM_CPU_HAS_NEON),y)
LVGL_CFLAGS += -mfpu=neon
endif

LVGL_COLOR_DEPTH = $(if $(BR2_PACKAGE_LVGL_COLOR_DEPTH_16),16,32)

# Every object depends on lv_conf.h, which make does not know: when the
# options change, compile all of it again (a kept build tree would otherwise
# keep the old options, as happened with LV_USE_QRCODE).
define LVGL_BUILD_CMDS
	sed 's/^#define LV_COLOR_DEPTH 32$$/#define LV_COLOR_DEPTH $(LVGL_COLOR_DEPTH)/' \
		$(LVGL_PKGDIR)/lv_conf.h > $(@D)/lv_conf.h.new
	cmp -s $(@D)/lv_conf.h.new $(@D)/lv_conf.h || \
		find $(@D)/src -name '*.o' -delete
	mv $(@D)/lv_conf.h.new $(@D)/lv_conf.h
	cd $(@D) && find src -name '*.c' -o -name '*.S' | sed 's/\.[cS]$$/.o/' | tr '\n' ' ' > objs.list
	printf 'OBJS := $$(shell cat objs.list)\nliblvgl.a: $$(OBJS)\n\t$$(AR) rcs $$@ $$(OBJS)\n%%.o: %%.S\n\t$$(CC) $$(CFLAGS) -c -o $$@ $$<\n' > $(@D)/Makefile.toon
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) -f Makefile.toon CC="$(TARGET_CC)" AR="$(TARGET_AR)" CFLAGS="$(LVGL_CFLAGS)"
endef

define LVGL_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/liblvgl.a $(STAGING_DIR)/usr/lib/liblvgl.a
	rm -rf $(STAGING_DIR)/usr/include/lvgl
	mkdir -p $(STAGING_DIR)/usr/include/lvgl
	cd $(@D) && find lvgl.h lv_version.h lv_conf.h src -name '*.h' | cpio -pdm $(STAGING_DIR)/usr/include/lvgl
endef

$(eval $(generic-package))
