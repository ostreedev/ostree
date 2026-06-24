#!/bin/bash
set -xeuo pipefail

# Prow jobs don't support adding emptydir today
export COSA_SKIP_OVERLAY=1
ostree --version
cd $(mktemp -d)
cosa init https://github.com/coreos/fedora-coreos-config/
# Use RPM overrides so cosa/rpm-ostree resolves dependencies properly
cp /cosa/rpms/ostree-*.rpm overrides/rpm/
# Remove -devel and -debug RPMs that aren't needed in the compose
rm -f overrides/rpm/ostree-devel-* overrides/rpm/ostree-debug*
cosa fetch
# For composefs
echo 'rootfs: "ext4verity"' >> src/config/image.yaml
cosa build
# For now, Prow just runs the composefs tests, since Jenkins covers the others
#cosa kola run 'ext.ostree.destructive-rs.composefs*'
