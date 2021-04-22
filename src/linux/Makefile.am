## Process this file with automake to produce Makefile.in

AM_CPPFLAGS = \
	-I$(top_builddir)/src -I$(top_srcdir)/src		\
	-I$(top_builddir)/ -I$(top_srcdir)			\
	-DUP_COMPILATION					\
	-DG_LOG_DOMAIN=\"UPower-Linux\"				\
	-DPACKAGE_SYSCONF_DIR=\""$(sysconfdir)"\"		\
	-I$(top_srcdir)/libupower-glib				\
	$(GIO_CFLAGS)						\
	$(GIO_UNIX_CFLAGS)					\
	$(GUDEV_CFLAGS)						\
	$(POLKIT_CFLAGS)					\
	$(GLIB_CFLAGS)						\
	$(IDEVICE_CFLAGS)					\
	$(NULL)

if BACKEND_TYPE_LINUX
noinst_LTLIBRARIES = libupshared.la
endif

if HAVE_IDEVICE
idevice_files = up-device-idevice.c up-device-idevice.h
else
idevice_files =
endif

libupshared_la_SOURCES =					\
	up-device-supply.c					\
	up-device-supply.h					\
	up-device-hid.c						\
	up-device-hid.h						\
	up-device-wup.c						\
	up-device-wup.h						\
	up-device-bluez.c					\
	up-device-bluez.h					\
	up-backend.c						\
	up-backend-linux-private.h				\
	up-native.c						\
	$(idevice_files)					\
	$(BUILT_SOURCES)

EXTRA_DIST = $(libupshared_la_SOURCES) 			\
	     integration-test

libupshared_la_CFLAGS =					\
	$(WARNINGFLAGS_C)

libupshared_la_LIBADD = $(GIO_LIBS)

clean-local :
	rm -f *~

uninstall-hook:
	rm -f $(DESTDIR)$(localstatedir)/lib/upower/history-* && \
	rmdir $(DESTDIR)$(localstatedir)/lib/upower && \
	rmdir $(DESTDIR)$(localstatedir)/lib && \
	rmdir $(DESTDIR)$(localstatedir)

-include $(top_srcdir)/git.mk
