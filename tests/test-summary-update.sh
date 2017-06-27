#!/bin/bash
#
# Copyright © 2017 Endless Mobile, Inc.
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
#
# Authors:
#  - Philip Withnall <withnall@endlessm.com>

set -euo pipefail

. $(dirname $0)/libtest.sh

echo "1..2"

cd ${test_tmpdir}
mkdir repo
ostree_repo_init repo

mkdir -p tree/root
touch tree/root/a

# Add a few commits
seq 5 | while read i; do
    echo a >> tree/root/a
    ${CMD_PREFIX} ostree --repo=repo commit --branch=test-$i -m test -s test tree
done

# Generate a plain summary file.
${CMD_PREFIX} ostree --repo=repo summary --update

# Generate a signed summary file.
${CMD_PREFIX} ostree --repo=repo summary --update --gpg-homedir=${TEST_GPG_KEYHOME} --gpg-sign=${TEST_GPG_KEYID_1}

# Try various ways of adding additional data.
${CMD_PREFIX} ostree --repo=repo summary --update --add-metadata key="'value'" --add-metadata=key2=true
${CMD_PREFIX} ostree --repo=repo summary --update -m some-int='@t 123'
${CMD_PREFIX} ostree --repo=repo summary --update --add-metadata=map='@a{sv} {}'

# Check the additional metadata turns up in the output.
${CMD_PREFIX} ostree --repo=repo summary --view > summary
assert_file_has_content summary "^map: {}$"

echo "ok 1 update summary"

# Test again, but with collections enabled in the repository (if supported).
if ! ostree --version | grep -q -e '- experimental'; then
    echo "ok 2 # skip No experimental API is compiled in"
    exit 0
fi

cd ${test_tmpdir}
rm -rf repo
ostree_repo_init repo --collection-id org.example.Collection1

mkdir -p tree/root
touch tree/root/a

# Add a few commits
seq 5 | while read i; do
    echo a >> tree/root/a
    ${CMD_PREFIX} ostree --repo=repo commit --branch=test-$i -m test -s test tree
    ${CMD_PREFIX} ostree --repo=repo refs --collections --create=org.example.Collection2:test-$i test-$i
done

# Generate a plain summary file.
${CMD_PREFIX} ostree --repo=repo summary --update

# Generate a signed summary file.
${CMD_PREFIX} ostree --repo=repo summary --update --gpg-homedir=${TEST_GPG_KEYHOME} --gpg-sign=${TEST_GPG_KEYID_1}

# Try various ways of adding additional data.
${CMD_PREFIX} ostree --repo=repo summary --update --add-metadata key="'value'" --add-metadata=key2=true
${CMD_PREFIX} ostree --repo=repo summary --update -m some-int='@t 123'
${CMD_PREFIX} ostree --repo=repo summary --update --add-metadata=map='@a{sv} {}'

# Check the additional metadata turns up in the output.
${CMD_PREFIX} ostree --repo=repo summary --view > summary
assert_file_has_content summary "^map: {}$"

echo "ok 2 update summary with collections"
