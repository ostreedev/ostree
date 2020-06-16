#!/bin/bash
#
# Copyright (C) 2020 Collabora Ltd.
#
# SPDX-License-Identifier: LGPL-2.0+
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

. $(dirname $0)/libtest.sh

echo "1..1"

if ! has_sign_ed25519; then
    echo "ok pre-signed pull # SKIP due ed25519 unavailability"
    exit 0
fi

mkdir upstream
cd upstream
tar xzf $(dirname $0)/pre-signed-pull-data.tar.gz
cd ..

pubkey='45yzbkuEok0lLabxzdAHWUDSMZgYfxU40sN+LMfYHVA='

ostree --repo=repo init --mode=archive
ostree --repo=repo remote add upstream --set=gpg-verify=false --sign-verify=ed25519=inline:${pubkey} file://$(pwd)/upstream/repo
ostree --repo=repo pull upstream:testref

wrongkey=$(gen_ed25519_random_public)
rm repo -rf
ostree --repo=repo init --mode=archive
ostree --repo=repo remote add badupstream --set=gpg-verify=false --sign-verify=ed25519=inline:${wrongkey} file://$(pwd)/upstream/repo
if ostree --repo=repo pull badupstream:testref 2>err.txt; then
    fatal "pulled with wrong key"
fi
assert_file_has_content err.txt 'error:.* ed25519: Signature couldn.t be verified with: key'
echo "ok pre-signed pull"
