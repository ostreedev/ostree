#!/bin/bash

# Verify our /etc merge works with selinux

set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libinsttest.sh

# Create a new deployment
ostree admin deploy --karg-proc-cmdline ${host_refspec}
new_deployment_path=/ostree/deploy/${host_osname}/deploy/${host_commit}.1

# A set of files that have a variety of security contexts
for file in fstab passwd exports hostname sysctl.conf yum.repos.d \
            NetworkManager/dispatcher.d/hook-network-manager; do
    if ! test -e /etc/${file}; then
        continue
    fi

    current=$(cd /etc && ls -Z ${file})
    new=$(cd ${new_deployment_path}/etc && ls -Z ${file})
    assert_streq "${current}" "${new}"
done

ostree admin undeploy 0
