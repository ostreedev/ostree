#!/usr/bin/bash
set -xeuo pipefail

./tests/installed/provision.sh
# TODO: enhance papr to have caching, a bit like https://docs.travis-ci.com/user/caching/
cd tests/installed
# This should be https://getfedora.org/atomic_qcow2_latest but that's broken
curl -Lo fedora-atomic-host.qcow2 https://kojipkgs.fedoraproject.org/compose/twoweek/Fedora-Atomic-28-20180626.0/compose/AtomicHost/x86_64/images/Fedora-AtomicHost-28-20180626.0.x86_64.qcow2
exec env "TEST_SUBJECTS=$(pwd)/fedora-atomic-host.qcow2" ./run.sh
