#!/bin/bash

# Test commit --selinux-policy

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libinsttest.sh

cd /ostree/repo/tmp
rm co -rf
ostree checkout -H ${host_refspec} co
testbin=co/usr/bin/foo-a-generic-binary
assert_not_has_file "${testbin}"
# Make a test binary that we label as shell_exec_t on disk, but should be
# reset by --selinux-policy back to bin_t
echo 'test foo' > ${testbin}
chcon --reference co/usr/bin/true ${testbin}
oldcon=$(getfattr --only-values -m security.selinux ${testbin})
chcon --reference co/usr/bin/bash ${testbin}
newcon=$(getfattr --only-values -m security.selinux ${testbin})
assert_not_streq "${oldcon}" "${newcon}"
ostree commit -b testbranch --link-checkout-speedup \
       --selinux-policy co --tree=dir=co
ostree ls -X testbranch /usr/bin/foo-a-generic-binary > ls.txt
assert_file_has_content ls.txt ${oldcon}
ostree fsck

ostree refs --delete testbranch
rm co -rf
echo "ok commit with sepolicy"

# Now let's check that selinux policy labels can be applied on checkout

rm rootfs -rf
if ostree checkout -H \
    --selinux-policy / ${host_refspec} co; then
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

ostree refs --delete testbranch
rm co -rf
echo "ok checkout with sepolicy and selinux-prefix"
