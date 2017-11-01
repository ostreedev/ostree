# This file is to be sourced, not executed

# Copyright (C) 2011 Colin Walters <walters@verbum.org>
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

function repo_init() {
    cd ${test_tmpdir}
    rm repo -rf
    mkdir repo
    ostree_repo_init repo --mode=${repo_mode}
    ${CMD_PREFIX} ostree --repo=repo remote add origin $(cat httpd-address)/ostree/gnomerepo "$@"
}

repo_init --no-gpg-verify

# See also the copy of this in basic-test.sh
COMMIT_ARGS=""
CHECKOUT_U_ARG=""
CHECKOUT_H_ARGS="-H"
if is_bare_user_only_repo repo; then
    COMMIT_ARGS="--canonical-permissions"
    # Also, since we can't check out uid=0 files we need to check out in user mode
    CHECKOUT_U_ARG="-U"
    CHECKOUT_H_ARGS="-U -H"
else
    if grep -E -q '^mode=bare-user' repo/config; then
        CHECKOUT_H_ARGS="-U -H"
    fi
fi

echo "1..1"
cd ${test_tmpdir}
repo_init --no-gpg-verify
prev_rev=$(ostree --repo=ostree-srv/repo rev-parse ${remote_ref}^)
rev=$(ostree --repo=ostree-srv/repo rev-parse ${remote_ref})
${CMD_PREFIX} ostree --repo=ostree-srv/repo static-delta generate ${remote_ref}
${CMD_PREFIX} ostree --repo=ostree-srv/repo summary -u
${CMD_PREFIX} ostree --repo=repo pull origin ${remote_ref}@${prev_rev}
${CMD_PREFIX} ostree --repo=repo pull --dry-run --require-static-deltas origin ${remote_ref} >dry-run-pull.txt
assert_file_has_content dry-run-pull.txt 'Delta update: 0/1 parts, 0 bytes/[45][0-9].[0-9] kB, 1.[678] MB total uncompressed'
${CMD_PREFIX} ostree --repo=repo pull --require-static-deltas origin ${remote_ref}
final_rev=$(${CMD_PREFIX} ostree --repo=repo rev-parse origin:${remote_ref})
assert_streq "${rev}" "${final_rev}"
${CMD_PREFIX} ostree --repo=repo fsck
echo "ok delta"
