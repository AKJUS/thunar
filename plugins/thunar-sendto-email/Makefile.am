AM_CPPFLAGS =								\
	-I$(top_builddir)						\
	-I$(top_builddir)/plugins					\
	-I$(top_srcdir)							\
	-I$(top_srcdir)/plugins						\
	-DG_LOG_DOMAIN=\"thunar-sendto-email\"				\
	-DPACKAGE_LOCALE_DIR=\"$(localedir)\"				\
	$(PLATFORM_CPPFLAGS)

thunar_sendto_emaildir =						\
	$(HELPER_PATH_PREFIX)/Thunar

thunar_sendto_email_PROGRAMS =						\
	thunar-sendto-email

thunar_sendto_email_SOURCES =						\
	main.c

thunar_sendto_email_CFLAGS =						\
	$(GIO_CFLAGS)							\
	$(GTK_CFLAGS)							\
	$(LIBXFCE4UI_CFLAGS)							\
	$(LIBXFCE4UTIL_CFLAGS)						\
	$(PLATFORM_CFLAGS)

thunar_sendto_email_LDADD =						\
	$(GIO_LIBS)							\
	$(GTK_LIBS)							\
	$(LIBXFCE4UI_LIBS)							\
	$(LIBXFCE4UTIL_LIBS)						\
	$(PLATFORM_LDFLAGS)

desktopdir = $(datadir)/Thunar/sendto
desktop_in_in_files = thunar-sendto-email.desktop.in.in
desktop_in_files = $(desktop_in_in_files:.desktop.in.in=.desktop.in)
%.desktop.in: %.desktop.in.in
	sed -e "s,\@HELPERDIR\@,$(HELPER_PATH_PREFIX),g" < $< > $@
desktop_DATA = $(desktop_in_files:.desktop.in=.desktop)
%.desktop: %.desktop.in Makefile
	$(AM_V_GEN)$(MSGFMT) --desktop --template $< -d $(top_srcdir)/po -o $@

EXTRA_DIST =								\
	$(desktop_in_in_files)						\
	README.md \
	meson.build \
	$(NULL)

DISTCLEANFILES =							\
	$(desktop_in_files)						\
	$(desktop_DATA)

# vi:set ts=8 sw=8 noet ai nocindent syntax=automake:
