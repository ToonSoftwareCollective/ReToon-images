################################################################################
# toon-ui (the screen: dashboards, Home Assistant link, WiFi setup, web
# interface)
################################################################################
TOON_UI_VERSION = 1.0
TOON_UI_SITE = $(TOON_UI_PKGDIR)/src
TOON_UI_SITE_METHOD = local
TOON_UI_DEPENDENCIES = lvgl cjson toon-bus mosquitto openssl lighttpd quickjs libcurl jpeg
TOON_UI_LICENSE = GPL-2.0+, OFL-1.1 (Inter), Apache-2.0 (Material Design Icons)

TOON_UI_CFLAGS = $(TARGET_CFLAGS) -O2 -I$(STAGING_DIR)/usr/include/lvgl
# same NEON setting as lvgl (its lv_conf.h checks __ARM_NEON)
ifeq ($(BR2_ARM_CPU_HAS_NEON),y)
TOON_UI_CFLAGS += -mfpu=neon
endif
# ARMv5 (Toon 1): the atomics QuickJS uses come from libatomic
ifeq ($(BR2_ARM_CPU_ARMV5),y)
TOON_UI_EXTRA_LIBS = -latomic
endif

define TOON_UI_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) CC="$(TARGET_CC)" \
		CFLAGS="$(TOON_UI_CFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS)" \
		QJS_LIB="$(STAGING_DIR)/usr/lib/quickjs/libquickjs.a" \
		EXTRA_LIBS="$(TOON_UI_EXTRA_LIBS)" $(TOON_UI_MAKE_OPTS)
endef

define TOON_UI_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/toon-ui $(TARGET_DIR)/usr/bin/toon-ui
	$(INSTALL) -D -m 0755 $(@D)/toon-ha $(TARGET_DIR)/usr/bin/toon-ha
	$(INSTALL) -D -m 0755 $(@D)/toon-splash $(TARGET_DIR)/usr/bin/toon-splash
	mkdir -p $(TARGET_DIR)/usr/share/toon/fonts
	cp $(TOON_UI_PKGDIR)/fonts/* $(TARGET_DIR)/usr/share/toon/fonts/
	$(INSTALL) -D -m 0644 $(TOON_UI_PKGDIR)/default-dashboard.json \
		$(TARGET_DIR)/usr/share/toon/default-dashboard.json
	# the app interface level, for toon-apps (APPS_API in src/apps.c)
	sed -n 's/^#define APPS_API[[:space:]]*//p' $(@D)/apps.c \
		> $(TARGET_DIR)/usr/share/toon/apps-api
	for f in $(TOON_UI_PKGDIR)/bin/*; do \
		$(INSTALL) -D -m 0755 $$f $(TARGET_DIR)/usr/bin/$$(basename $$f); done
	rm -rf $(TARGET_DIR)/usr/share/toon/web
	cp -r $(TOON_UI_PKGDIR)/web $(TARGET_DIR)/usr/share/toon/web
	ln -sf /usr/share/toon-bus/toonbus.js $(TARGET_DIR)/usr/share/toon/web/toonbus.js
	# ?v= on the page's style and scripts: from their contents, so a browser
	# fetches them again after every change (a number set by hand was
	# forgotten once, and browsers kept an old app.js)
	v=$$(cat $(TOON_UI_PKGDIR)/web/app.js $(TOON_UI_PKGDIR)/web/i18n.js \
		$(TOON_UI_PKGDIR)/web/style.css $(TOON_UI_PKGDIR)/../toon-bus/web/toonbus.js | \
		sha256sum | cut -c1-10) && \
	sed -i "s/\(\.\(css\|js\)\)?v=[0-9a-z]*/\1?v=$$v/g" $(TARGET_DIR)/usr/share/toon/web/index.html && \
	grep -q "app.js?v=$$v" $(TARGET_DIR)/usr/share/toon/web/index.html
	$(INSTALL) -D -m 0755 $(@D)/toon-api $(TARGET_DIR)/usr/share/toon/web/cgi-bin/api
	$(INSTALL) -D -m 0755 $(TOON_UI_PKGDIR)/api.sh $(TARGET_DIR)/usr/share/toon/api.sh
	rm -rf $(TARGET_DIR)/usr/share/toon/web-redirect
	cp -r $(TOON_UI_PKGDIR)/web-redirect $(TARGET_DIR)/usr/share/toon/web-redirect
endef

define TOON_UI_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(TOON_UI_PKGDIR)/S00toon-splash $(TARGET_DIR)/etc/init.d/S00toon-splash
	$(INSTALL) -D -m 0755 $(TOON_UI_PKGDIR)/S50toon-ui $(TARGET_DIR)/etc/init.d/S50toon-ui
	$(INSTALL) -D -m 0755 $(TOON_UI_PKGDIR)/S51toon-web $(TARGET_DIR)/etc/init.d/S51toon-web
	$(INSTALL) -D -m 0755 $(TOON_UI_PKGDIR)/S90toon-autoupdate $(TARGET_DIR)/etc/init.d/S90toon-autoupdate
	# S51toon-web runs lighttpd with its own configuration, and only these
	# of its modules
	rm -f $(TARGET_DIR)/etc/init.d/S50lighttpd $(TARGET_DIR)/usr/sbin/lighttpd-angel
	rm -rf $(TARGET_DIR)/etc/lighttpd
	find $(TARGET_DIR)/usr/lib/lighttpd -name 'mod_*.so' \
		! -name mod_auth.so ! -name mod_authn_file.so ! -name mod_cgi.so \
		! -name mod_openssl.so ! -name mod_wstunnel.so -delete
	# a module that an earlier build of this package deleted is not put back
	# when lighttpd itself is not built again: take it from lighttpd's build
	for m in auth authn_file cgi openssl wstunnel; do \
		[ -e $(TARGET_DIR)/usr/lib/lighttpd/mod_$$m.so ] || \
		$(INSTALL) -D -m 0755 $(LIGHTTPD_DIR)/buildroot-build/src/mod_$$m.so \
			$(TARGET_DIR)/usr/lib/lighttpd/mod_$$m.so || exit 1; \
	done
endef

# the static toon-splash for the Toon 1 boot manager (board/quby/toon1/mkinitrd.sh);
# only the Toon 1 (ARM926) has one
ifeq ($(BR2_arm926t),y)
TOON_UI_MAKE_OPTS += STATIC=toon-splash-static
TOON_UI_INSTALL_IMAGES = YES
define TOON_UI_INSTALL_IMAGES_CMDS
	$(INSTALL) -D -m 0755 $(@D)/toon-splash-static $(BINARIES_DIR)/toon-splash-static
endef
endif

$(eval $(generic-package))
