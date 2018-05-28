#!/usr/bin/bash
set -xeuo pipefail

./tests/installed/provision.sh
# TODO: enhance papr to have caching, a bit like https://docs.travis-ci.com/user/caching/
cd tests/installed
curl -Lo fedora-atomic-host.qcow2 https://getfedora.org/atomic_qcow2_latest
exec env "TEST_SUBJECTS=$(pwd)/fedora-atomic-host.qcow2" ./run.sh
