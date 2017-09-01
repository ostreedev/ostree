#!/bin/bash
#
# Copyright (C) 2016 Colin Walters <walters@verbum.org>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

set -euo pipefail

. $(dirname $0)/libtest.sh

skip_without_fuse
skip_without_user_xattrs

echo "1..1"

# Run "triggers" like ldconfig, gtk-update-icon-cache, etc.
demo_triggers() {
    root=$1
    shift
    mkdir -p ${root}/usr/lib
    echo updated ldconfig at $(date) > ${root}/usr/lib/ld.so.cache.new
    mv ${root}/usr/lib/ld.so.cache{.new,}
}

# Make a binary in /usr/bin/$pkg which contains $version
exampleos_build_commit_package() {
    pkg=$1
    version=$2
    mkdir -p ${pkg}-package/usr/bin/
    echo "${pkg}-content ${version}" > ${pkg}-package/usr/bin/${pkg}
    # Use a dummy subject for this.
    ${CMD_PREFIX} ostree --repo=build-repo commit -b exampleos/x86_64/${pkg} -s '' --tree=dir=${pkg}-package
    rm ${pkg}-package -rf
}

exampleos_recompose() {
    rm exampleos-build -rf
    for pkg in ${packages}; do
	${CMD_PREFIX} ostree --repo=build-repo checkout -U --union exampleos/x86_64/${pkg} exampleos-build
    done

    # Now that we have our rootfs, run triggers
    rofiles-fuse exampleos-build mnt
    demo_triggers mnt/
    fusermount -u mnt
    
    # Then we commit it, using --link-checkout-speedup to effectively
    # only re-checksum the ldconfig file.  We also have dummy commit
    # message here.
    ${CMD_PREFIX} ostree --repo=build-repo commit -b exampleos/x86_64/standard -s 'exampleos build' --link-checkout-speedup exampleos-build
}

packages="bash systemd"

mkdir build-repo
ostree_repo_init build-repo --mode=bare-user
mkdir repo
ostree_repo_init repo --mode=archive
# Our FUSE mount point
mkdir mnt

# "Build" some packages which are really just files with
# the version number inside.
exampleos_build_commit_package bash 0.4.7
exampleos_build_commit_package systemd 224

# Now union the packages and commit
exampleos_recompose

# This is our first commit - let's publish it.
${CMD_PREFIX} ostree --repo=repo pull-local build-repo exampleos/x86_64/standard

# Now, update the bash package - this is a new commit on the branch
# exampleos/x86_64/bash.
exampleos_build_commit_package bash 0.5.0

# We now have two commits
exampleos_recompose

# Publish again:
${CMD_PREFIX} ostree --repo=repo pull-local build-repo exampleos/x86_64/standard
# Optional: Generate a static delta vs the previous build
${CMD_PREFIX} ostree --repo=repo static-delta generate exampleos/x86_64/standard
# Optional: Regenerate the summary file
${CMD_PREFIX} ostree --repo=repo summary -u

# Try: ostree --repo=demo-repo ls -R exampleos/x86_64/standard

echo "ok demo buildsystem"
