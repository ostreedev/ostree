#!/bin/sh

# Prepare docs directory for running jekyll. This would be better as a
# local Jekyll plugin, but those aren't allowed by the github-pages gem.

set -e

docsdir=$(dirname "$0")
topdir="$docsdir/.."

# Make sure the API docs have been generated and copy them to the
# reference directory.
apidocs="$topdir/apidoc/html"
refdir="$docsdir/reference"
if [ ! -d "$apidocs" ]; then
    echo "error: API docs $apidocs have not been generated" >&2
    echo "Rebuild with --enable-gtk-doc option" >&2
    exit 1
fi

echo "Copying $apidocs to $refdir"
rm -rf "$refdir"
cp -r "$apidocs" "$refdir"

# Make sure the manpages have been generated and copy them to the man
# directory.
manhtml="$topdir/man/html"
mandir="$docsdir/man"
if [ ! -d "$manhtml" ]; then
    echo "error: HTML manpages $manhtml have not been generated" >&2
    echo "Rebuild with --enable-man option and run `make manhtml`" >&2
    exit 1
fi

echo "Copying $manhtml to $mandir"
rm -rf "$mandir"
cp -r "$manhtml" "$mandir"
