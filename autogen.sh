#!/bin/sh

test -n "$srcdir" || srcdir=`dirname "$0"`
test -n "$srcdir" || srcdir=.

olddir=`pwd`
cd $srcdir

AUTORECONF=`which autoreconf`
if test -z $AUTORECONF; then
        echo "*** No autoreconf found, please intall it ***"
        exit 1
fi

mkdir -p m4

autoreconf --force --install --verbose

cd $olddir
test -n "$NOCONFIGURE" || "$srcdir/configure" "$@"
