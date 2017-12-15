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

OSTREE_GITREV=$(shell cd $(srcdir) && if command -v git >/dev/null 2>&1 && test -d .git; then git describe --abbrev=42 --tags --always HEAD; fi)

ACLOCAL_AMFLAGS = -I buildutil -I libglnx ${ACLOCAL_FLAGS}
AM_CPPFLAGS += -DDATADIR='"$(datadir)"' -DLIBEXECDIR='"$(libexecdir)"' \
	-DLOCALEDIR=\"$(datadir)/locale\" -DSYSCONFDIR=\"$(sysconfdir)\" \
	-DTARGET_PREFIX='"$(prefix)"' -DSHORTENED_SYSCONFDIR=\"$(shortened_sysconfdir)\" \
	-DOSTREE_FEATURES='"$(OSTREE_FEATURES)"' \
	-DOSTREE_COMPILATION \
	-DG_LOG_DOMAIN=\"OSTree\" \
  -DOSTREE_GITREV='"$(OSTREE_GITREV)"' \
	-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_40 '-DGLIB_VERSION_MAX_ALLOWED=G_ENCODE_VERSION(2,50)' \
	-DSOUP_VERSION_MIN_REQUIRED=SOUP_VERSION_2_40 '-DSOUP_VERSION_MAX_ALLOWED=G_ENCODE_VERSION(2,48)'
# For strict aliasing, see https://bugzilla.gnome.org/show_bug.cgi?id=791622
AM_CFLAGS += -std=gnu99 -fno-strict-aliasing $(WARN_CFLAGS)
AM_DISTCHECK_CONFIGURE_FLAGS += \
	--enable-gtk-doc \
	--enable-man \
	--disable-maintainer-mode \
	$(NULL)

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

# These bits based on gnome:librsvg/Makefile.am
if ENABLE_RUST
if RUST_DEBUG
CARGO_RELEASE_ARGS=
else
CARGO_RELEASE_ARGS=--release
endif

check-local:
	cd $(srcdir)/rust && CARGO_TARGET_DIR=$(abs_top_builddir)/target cargo test

clean-local:
	cd $(srcdir)/rust && CARGO_TARGET_DIR=$(abs_top_builddir)/target cargo clean

dist-hook:
	(cd $(distdir)/rust && \
   cp $(abs_top_srcdir)/rust/Cargo.lock . && \
   cargo vendor -q && \
   mkdir .cargo && \
	 cp cargo-vendor-config .cargo/config)

EXTRA_DIST += $(srcdir)/rust/Cargo.toml $(srcdir)/rust/cargo-vendor-config
endif # end ENABLE_RUST

libglnx_srcpath := $(srcdir)/libglnx
libglnx_cflags := $(OT_DEP_GIO_UNIX_CFLAGS) "-I$(libglnx_srcpath)"
libglnx_libs := $(OT_DEP_GIO_UNIX_LIBS)
# See also autogen.sh and https://github.com/ostreedev/ostree/pull/1274/
#
# v2017.12 didn't include test-libglnx-shutil.c, but if you re-run
# autogen.sh (as we do in Debian, to update the Autotools build system)
# it will try to build it.
$(srcdir)/libglnx/Makefile-libglnx.am.inc: $(srcdir)/libglnx/Makefile-libglnx.am
	sed -e 's,$$(libglnx_srcpath),libglnx,g' < $< > $@
include libglnx/Makefile-libglnx.am.inc
EXTRA_DIST += libglnx/Makefile-libglnx.am
noinst_LTLIBRARIES += libglnx.la

libbsdiff_srcpath := $(srcdir)/bsdiff

libbsdiff_cflags := $(OT_DEP_GIO_UNIX_CFLAGS) "-I$(bsdiff_srcpath)"
libbsdiff_libs := $(OT_DEP_GIO_UNIX_LIBS)
# See the comment for the similar libglnx bit above
$(srcdir)/bsdiff/Makefile-bsdiff.am.inc: $(srcdir)/bsdiff/Makefile-bsdiff.am
	sed -e 's,$$(libbsdiff_srcpath),bsdiff,g' < $< > $@
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
include Makefile-bash.am

release-tag:
	cd $(srcdir) && git $(srcdir) tag -m "Release $(VERSION)" v$(VERSION)

embed_dependency=tar -C $(srcdir) --append --exclude='.git/*' --transform="s,^embedded-dependencies/,ostree-embeddeps-$${GITVERSION}/embedded-dependencies/," --file=$${TARFILE_TMP}

git_version_rpm = $$(cd $(srcdir) && git describe | sed -e 's,-,\.,g' -e 's,^v,,')

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
