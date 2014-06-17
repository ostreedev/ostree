# Copyright (C) 2013 Colin Walters <walters@verbum.org>
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

abs_top_builddir = @abs_top_builddir@

include ../Makefile-libostree-defines.am

NULL =

# We require automake 1.6 at least.
AUTOMAKE_OPTIONS = 1.6

# The name of the module, e.g. 'glib'.
DOC_MODULE=ostree

# The top-level SGML file. You can change this if you want to.
DOC_MAIN_SGML_FILE=$(DOC_MODULE)-docs.xml

# The directory containing the source code. Relative to $(srcdir).
# gtk-doc will search all .c & .h files beneath here for inline comments
# documenting the functions and macros.
# e.g. DOC_SOURCE_DIR=../../../gtk
DOC_SOURCE_DIR=$(top_srcdir)/src/libostree

# Extra options to pass to gtkdoc-scangobj. Not normally needed.
SCANGOBJ_OPTIONS= --version

# Extra options to supply to gtkdoc-scan.
# e.g. SCAN_OPTIONS=--deprecated-guards="GTK_DISABLE_DEPRECATED"
SCAN_OPTIONS= --rebuild-types

# Extra options to supply to gtkdoc-mkdb.
# e.g. MKDB_OPTIONS=--sgml-mode --output-format=xml
MKDB_OPTIONS=--sgml-mode --output-format=xml

# Extra options to supply to gtkdoc-mktmpl
# e.g. MKTMPL_OPTIONS=--only-section-tmpl
MKTMPL_OPTIONS=

# MKHTML_OPTIONS=--path="$(builddir)/html $(srcdir)/html"

# Extra options to supply to gtkdoc-fixref. Not normally needed.
# e.g. FIXXREF_OPTIONS=--extra-dir=../gdk-pixbuf/html --extra-dir=../gdk/html
FIXXREF_OPTIONS=

# Used for dependencies. The docs will be rebuilt if any of these change.
# e.g. HFILE_GLOB=$(top_srcdir)/gtk/*.h
# e.g. CFILE_GLOB=$(top_srcdir)/gtk/*.c
HFILE_GLOB=$(addprefix $(top_srcdir)/,$(libostree_public_headers))
CFILE_GLOB=$(top_srcdir)/src/libostree/*.c

# Header files to ignore when scanning.
# e.g. IGNORE_HFILES=gtkdebug.h gtkintl.h
IGNORE_HFILES= $(NULL)

# Images to copy into HTML directory.
# e.g. HTML_IMAGES=$(top_srcdir)/gtk/stock-icons/stock_about_24.png
HTML_IMAGES=

# Extra SGML files that are included by $(DOC_MAIN_SGML_FILE).
# e.g. content_files=running.sgml building.sgml changes-2.0.sgml
content_files= \
	overview.xml \
	repo.xml \
	deployment.xml \
	atomic-upgrades.xml \
	adapting-existing.xml \
	$(NULL)

# SGML files where gtk-doc abbrevations (#GtkWidget) are expanded
# These files must be listed here *and* in content_files
# e.g. expand_content_files=running.sgml
expand_content_files= \
	version.xml \
	$(NULL)

# CFLAGS and LDFLAGS for compiling gtkdoc-scangobj with your library.
# Only needed if you are using gtkdoc-scangobj to dynamically query widget
# signals and properties.
# e.g. INCLUDES=-I$(top_srcdir) -I$(top_builddir) $(GTK_DEBUG_FLAGS)
# e.g. GTKDOC_LIBS=$(top_builddir)/gtk/$(gtktargetlib)
GTKDOC_LIBS=

# Hacks around gtk-doc brokenness for out of tree builds
ostree-sections.txt: $(srcdir)/ostree-sections.txt
	cp $< $@

version.xml:
	echo -n $(VERSION) > "$@"

# This includes the standard gtk-doc make rules, copied by gtkdocize.
include $(top_srcdir)/gtk-doc.make

man1_MANS =

if ENABLE_GTK_DOC
man1_MANS += ostree.1 ostree.repo.5 ostree.repo-config.5 ostree-admin-cleanup.1 ostree-admin-config-diff.1 ostree-admin-deploy.1 ostree-admin-init-fs.1 ostree-admin-instutil.1 ostree-admin-os-init.1 ostree-admin-status.1 ostree-admin-switch.1 ostree-admin-undeploy.1 ostree-admin-upgrade.1 ostree-admin.1 ostree-cat.1 ostree-checkout.1 ostree-checksum.1 ostree-commit.1 ostree-config.1 ostree-diff.1 ostree-fsck.1 ostree-init.1 ostree-log.1 ostree-ls.1 ostree-prune.1 ostree-pull-local.1 ostree-pull.1 ostree-refs.1 ostree-remote.1 ostree-reset.1 ostree-rev-parse.1 ostree-show.1 ostree-static-delta.1 ostree-trivial-httpd.1


XSLTPROC_FLAGS = \
        --nonet \
        --stringparam man.output.quietly 1 \
        --stringparam funcsynopsis.style ansi \
        --stringparam man.th.extra1.suppress 1 \
        --stringparam man.authors.section.enabled 0 \
        --stringparam man.copyright.section.enabled 0

XSLTPROC_MAN = \
	$(XSLTPROC) $(XSLTPROC_FLAGS) http://docbook.sourceforge.net/release/xsl/current/manpages/docbook.xsl

.xml.1:
	$(AM_V_GEN) $(XSLTPROC_MAN) $<
.xml.5:
	$(AM_V_GEN) $(XSLTPROC_MAN) $<

endif # ENABLE_GTK_DOC

MAN_IN_FILES = \
	$(man1_MANS:.1=.xml) \
	$(NULL)

CLEANFILES += \
	$(man1_MANS) \
	$(NULL)

EXTRA_DIST += \
	$(MAN_IN_FILES) \
	version.xml \
	$(NULL)
