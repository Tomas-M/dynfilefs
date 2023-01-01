################################################################################
#
# dynfilefs
#
################################################################################

DYNFILEFS_VERSION = master
DYNFILEFS_SOURCE = dynfilefs-v3.2.tar.gz
DYNFILEFS_SITE = https://github.com/Tomas-M/dynfilefs/archive
DYNFILEFS_LICENSE = GPLv2+, LGPLv2+
DYNFILEFS_DEPENDENCIES = host-pkgconf libfuse
DYNFILEFS_MAKE_OPTS = LDFLAGS=-static

$(eval $(autotools-package))
