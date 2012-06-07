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

build-glib/gio/libgio-2.0.la:
	mkdir -p build-glib
	(srcdir=$$(cd $(srcdir) && pwd); \
	 cd build-glib; \
         $${srcdir}/glib/autogen.sh --prefix=/EMBEDGLIB --disable-introspection --disable-man --disable-gtk-doc && \
	 $(MAKE))

install-glib/EMBEDGLIB/include/glib-2.0/glib.h: build-glib/gio/libgio-2.0.la
	(top_srcdir=$$(cd $(top_srcdir) && pwd); \
	 builddir=$$(pwd); \
	 cd build-glib && \
         rm -rf install-glib && \
	 $(MAKE) install DESTDIR=$${builddir}/install-glib)

all-local: install-glib/EMBEDGLIB/include/glib-2.0/glib.h

install-exec-local: 
	echo `pwd`
	$(MKDIR_P) $(DESTDIR)/$(pkglibdir)
	install install-glib/EMBEDGLIB/lib/*.so.* $(DESTDIR)/$(pkglibdir)

