################################################################################
#
# dynfilefs
#
################################################################################

UNIONFS_VERSION = master
UNIONFS_SOURCE = master.tar.gz
UNIONFS_SITE = https://github.com/Tomas-M/dynfilefs/archive/refs/heads
UNIONFS_LICENSE = GPLv2
UNIONFS_DEPENDENCIES = libfuse host-pkgconf
UNIONFS_MAKE_OPTS = LDFLAGS=-static

# Override the configure step to run autogen.sh first
define UNIONFS_RUN_AUTOGEN
    cd $(@D) && ./autogen.sh
endef
UNIONFS_PRE_CONFIGURE_HOOKS += UNIONFS_RUN_AUTOGEN

$(eval $(autotools-package))
