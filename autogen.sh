#!/bin/sh

test -n "$srcdir" || srcdir=`dirname "$0"`
test -n "$srcdir" || srcdir=.

olddir=`pwd`
cd $srcdir

AUTORECONF=`which autoreconf`
if test -z $AUTORECONF; then
        echo "*** No autoreconf found, please install it ***"
        exit 1
fi

set -e

mkdir -p m4

GTKDOCIZE=$(which gtkdocize 2>/dev/null || true)
if test -z "$GTKDOCIZE"; then
        echo "You don't have gtk-doc installed, and thus won't be able to generate the documentation."
        rm -f gtk-doc.make
        cat > gtk-doc.make <<EOF
EXTRA_DIST =
CLEANFILES =
EOF
else
        gtkdocize
fi

cd $olddir
if ! test -f libglnx/README.md || ! test -f bsdiff/README.md; then
    git submodule update --init
fi
# Workaround automake bug with subdir-objects and computed paths
sed -e 's,$(libglnx_srcpath),libglnx,g' < libglnx/Makefile-libglnx.am >libglnx/Makefile-libglnx.am.inc
sed -e 's,$(libbsdiff_srcpath),bsdiff,g' < bsdiff/Makefile-bsdiff.am >bsdiff/Makefile-bsdiff.am.inc

# FIXME - figure out how to get aclocal to find this by default
ln -sf ../libglnx/libglnx.m4 buildutil/libglnx.m4

autoreconf --force --install --verbose

test -n "$NOCONFIGURE" || "$srcdir/configure" "$@"
