#!/bin/bash
#
# Copyright © 2023 Endless OS Foundation LLC
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
# License along with this library. If not, see <https://www.gnu.org/licenses/>.
#
# Authors:
#  - Dan Nicholson <dbn@endlessos.org>

set -euo pipefail

. $(dirname $0)/libtest.sh

echo "1..2"

setup_fake_remote_repo2 "archive"
${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/repo summary -u
${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/repo refs > origin-refs
${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/repo refs --revision > origin-refs-revs

cd ${test_tmpdir}
rm -rf repo
ostree_repo_init repo --mode=archive
${OSTREE} remote add --no-sign-verify origin $(cat httpd-address)/ostree/repo

${OSTREE} remote refs origin > refs
sed 's/^/origin:/' origin-refs > expected-refs
assert_files_equal refs expected-refs

echo "ok remote refs listing"

${OSTREE} remote refs origin --revision > refs-revs
sed 's/^/origin:/' origin-refs-revs > expected-refs-revs
assert_files_equal refs-revs expected-refs-revs

echo "ok remote refs revisions"
