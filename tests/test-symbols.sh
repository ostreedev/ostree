#!/bin/bash
#
# Copyright (C) 2016 Colin Walters <walters@verbum.org>
#
# SPDX-License-Identifier: LGPL-2.0+

set -xeuo pipefail

echo '1..3'

released_syms=${G_TEST_SRCDIR}/src/libostree/libostree-released.sym
if echo "$OSTREE_FEATURES" | grep --quiet --no-messages "devel"; then
    devel_syms=${G_TEST_SRCDIR}/src/libostree/libostree-devel.sym
else
    devel_syms=
fi
if echo "$OSTREE_FEATURES" | grep --quiet --no-messages "experimental"; then
  experimental_sym="${G_TEST_SRCDIR}/src/libostree/libostree-experimental.sym"
  experimental_sections="${G_TEST_SRCDIR}/apidoc/ostree-experimental-sections.txt"
else
  experimental_sym=""
  experimental_sections=""
fi

echo "Verifying all expected symbols are actually exported..."
grep --no-filename ' ostree_[A-Za-z0-9_]*;' ${released_syms} ${devel_syms} ${experimental_sym} | sed -e 's,^ *\([A-Za-z0-9_]*\);,\1,' | sort -u > expected-symbols.txt
eu-readelf -a ${G_TEST_BUILDDIR}/.libs/libostree-1.so | grep 'FUNC.*GLOBAL.*DEFAULT.*@@LIBOSTREE_' | sed -e 's,^.* \(ostree_[A-Za-z0-9_]*\)@@LIBOSTREE_[0-9A-Z_.]*,\1,' |sort -u > found-symbols.txt
diff -u expected-symbols.txt found-symbols.txt
echo "ok exports"

# cmd__private__ is private.  The fetcher symbol should not have been made public.
grep -E -v '(ostree_cmd__private__)|(ostree_fetcher_config_flags_get_type)' found-symbols.txt > expected-documented.txt

echo "Verifying all public symbols are documented:"
grep --no-filename '^ostree_' ${G_TEST_SRCDIR}/apidoc/ostree-sections.txt $experimental_sections |sort -u > found-documented.txt
diff -u expected-documented.txt found-documented.txt

echo 'ok documented symbols'

# ONLY update this checksum in release commits!
cat > released-sha256.txt <<EOF
e880ade1f3b4cc7587dc1a7a30059ab1d5287484ee70843f1bb4f258fbec677c  ${released_syms}
EOF
sha256sum -c released-sha256.txt

echo "ok someone didn't add a symbol to a released version"
