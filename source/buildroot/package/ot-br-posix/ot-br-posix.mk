################################################################################
# ot-br-posix (OpenThread Border Router)
################################################################################
OT_BR_POSIX_VERSION = 57a6ea787a559a9abe8b920e7dfdf27086ce7abb
OT_BR_POSIX_SITE = https://github.com/openthread/ot-br-posix.git
OT_BR_POSIX_SITE_METHOD = git
OT_BR_POSIX_GIT_SUBMODULES = YES
OT_BR_POSIX_LICENSE = BSD-3-Clause
OT_BR_POSIX_LICENSE_FILES = LICENSE

# No D-Bus, web page, firewall, NAT64 or TREL: the REST API is what Home
# Assistant uses, and mDNS is OpenThread's own (no avahi client needed).
# MAC security is done by the host because the GP711 cannot.
# OpenThread's internal libs (openthread-ftd, tcplp-ftd) have a cycle that
# CMake allows only among static libraries, so build them static; we install
# the executables, not the libs.
OT_BR_POSIX_CONF_OPTS = \
	-DBUILD_SHARED_LIBS=OFF \
	-DBUILD_TESTING=OFF \
	-DOT_COMPILE_WARNING_AS_ERROR=OFF \
	-DOT_FIREWALL=OFF \
	-DOTBR_MDNS=openthread \
	-DOTBR_REST=ON \
	-DOTBR_DBUS=OFF \
	-DOTBR_WEB=OFF \
	-DOTBR_BACKBONE_ROUTER=OFF \
	-DOTBR_TREL=OFF \
	-DOTBR_NAT64=OFF \
	-DOTBR_DNS_UPSTREAM_QUERY=ON \
	-DOTBR_SRP_SERVER_AUTO_ENABLE=ON \
	-DOTBR_NOTIFY_UPSTART=OFF \
	-DOTBR_POWER_CALIBRATION=OFF \
	-DOTBR_FEATURE_FLAGS=OFF \
	-DOTBR_TELEMETRY_DATA_API=OFF

OT_BR_POSIX_BUILD_OPTS = --target otbr-agent ot-ctl

define OT_BR_POSIX_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(OT_BR_POSIX_BUILDDIR)/src/agent/otbr-agent $(TARGET_DIR)/usr/sbin/otbr-agent
	$(INSTALL) -D -m 0755 $(OT_BR_POSIX_BUILDDIR)/third_party/openthread/repo/src/posix/ot-ctl $(TARGET_DIR)/usr/sbin/ot-ctl
	$(INSTALL) -D -m 0755 $(OT_BR_POSIX_PKGDIR)/toon-thread $(TARGET_DIR)/usr/sbin/toon-thread
endef

define OT_BR_POSIX_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(OT_BR_POSIX_PKGDIR)/S62toon-thread $(TARGET_DIR)/etc/init.d/S62toon-thread
endef

$(eval $(cmake-package))
