#!/bin/bash
#
# Copyright (C) 2026 Colin Walters <walters@verbum.org>
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
# Regression: a static-delta superblock is not signed. With gpg-verify
# on and gpg-verify-summary off (both defaults), a mirror can advertise
# an unsigned commit and serve a delta whose superblock names an
# already-present signed commit. That skips commit verification, delta
# parts can write the unsigned commit, and the ref follows the summary.
# The pull must reject that. A normal signed delta must still apply.

set -euo pipefail

. $(dirname $0)/libtest.sh

skip_without_user_xattrs

if ! python3 -c 'from gi.repository import GLib' >/dev/null 2>&1; then
    skip "python3-gi is required to rewrite a static delta superblock"
fi

echo '1..3'

# Rewrite the A->E superblock so its to-checksum is the already-present
# signed commit, and add a part that writes unsigned commit E. The on-disk
# delta path stays A-E, which is what the client requests.
rewrite_malicious_delta() {
    local repo=$1 from=$2 to=$3
    python3 - "$repo" "$from" "$to" <<'PY'
import hashlib
import sys
from pathlib import Path

from gi.repository import GLib

repo, from_rev, to_rev = sys.argv[1:]
sb_type = "(a{sv}tayay(a{sv}aya(say)sstayay)aya(uayttay)a(yaytt))"
part_type = "(a(uuu)aa(ayay)ayay)"
commit_type = "(a{sv}aya(say)sstayay)"
header_type = "(uayttay)"


def load(path, type_str):
    data = Path(path).read_bytes()
    return GLib.Variant.new_from_bytes(
        GLib.VariantType.new(type_str), GLib.Bytes.new(data), False
    )


def raw(variant):
    return bytes(variant.get_data_as_bytes().get_data())


def ay(data):
    return GLib.Variant.new_from_bytes(
        GLib.VariantType.new("ay"), GLib.Bytes.new(data), True
    )


def varuint(n):
    if n < 0x80:
        return bytes([n])
    if n < (1 << 14):
        return bytes([n & 0x7F | 0x80, (n >> 7) & 0x7F])
    raise SystemExit(f"varuint too big: {n}")


matches = list(Path(repo, "deltas").rglob("superblock"))
if len(matches) != 1:
    raise SystemExit(f"expected one superblock, found {matches}")
delta_dir = matches[0].parent
superblock = load(delta_dir / "superblock", sb_type)
commit_e = Path(repo, "objects", to_rev[:2], to_rev[2:] + ".commit").read_bytes()
load(Path(repo, "objects", to_rev[:2], to_rev[2:] + ".commit"), commit_type)

ops = b"S" + varuint(len(commit_e)) + varuint(0)
payload = GLib.Variant.new_tuple(
    GLib.Variant.new_array(GLib.VariantType.new("(uuu)"), []),
    GLib.Variant.new_array(GLib.VariantType.new("a(ayay)"), []),
    ay(commit_e),
    ay(ops),
)
if payload.get_type_string() != part_type:
    raise SystemExit(f"unexpected part type {payload.get_type_string()}")
part_file = b"\x00" + raw(payload)
objects = bytes([4]) + bytes.fromhex(to_rev)  # OSTREE_OBJECT_TYPE_COMMIT
header = GLib.Variant.new_tuple(
    GLib.Variant.new_uint32(0),
    ay(hashlib.sha256(part_file).digest()),
    GLib.Variant.new_uint64(len(part_file)),
    GLib.Variant.new_uint64(len(commit_e)),
    ay(objects),
)
if header.get_type_string() != header_type:
    raise SystemExit(f"unexpected header type {header.get_type_string()}")

old_headers = superblock.get_child_value(6)
headers = GLib.Variant.new_array(
    GLib.VariantType.new(header_type),
    [old_headers.get_child_value(i) for i in range(old_headers.n_children())] + [header],
)
new_sb = GLib.Variant.new_tuple(
    superblock.get_child_value(0),
    superblock.get_child_value(1),
    superblock.get_child_value(2),
    ay(bytes.fromhex(from_rev)),
    load(Path(repo, "objects", from_rev[:2], from_rev[2:] + ".commit"), commit_type),
    superblock.get_child_value(5),
    headers,
    superblock.get_child_value(7),
)
if new_sb.get_type_string() != sb_type or not new_sb.is_normal_form():
    raise SystemExit(f"bad superblock {new_sb.get_type_string()}")
(delta_dir / "1").write_bytes(part_file)
(delta_dir / "superblock").write_bytes(raw(new_sb))
print(f"rewrote {delta_dir}")
PY
}

cd ${test_tmpdir}
mkdir srv
ostree_repo_init srv --mode=archive
mkdir files
echo signed-good > files/content
${CMD_PREFIX} ostree --repo=srv commit --gpg-sign=${TEST_GPG_KEYID_1} \
    --gpg-homedir=${TEST_GPG_KEYHOME} -b main -s signed --tree=dir=files
A=$(${CMD_PREFIX} ostree --repo=srv rev-parse main)

echo evil-unsigned > files/content
echo extra > files/extra
${CMD_PREFIX} ostree --repo=srv commit -b evil -s "unsigned evil" --tree=dir=files
E=$(${CMD_PREFIX} ostree --repo=srv rev-parse evil)

mkdir client
ostree_repo_init client --mode=bare-user
${CMD_PREFIX} ostree --repo=client remote add \
    --gpg-import=${TEST_GPG_KEYHOME}/key1.asc \
    --set=gpg-verify=true --set=gpg-verify-summary=false \
    origin file://${test_tmpdir}/srv
${CMD_PREFIX} ostree --repo=client pull --disable-static-deltas origin main
assert_streq $(${CMD_PREFIX} ostree --repo=client rev-parse origin:main) ${A}

# Same commit, fetched as an object, must not pass GPG.
if ${CMD_PREFIX} ostree --repo=client pull --disable-static-deltas origin evil 2>err.txt; then
    assert_not_reached "object pull of unsigned commit unexpectedly succeeded"
fi
assert_file_has_content err.txt "no signatures found"
assert_streq $(${CMD_PREFIX} ostree --repo=client rev-parse origin:main) ${A}
echo "ok object pull rejects unsigned commit"

# Prove the crafted delta is something pull will actually execute, then
# show that commit verification must still reject it.
${CMD_PREFIX} ostree --repo=srv reset main ${E}
${CMD_PREFIX} ostree --repo=srv static-delta generate --from=${A} --to=${E}
rewrite_malicious_delta srv ${A} ${E}
${CMD_PREFIX} ostree --repo=srv summary -u

mkdir client-nocheck
ostree_repo_init client-nocheck --mode=bare-user
${CMD_PREFIX} ostree --repo=client-nocheck remote add \
    --set=gpg-verify=false --set=gpg-verify-summary=false \
    origin file://${test_tmpdir}/srv
${CMD_PREFIX} ostree --repo=client-nocheck pull --disable-static-deltas origin ${A}
${CMD_PREFIX} ostree --repo=client-nocheck pull --require-static-deltas origin main \
    >nocheck-pull.txt
assert_file_has_content nocheck-pull.txt "delta parts"
assert_streq $(${CMD_PREFIX} ostree --repo=client-nocheck rev-parse origin:main) ${E}
rm -rf nocheck-checkout
${CMD_PREFIX} ostree --repo=client-nocheck checkout origin:main nocheck-checkout
assert_file_has_content nocheck-checkout/content evil-unsigned
assert_file_has_content nocheck-checkout/extra extra

if ${CMD_PREFIX} ostree --repo=client pull --require-static-deltas origin main 2>err.txt; then
    assert_not_reached "gpg-verify pull applied a static delta for unsigned commit ${E}"
fi
assert_streq $(${CMD_PREFIX} ostree --repo=client rev-parse origin:main) ${A}
# A fixture failure would also fail the gpg-verify=false pull above.
assert_file_has_content err.txt "superblock targets commit ${A}, expected ${E}\|no signatures found\|GPG verification"
echo "ok malicious static delta does not deploy unsigned commit"

# A real delta between two signed commits must still apply.
echo signed-update > files/content
${CMD_PREFIX} ostree --repo=srv commit --gpg-sign=${TEST_GPG_KEYID_1} \
    --gpg-homedir=${TEST_GPG_KEYHOME} -b main -s "signed update" --tree=dir=files
B=$(${CMD_PREFIX} ostree --repo=srv rev-parse main)
${CMD_PREFIX} ostree --repo=srv static-delta generate --from=${A} --to=${B}
${CMD_PREFIX} ostree --repo=srv summary -u
${CMD_PREFIX} ostree --repo=client pull --require-static-deltas origin main >signed-pull.txt
assert_file_has_content signed-pull.txt "delta parts"
assert_streq $(${CMD_PREFIX} ostree --repo=client rev-parse origin:main) ${B}
${CMD_PREFIX} ostree --repo=client show origin:main >show-signed.txt
assert_file_has_content show-signed.txt "Good signature"
echo "ok signed static delta still applies"
