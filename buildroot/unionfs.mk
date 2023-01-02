################################################################################
#
# dynfilefs
#
################################################################################

UNIONFS_VERSION = master
UNIONFS_SOURCE = master.tar.gz
UNIONFS_SITE = https://github.com/Tomas-M/dynfilefs/archive/refs/heads/
UNIONFS_LICENSE = GPLv2
UNIONFS_DEPENDENCIES = libfuse host-pkgconf
UNIONFS_MAKE_OPTS = LDFLAGS=-static

$(eval $(autotools-package))
