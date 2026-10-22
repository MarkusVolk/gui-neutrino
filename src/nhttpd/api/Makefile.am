AM_CPPFLAGS = \
	-I$(srcdir) \
	-I$(top_builddir) \
	-I$(top_srcdir) \
	-I$(top_srcdir)/lib \
	-I$(top_srcdir)/src/include \
	-I$(top_srcdir)/src \
	-I$(top_srcdir)/src/nhttpd \
	-I$(top_srcdir)/src/nhttpd/yhttpd_core \
	-I$(top_srcdir)/src/nhttpd/yhttpd_mods \
	-I$(top_srcdir)/src/nhttpd/tuxboxapi \
	-I$(top_srcdir)/lib/xmltree \
	-I$(top_srcdir)/lib/jsoncpp \
	-I$(top_srcdir)/lib/connection \
	-I$(top_srcdir)/lib/libeventserver \
	-I$(top_srcdir)/lib/libconfigfile \
	@SIGC_CFLAGS@ \
	@FREETYPE_CFLAGS@ \
	@AVFORMAT_CFLAGS@ \
	@LUA_CFLAGS@ \
	@HWLIB_CFLAGS@ \
	@MHD_CFLAGS@

AM_CPPFLAGS += -fno-rtti -D_FILE_OFFSET_BITS=64

noinst_LIBRARIES = libyapi.a

libyapi_a_SOURCES = \
	apiserver.cpp

# apiserver.cpp includes the generated src/tools/rcsim.h
apiserver.$(OBJEXT): $(top_builddir)/src/tools/rcsim.h
$(top_builddir)/src/tools/rcsim.h:
	$(MAKE) -C $(top_builddir)/src/tools rcsim.h
