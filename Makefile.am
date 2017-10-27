# -*- Makefile -*-

AM_CPPFLAGS            = ${regular_CPPFLAGS}
AM_CFLAGS              = $(regular_CFLAGS) $(libfuse_CFLAGS)

sbin_PROGRAMS          = dynfilefs
dynfilefs_SOURCES      = dynfilefs.c
dynfilefs_LDADD        = -lfuse -lpthread -lrt -ldl
