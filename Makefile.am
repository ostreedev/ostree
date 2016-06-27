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

include Makefile-decls.am

shortened_sysconfdir = $$(echo "$(sysconfdir)" | sed -e 's|^$(prefix)||' -e 's|^/||')

ACLOCAL_AMFLAGS = -I buildutil -I libglnx ${ACLOCAL_FLAGS}
AM_CPPFLAGS += -DDATADIR='"$(datadir)"' -DLIBEXECDIR='"$(libexecdir)"' \
	-DLOCALEDIR=\"$(datadir)/locale\" -DSYSCONFDIR=\"$(sysconfdir)\" \
	-DSHORTENED_SYSCONFDIR=\"$(shortened_sysconfdir)\" \
	-DOSTREE_FEATURES='"$(OSTREE_FEATURES)"' \
	-DOSTREE_COMPILATION \
	-DG_LOG_DOMAIN=\"OSTree\" \
	-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_40 -DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_40 \
	-DSOUP_VERSION_MIN_REQUIRED=SOUP_VERSION_2_40 -DSOUP_VERSION_MAX_ALLOWED=SOUP_VERSION_2_48
AM_CFLAGS += -std=gnu99 $(WARN_CFLAGS)
AM_DISTCHECK_CONFIGURE_FLAGS += --enable-gtk-doc --disable-maintainer-mode

GITIGNOREFILES = aclocal.m4 build-aux/ buildutil/*.m4 config.h.in gtk-doc.make

SUBDIRS += .

if ENABLE_GTK_DOC
SUBDIRS += apidoc
endif

EXTRA_DIST += autogen.sh COPYING README.md

OT_INTERNAL_GIO_UNIX_CFLAGS = $(OT_DEP_GIO_UNIX_CFLAGS)
OT_INTERNAL_GIO_UNIX_LIBS = $(OT_DEP_GIO_UNIX_LIBS)
OT_INTERNAL_SOUP_CFLAGS = $(OT_DEP_SOUP_CFLAGS)
OT_INTERNAL_SOUP_LIBS = $(OT_DEP_SOUP_LIBS)

# This canonicalizes the PKG_CHECK_MODULES or AM_PATH_GPGME results
OT_INTERNAL_GPGME_CFLAGS = $(OT_DEP_GPGME_CFLAGS) $(GPGME_PTHREAD_CFLAGS)
OT_INTERNAL_GPGME_LIBS = $(OT_DEP_GPGME_LIBS) $(GPGME_PTHREAD_LIBS)

if BUILDOPT_INTROSPECTION
include $(INTROSPECTION_MAKEFILE)
GIRS =
TYPELIBS = $(GIRS:.gir=.typelib)
endif

libglnx_srcpath := $(srcdir)/libglnx
libglnx_cflags := $(OT_DEP_GIO_UNIX_CFLAGS) "-I$(libglnx_srcpath)"
libglnx_libs := $(OT_DEP_GIO_UNIX_LIBS)
include libglnx/Makefile-libglnx.am.inc
EXTRA_DIST += libglnx/Makefile-libglnx.am
noinst_LTLIBRARIES += libglnx.la

libbsdiff_srcpath := $(srcdir)/bsdiff
libbsdiff_cflags := $(OT_DEP_GIO_UNIX_CFLAGS) "-I$(bsdiff_srcpath)"
libbsdiff_libs := $(OT_DEP_GIO_UNIX_LIBS)
include bsdiff/Makefile-bsdiff.am.inc
EXTRA_DIST += bsdiff/Makefile-bsdiff.am
noinst_LTLIBRARIES += libbsdiff.la

include Makefile-otutil.am
include Makefile-libostree.am
include Makefile-ostree.am
include Makefile-switchroot.am
if BUILDOPT_FUSE
include src/rofiles-fuse/Makefile-inc.am
endif
include Makefile-tests.am
include Makefile-boot.am
include Makefile-man.am

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

-include $(top_srcdir)/git.mk
