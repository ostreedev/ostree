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

echo '1..1'

grep ' ostree_[A-Za-z0-9_]*;' ${G_TEST_SRCDIR}/src/libostree/libostree.sym | sed -e 's,^ *\([A-Za-z0-9_]*\);,\1,' | sort -u > expected-symbols.txt
eu-readelf -a ${G_TEST_BUILDDIR}/.libs/libostree-1.so | grep 'FUNC.*GLOBAL.*DEFAULT.*@@LIBOSTREE_' | sed -e 's,^.* \(ostree_[A-Za-z0-9_]*\)@@LIBOSTREE_[0-9_.]*,\1,' |sort -u > found-symbols.txt
diff -u expected-symbols.txt found-symbols.txt

echo 'ok'
