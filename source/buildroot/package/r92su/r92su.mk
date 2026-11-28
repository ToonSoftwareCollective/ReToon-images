################################################################################
#
# r92su (Realtek RTL8191SU WiFi driver, out-of-tree kernel module)
#
################################################################################

# branch "retoon" of our fork: the 6.12 backport and 802.11k/v
R92SU_VERSION = 56c09b78213f0011cdbe12a4b381e321c6d9b153
R92SU_SITE = $(call github,IgorYbema,rtl8192su,$(R92SU_VERSION))
R92SU_LICENSE = GPL-2.0
R92SU_LICENSE_FILES = LICENSE
# warnings as errors: a kernel update that breaks the driver fails the build
R92SU_MODULE_MAKE_OPTS = R92SU_WERROR=1

$(eval $(kernel-module))
$(eval $(generic-package))
