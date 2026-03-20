#!/bin/bash
# Prepare the RPM spec file for building from source.
# Downloads the spec from Fedora dist-git if not present, then patches it
# for the current git version.
# Used by both the Dockerfile rpmbuild stage and .packit.yaml.
set -xeuo pipefail

spec="${1:-ostree.spec}"

if [ ! -f "$spec" ]; then
    curl -LO https://src.fedoraproject.org/rpms/ostree/raw/rawhide/f/ostree.spec
fi

version=$(git describe --always --tags --match 'v2???.*' | sed -e 's,-,\.,g' -e 's,^v,,')
sed -i "s,^Version:.*,Version: ${version}," "$spec"
sed -i 's/^Patch/# Patch/g' "$spec"
sed -i 's,%autorelease,1%{?dist},g' "$spec"
