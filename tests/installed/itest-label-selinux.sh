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
ostree --repo=/ostree/repo commit -b testbranch --link-checkout-speedup \
       --selinux-policy co --tree=dir=co
ostree --repo=/ostree/repo ls -X testbranch /usr/bin/foo-a-generic-binary > ls.txt
assert_file_has_content ls.txt ${oldcon}

ostree --repo=/ostree/repo refs --delete testbranch
rm co -rf
