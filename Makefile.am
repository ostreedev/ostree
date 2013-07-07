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
	-DOSTREE_FEATURES='"$(OSTREE_FEATURES)"' \
	-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_34 -DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_34 \
	-DSOUP_VERSION_MIN_REQUIRED=SOUP_VERSION_2_40 -DSOUP_VERSION_MAX_ALLOWED=SOUP_VERSION_40
AM_CFLAGS = $(WARN_CFLAGS)

if BUILD_EMBEDDED_DEPENDENCIES
SUBDIRS = embedded-dependencies .
else
SUBDIRS = .
endif

NULL = 
INSTALL_DATA_HOOKS =
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

EXTRA_DIST += autogen.sh COPYING.GPL COPYING.LGPL README.md

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

libgsystem_srcpath := src/libgsystem
libgsystem_cflags = $(OT_INTERNAL_GIO_UNIX_CFLAGS) -I$(srcdir)/src/libgsystem
libgsystem_libs = $(OT_INTERNAL_GIO_UNIX_LIBS)
include src/libgsystem/Makefile-libgsystem.am
noinst_LTLIBRARIES += libgsystem.la
include Makefile-daemon.am
include Makefile-otutil.am
include Makefile-libostree.am
include Makefile-ostree.am
include Makefile-switchroot.am
include Makefile-tests.am
include Makefile-dracut.am

install-data-hook: $(INSTALL_DATA_HOOKS)

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

embed_dependency=tar -C $(srcdir) --append --exclude='.git/*' --transform="s,^embedded-dependencies/,ostree-embeddeps-$${GITVERSION}/embedded-dependencies/," --file=$${TARFILE_TMP}

git_version_rpm = $$(git describe | sed -e 's,-,\.,g' -e 's,^v,,')

release-tarball-embedded:
	set -x; \
	GITVERSION=$(git_version_rpm); export GITVERSION; \
	TARFILE_TMP=ostree-embeddeps-$${GITVERSION}.tar.tmp; \
	REV=$$(git rev-parse HEAD); \
	echo "Archiving ostree at $${REV}"; \
	(cd $(srcdir); git archive --format=tar --prefix=ostree-embeddeps-$${GITVERSION}/ $${REV}) > $${TARFILE_TMP}; \
	(cd $$(git rev-parse --show-toplevel); git submodule status) | while read line; do \
	  rev=$$(echo $$line | cut -f 1 -d ' '); path=$$(echo $$line | cut -f 2 -d ' '); \
	  echo "Archiving $${path} at $${rev}"; \
	  (cd $(srcdir)/$$path; git archive --format=tar --prefix=ostree-embeddeps-$${GITVERSION}/$$path/ $${rev}) > submodule.tar; \
	  tar -A -f $${TARFILE_TMP} submodule.tar; \
	  rm submodule.tar; \
	done; \
	echo "Archiving glib"; \
	$(embed_dependency) embedded-dependencies/glib; \
	echo "Archiving libsoup"; \
	$(embed_dependency) embedded-dependencies/libsoup; \
	mv ostree-embeddeps-$${GITVERSION}.tar{.tmp,}; \
	gzip -f ostree-embeddeps-$${GITVERSION}.tar 

srpm:
	GITVERSION=$(git_version_rpm); export GITVERSION; \
	$(srcdir)/packages/substitute-gitversion.sh $(srcdir)/packages/ostree.spec.in > ostree.spec
	$(srcdir)/packages/rpmbuild-cwd -bs ostree.spec

dist-snapshot:
	set -x; \
	GITVERSION=$(git_version_rpm); export GITVERSION; \
	TARFILE_TMP=ostree-$${GITVERSION}.tar.tmp; \
	REV=$$(git rev-parse HEAD); \
	echo "Archiving ostree at $${REV}"; \
	(cd $(srcdir); git archive --format=tar --prefix=ostree-$${GITVERSION}/ $${REV}) > $${TARFILE_TMP}; \
	(cd $$(git rev-parse --show-toplevel); git submodule status) | while read line; do \
	  rev=$$(echo $$line | cut -f 1 -d ' '); path=$$(echo $$line | cut -f 2 -d ' '); \
	  echo "Archiving $${path} at $${rev}"; \
	  (cd $(srcdir)/$$path; git archive --format=tar --prefix=ostree-$${GITVERSION}/$$path/ $${rev}) > submodule.tar; \
	  tar -A -f $${TARFILE_TMP} submodule.tar; \
	  rm submodule.tar; \
	done; \
	mv ostree-$${GITVERSION}.tar{.tmp,}; \
	gzip -f ostree-$${GITVERSION}.tar 
