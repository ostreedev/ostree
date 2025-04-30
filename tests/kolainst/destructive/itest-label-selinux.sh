#!/bin/bash

# Test commit --selinux-policy

set -xeuo pipefail

. ${KOLA_EXT_DATA}/libinsttest.sh
require_writable_sysroot
prepare_tmpdir /var/tmp

date
cd /ostree/repo/tmp
rm co -rf
ostree checkout -H ${host_commit} co
testbin=co/usr/bin/foo-a-generic-binary
assert_not_has_file "${testbin}"
# Make a test binary that we label as shell_exec_t on disk, but should be
# reset by --selinux-policy back to bin_t
echo 'test foo' > ${testbin}
# Extend it with some real data to help test reflinks
cat < /usr/bin/bash >> "${testbin}"
# And set its label
chcon --reference co/usr/bin/true ${testbin}
# Now at this point, it should not have shared extents
filefrag -v "${testbin}" > out.txt
assert_not_file_has_content out.txt shared
rm -f out.txt
oldcon=$(getfattr --only-values -m security.selinux ${testbin})
chcon --reference co/usr/bin/bash ${testbin}
newcon=$(getfattr --only-values -m security.selinux ${testbin})
assert_not_streq "${oldcon}" "${newcon}"
# Ensure the tmpdir isn't pruned
touch co
ostree commit -b testbranch --link-checkout-speedup \
       --selinux-policy co --tree=dir=co
ostree ls -X testbranch /usr/bin/foo-a-generic-binary > ls.txt
assert_file_has_content ls.txt ${oldcon}
ostree fsck
# Additionally, the commit process should have reflinked our input
filefrag -v "${testbin}" > out.txt
assert_file_has_content out.txt shared
rm -f out.txt

ostree refs --delete testbranch
rm co -rf
echo "ok commit with sepolicy"

ostree ls -X ${host_commit} /usr/etc/sysctl.conf > ls.txt
if grep -qF ':etc_t:' ls.txt; then
  ostree checkout -H ${host_commit} co
  ostree commit -b testbranch --link-checkout-speedup \
       --selinux-policy co --tree=dir=co --selinux-labeling-epoch=1
  ostree ls -X testbranch /usr/etc/sysctl.conf > ls.txt
  assert_file_has_content ls.txt ':system_conf_t:'
  rm co ls.txt -rf
  ostree refs --delete testbranch
else
  echo 'Already using --selinux-labeling-epoch > 0 on host, hopefully!'
fi

echo "ok --selinux-labeling-epoch=1"

# Now let's check that selinux policy labels can be applied on checkout

rm rootfs -rf
if ostree checkout -H \
    --selinux-policy / ${host_commit} co; then
  assert_not_reached "checked out with -H and --selinux-policy"
fi
# recommit just two binaries into a new branch with selinux labels stripped
mkdir -p rootfs/usr/bin
oldcon=$(getfattr --only-values -m security.selinux /usr/bin/bash)
cp /usr/bin/{true,bash} rootfs/usr/bin
newcon=$(getfattr --only-values -m security.selinux rootfs/usr/bin/bash)
assert_not_streq "${oldcon}" "${newcon}"
echo "ok checkout with sepolicy setup"

ostree commit -b testbranch rootfs
ostree checkout testbranch --selinux-policy / co
newcon=$(getfattr --only-values -m security.selinux co/usr/bin/bash)
assert_streq "${oldcon}" "${newcon}"
rm co -rf
echo "ok checkout with sepolicy"
ostree checkout testbranch --selinux-policy / --subpath /usr/bin co
newcon=$(getfattr --only-values -m security.selinux co/bash)
assert_streq "${oldcon}" "${newcon}"
rm co -rf
echo "ok checkout with sepolicy and subpath"

# now commit tree with mismatched leading dirs
mkdir -p rootfs/subdir
mv rootfs/{usr,subdir}
ostree commit -b testbranch rootfs
ostree checkout testbranch --selinux-policy / co
newcon=$(getfattr --only-values -m security.selinux co/subdir/usr/bin/bash)
assert_not_streq "${oldcon}" "${newcon}"
rm co -rf
ostree checkout testbranch --selinux-policy / \
  --subpath subdir --selinux-prefix / co
newcon=$(getfattr --only-values -m security.selinux co/usr/bin/bash)
assert_streq "${oldcon}" "${newcon}"
rm co -rf
echo "ok checkout with sepolicy and selinux-prefix"

# Now check that combining --selinux-policy with --skip-list doesn't blow up
echo > skip-list.txt << EOF
/usr/bin/true
EOF
ostree checkout testbranch --selinux-policy / --skip-list skip-list.txt \
  --subpath subdir --selinux-prefix / co
! test -f co/usr/bin/true
test -f co/usr/bin/bash
newcon=$(getfattr --only-values -m security.selinux co/usr/bin/bash)
assert_streq "${oldcon}" "${newcon}"
rm co -rf
ostree refs --delete testbranch
echo "ok checkout selinux and skip-list"
date

mkdir -p usr/{bin,lib,etc}
echo 'somebinary' > usr/bin/somebinary
ls -Z usr/bin/somebinary > lsz.txt
assert_not_file_has_content lsz.txt ':bin_t:'
rm -f lsz.txt
echo 'somelib' > usr/lib/somelib.so
echo 'someconf' > usr/etc/some.conf
ostree commit -b newbase --selinux-policy-from-base --tree=ref=${host_commit} --tree=dir=$(pwd)
ostree ls -X newbase /usr/bin/somebinary > newls.txt
assert_file_has_content newls.txt ':bin_t:'
ostree ls -X newbase /usr/lib/somelib.so > newls.txt
assert_file_has_content newls.txt ':lib_t:'
ostree ls -X newbase /usr/etc/some.conf > newls.txt
assert_file_has_content newls.txt ':etc_t:'
echo "ok commit --selinux-policy-from-base"

rm rootfs -rf
mkdir rootfs
mkdir -p rootfs/usr/{bin,lib,etc}
echo 'somebinary' > rootfs/usr/bin/somebinary
ls -Z rootfs/usr/bin/somebinary > lsz.txt
assert_not_file_has_content lsz.txt ':bin_t:'
rm -f lsz.txt
tar -C rootfs -cf rootfs.tar .
ostree commit -b newbase --selinux-policy / --tree=tar=rootfs.tar
ostree ls -X newbase /usr/bin/somebinary > newls.txt
assert_file_has_content newls.txt ':bin_t:'
echo "ok commit --selinux-policy with --tree=tar"
