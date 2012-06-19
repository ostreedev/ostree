# Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

ACLOCAL_AMFLAGS = -I m4 ${ACLOCAL_FLAGS}
AM_CPPFLAGS = -DDATADIR='"$(datadir)"' -DLIBEXECDIR='"$(libexecdir)"' \
	-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_28 -DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_28
AM_CFLAGS = $(WARN_CFLAGS)

if BUILD_EMBEDDED_DEPENDENCIES
SUBDIRS = embedded-dependencies .
else
SUBDIRS = .
endif

NULL = 
BUILT_SOURCES =
MANPAGES =
CLEANFILES =
EXTRA_DIST =
bin_PROGRAMS =
sbin_PROGRAMS =
bin_SCRIPTS =
libexec_PROGRAMS =
noinst_LTLIBRARIES =
noinst_PROGRAMS =
privlibdir = $(pkglibdir)
privlib_LTLIBRARIES =

if BUILD_EMBEDDED_DEPENDENCIES
OT_INTERNAL_GIO_UNIX_CFLAGS = \
	-I$(top_builddir)/embedded-dependencies/EMBEDDEPS/include/glib-2.0 \
	-I$(top_builddir)/embedded-dependencies/EMBEDDEPS/include/gio-unix-2.0 \
	-I$(top_builddir)/embedded-dependencies/EMBEDDEPS/lib/glib-2.0/include \
	$(NULL)
OT_INTERNAL_GIO_UNIX_LIBS = \
	$(top_builddir)/embedded-dependencies/EMBEDDEPS/lib/libglib-2.0.so \
	$(top_builddir)/embedded-dependencies/EMBEDDEPS/lib/libgthread-2.0.so \
	$(top_builddir)/embedded-dependencies/EMBEDDEPS/lib/libgmodule-2.0.so \
	$(top_builddir)/embedded-dependencies/EMBEDDEPS/lib/libgobject-2.0.so \
	$(top_builddir)/embedded-dependencies/EMBEDDEPS/lib/libgio-2.0.so \
	$(NULL)
OT_INTERNAL_SOUP_CFLAGS = \
	$(OT_INTERNAL_GIO_UNIX_CFLAGS) \
	-I$(top_builddir)/embedded-dependencies/EMBEDDEPS/include/libsoup-2.4 \
	$(NULL)
OT_INTERNAL_SOUP_LIBS = \
	$(OT_INTERNAL_GIO_UNIX_LIBS) \
	$(top_builddir)/embedded-dependencies/EMBEDDEPS/lib/libsoup-2.4.so \
	$(NULL)
else
OT_INTERNAL_GIO_UNIX_CFLAGS = $(OT_DEP_GIO_UNIX_CFLAGS)
OT_INTERNAL_GIO_UNIX_LIBS = $(OT_DEP_GIO_UNIX_LIBS)
OT_INTERNAL_SOUP_CFLAGS = $(OT_DEP_SOUP_CFLAGS)
OT_INTERNAL_SOUP_LIBS = $(OT_DEP_SOUP_LIBS)
endif

include Makefile-daemon.am
include Makefile-otutil.am
include Makefile-libostree.am
include Makefile-ostree.am
include Makefile-ostadmin.am
include Makefile-switchroot.am
include Makefile-triggers.am

#  Docbook generation copied from systemd/Makefile.am
#
#  Copyright 2010 Lennart Poettering
#
#  systemd is free software; you can redistribute it and/or modify it
#  under the terms of the GNU General Public License as published by
#  the Free Software Foundation; either version 2 of the License, or
#  (at your option) any later version.
#
#  systemd is distributed in the hope that it will be useful, but
#  WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
#  General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with systemd; If not, see <http://www.gnu.org/licenses/>.

XML_FILES = \
	${patsubst %.1,%.xml,${patsubst %.3,%.xml,${patsubst %.5,%.xml,${patsubst %.7,%.xml,${patsubst %.8,%.xml,$(MANPAGES)}}}}}
EXTRA_DIST += $(XML_FILES)

if BUILD_DOCUMENTATION

dist_man_MANS = $(MANPAGES)

XSLTPROC_FLAGS = \
	--nonet \
	--param funcsynopsis.style "'ansi'"

XSLTPROC_PROCESS_MAN = \
	$(AM_V_GEN)$(MKDIR_P) $(dir $@) && \
	$(XSLTPROC) -o $@ $(XSLTPROC_FLAGS) http://docbook.sourceforge.net/release/xsl/current/manpages/docbook.xsl $<

doc/%.1: doc/%.xml
	$(XSLTPROC_PROCESS_MAN)

endif


release-tag:
	git tag -m "Release $(VERSION)" v$(VERSION)

embed_dependency=tar -C $(srcdir) --append --exclude='.git/*' --transform='s,^embedded-dependencies/,ostree-embeddeps-$(VERSION)/embedded-dependencies/,' --file=ostree-embeddeps-$(VERSION).tar.tmp

release-tarball-embedded:
	(cd $(srcdir); git archive --format=tar --prefix=ostree-embeddeps-$(VERSION)/ HEAD) > ostree-embeddeps-$(VERSION).tar.tmp
	$(embed_dependency) embedded-dependencies/glib
	$(embed_dependency) embedded-dependencies/libsoup
	mv ostree-embeddeps-$(VERSION).tar{.tmp,}
	gzip -f ostree-embeddeps-$(VERSION).tar 
