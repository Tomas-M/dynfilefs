################################################################################
#
# dynfilefs
#
################################################################################

UNIONFS_VERSION = 3.3
UNIONFS_SOURCE = dynfilefs-v3.3.tar.gz
UNIONFS_SITE = https://github.com/Tomas-M/dynfilefs/archive/refs/tags
UNIONFS_LICENSE = BSD-3-Clause
UNIONFS_DEPENDENCIES = libfuse host-pkgconf
UNIONFS_MAKE_OPTS = LDFLAGS=-static

$(eval $(autotools-package))
