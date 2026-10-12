# -*- Makefile -*-

AM_CPPFLAGS            = ${regular_CPPFLAGS}
AM_CFLAGS              = $(regular_CFLAGS) $(libfuse_CFLAGS)

sbin_PROGRAMS          = dynfilefs
dynfilefs_SOURCES      = dynfilefs.c
dynfilefs_LDADD        = $(libfuse_LIBS) -lpthread -lrt -ldl

install-exec-hook:
	ln -sf dynfilefs $(DESTDIR)$(sbindir)/mount.dynfilefs
