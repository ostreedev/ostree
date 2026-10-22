#!/bin/bash

# A deployment with a compiled SELinux policy but no module store can
# still be deployed: the semodule refresh is skipped.

set -xeuo pipefail

. ${KOLA_EXT_DATA}/libinsttest.sh

require_writable_sysroot

policy_type=$(sed -ne 's/^SELINUXTYPE=//p' /etc/selinux/config)
if ! test -d /usr/etc/selinux/${policy_type}/active; then
    echo "ok no module store in the host commit, nothing to test"
    exit 0
fi

cd /ostree/repo/tmp
ostree checkout --fsync=0 -H ${host_commit} test-no-store
rm -rf test-no-store/usr/etc/selinux/${policy_type}/active
# Otherwise the /etc merge keeps any locally modified store files.
rm -rf /etc/selinux/${policy_type}/active
ostree commit --link-checkout-speedup --selinux-policy=test-no-store -b test-no-store \
    --consume --tree=dir=test-no-store

since=$(date '+%Y-%m-%d %H:%M:%S')
ostree admin deploy --karg-proc-cmdline test-no-store
journalctl --since "${since}" > journal.txt
assert_file_has_content_literal journal.txt \
    "No SELinux policy store at /etc/selinux/${policy_type}/active, skipping policy refresh"

new_deployment_path=/ostree/deploy/${host_osname}/deploy/$(ostree rev-parse test-no-store).0
test -f ${new_deployment_path}/etc/selinux/${policy_type}/policy/policy.*
echo "ok deployed without a module store"

ostree admin undeploy 0
ostree refs --delete test-no-store
