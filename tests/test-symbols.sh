#!/bin/bash
#
# Copyright (C) 2016 Colin Walters <walters@verbum.org>
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

set -euo pipefail

echo '1..2'

if echo "$OSTREE_FEATURES" | grep --quiet --no-messages "experimental"; then
  experimental_sym="${G_TEST_SRCDIR}/src/libostree/libostree-experimental.sym"
  experimental_sections="${G_TEST_SRCDIR}/apidoc/ostree-experimental-sections.txt"
else
  experimental_sym=""
  experimental_sections=""
fi

echo "Verifying all expected symbols are actually exported..."
grep --no-filename ' ostree_[A-Za-z0-9_]*;' ${G_TEST_SRCDIR}/src/libostree/libostree.sym $experimental_sym | sed -e 's,^ *\([A-Za-z0-9_]*\);,\1,' | sort -u > expected-symbols.txt
eu-readelf -a ${G_TEST_BUILDDIR}/.libs/libostree-1.so | grep 'FUNC.*GLOBAL.*DEFAULT.*@@LIBOSTREE_' | sed -e 's,^.* \(ostree_[A-Za-z0-9_]*\)@@LIBOSTREE_[0-9A-Z_.]*,\1,' |sort -u > found-symbols.txt
diff -u expected-symbols.txt found-symbols.txt
echo "ok exports"

# cmd__private__ is private.  The fetcher symbol should not have been made public.
grep -E -v '(ostree_cmd__private__)|(ostree_fetcher_config_flags_get_type)' found-symbols.txt > expected-documented.txt

echo "Verifying all public symbols are documented:"
grep '^ostree_' ${G_TEST_SRCDIR}/apidoc/ostree-sections.txt $experimental_sections |sort -u > found-documented.txt
diff -u expected-documented.txt found-documented.txt

echo 'ok documented symbols'
