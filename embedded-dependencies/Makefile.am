# Import GLib into local tree
#
# Copyright (C) 2012 Colin Walters <walters@verbum.org>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

glib_config_opts = --disable-silent-rules --disable-static --disable-gtk-doc \
	--disable-man --disable-modular-tests --disable-libelf

libsoup_config_opts = --disable-silent-rules --disable-static --disable-gtk-doc \
	    --disable-introspection --without-gnome --disable-tls-check \
	    --without-apache-httpd --without-ntlm-auth

build_environment = PATH=$${builddir}/EMBEDDEPS/bin$${PATH:+:$$PATH} \
	LD_LIBRARY_PATH="$${builddir}/EMBEDDEPS/lib$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}" \
	C_INCLUDE_PATH="$${builddir}/EMBEDDEPS/include$${C_INCLUDE_PATH:+:$$C_INCLUDE_PATH}" \
	LDFLAGS="-L$${builddir}/EMBEDDEPS/lib$${LDFLAGS:+:$$LDFLAGS}" \
	ACLOCAL_PATH="$${builddir}/EMBEDDEPS/share/aclocal$${ACLOCAL_PATH:+:$$ACLOCAL_PATH}" \
	ACLOCAL_FLAGS="-I $${builddir}/EMBEDDEPS/share/aclocal $${ACLOCAL_FLAGS}" \
	PKG_CONFIG_PATH="$${builddir}/EMBEDDEPS/lib/pkgconfig$${PKG_CONFIG_PATH:+:$$PKG_CONFIG_PATH}"

# aclocal aborts if the path doesn't exist yet.  Lame.  Let's also
# make /bin just because.
stamp-buildroot: Makefile
	mkdir -p EMBEDDEPS/bin
	mkdir -p EMBEDDEPS/share/aclocal
	touch stamp-buildroot

build-glib/gio/libgio-2.0.la: stamp-buildroot
	(srcdir=$$(cd $(srcdir) && pwd); \
	 builddir=$$(pwd); \
	 (cd $${srcdir}/glib; env NOCONFIGURE=1 $(build_environment) ./autogen.sh) && \
	 mkdir -p build-glib && \
	 cd build-glib && \
         env $(build_environment) $${srcdir}/glib/configure --prefix=$${builddir}/EMBEDDEPS $(glib_config_opts) && \
	 env $(build_environment) $(MAKE))

EMBEDDEPS/include/glib-2.0/glib.h: build-glib/gio/libgio-2.0.la
	(top_srcdir=$$(cd $(top_srcdir) && pwd); \
	 builddir=$$(pwd); \
	 cd build-glib && \
	 $(MAKE) install)

build-libsoup/libsoup/libsoup-2.4.la: EMBEDDEPS/include/glib-2.0/glib.h
	(srcdir=$$(cd $(srcdir) && pwd); \
	 builddir=$$(pwd); \
	 (cd $${srcdir}/libsoup; env NOCONFIGURE=1 $(build_environment) ./autogen.sh) && \
	 mkdir -p build-libsoup && \
	 cd build-libsoup && \
	 env $(build_environment) $${srcdir}/libsoup/configure --prefix=$${builddir}/EMBEDDEPS $(libsoup_config_opts) && \
	 env $(build_environment) $(MAKE))

EMBEDDEPS/include/libsoup-2.4/libsoup/soup.h: build-libsoup/libsoup/libsoup-2.4.la
	(top_srcdir=$$(cd $(top_srcdir) && pwd); \
	 builddir=$$(pwd); \
	 cd build-libsoup && \
	 $(MAKE) install)

all-local: EMBEDDEPS/include/libsoup-2.4/libsoup/soup.h

install-exec-local: 
	echo `pwd`
	$(MKDIR_P) $(DESTDIR)/$(pkglibdir)
	install EMBEDDEPS/lib/*.so.* $(DESTDIR)/$(pkglibdir)

