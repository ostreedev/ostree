#!/bin/bash
set -xeuo pipefail

# Prow jobs don't support adding emptydir today
export COSA_SKIP_OVERLAY=1
# And suppress depcheck since we didn't install via RPM
export COSA_SUPPRESS_DEPCHECK=1
ostree --version
cd $(mktemp -d)
cosa init https://github.com/coreos/fedora-coreos-config/
rsync -rlv /cosa/component-install/ overrides/rootfs/
cosa fetch
# For composefs
echo 'rootfs: "ext4verity"' >> src/config/image.yaml
cosa build
# For now, Prow just runs the composefs tests, since Jenkins covers the others
#cosa kola run 'ext.ostree.destructive-rs.composefs*'
