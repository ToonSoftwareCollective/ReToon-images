################################################################################
# ens210 (out-of-tree IIO kernel module)
################################################################################
ENS210_VERSION = 1.0
ENS210_SITE = $(ENS210_PKGDIR)/src
ENS210_SITE_METHOD = local
$(eval $(kernel-module))
$(eval $(generic-package))
