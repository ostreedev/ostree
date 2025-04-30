#!/bin/bash

# Tests of the "raw ostree" functionality using the host's ostree repo as uid 0.

set -xeuo pipefail

. ${KOLA_EXT_DATA}/libinsttest.sh

echo "1..2"
date

require_writable_sysroot

cd /ostree/repo/tmp
rm co -rf
rm co-testref -rf
ostree refs --delete testref
ostree checkout -H ${host_commit} co
victim_symlink=/usr/bin/gtar  # Seems likely to stick around
# Copy the link to avoid corrupting it
cp co/${victim_symlink}{,.tmp}
mv co/${victim_symlink}{.tmp,}
# Add another xattr to a symlink and a directory, since otherwise this is unusual
setfattr -n security.biometric -v iris co/${victim_symlink}
setfattr -n security.crunchy -v withketchup co/usr/bin
csum=$(ostree commit -b testref --link-checkout-speedup --tree=dir=co)
ostree fsck
ostree ls -X testref ${victim_symlink} > ls.txt
assert_file_has_content ls.txt 'security\.biometric'
ostree ls -X ${host_commit} ${victim_symlink} > ls.txt
assert_not_file_has_content ls.txt 'security\.biometric'
ostree ls -X testref usr/bin > ls.txt
assert_file_has_content ls.txt 'security\.crunchy'

ostree checkout -H testref co-testref
getfattr -n security.biometric co-testref/${victim_symlink} > xattr.txt
assert_file_has_content xattr.txt 'security\.biometric="iris"'
getfattr -n security.crunchy co-testref/usr/bin > xattr.txt
assert_file_has_content xattr.txt 'security\.crunchy="withketchup"'

rm co -rf
rm co-testref -rf

echo "ok xattrs"
date
