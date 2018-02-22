#!/bin/bash
set -euo pipefail

# Copyright 2017 Colin Walters <walters@verbum.org>
# Licensed under the new-BSD license (http://www.opensource.org/licenses/bsd-license.php)

# This script is intended to be used as a CI gating check
# that if a submodule is changed, the commit message contains
# the text:
#
#  Update submodule: submodulepath
#
# It's very common for people to accidentally change submodules, and having this
# requirement is a small hurdle to pass.

# if running under PAPR, use the branch/PR HEAD actually
# being tested rather than the merge sha
HEAD=${PAPR_COMMIT:-HEAD}
dn=$(dirname $0)
. ${dn}/libpaprci/libbuild.sh

tmpd=$(mktemp -d)
touch ${tmpd}/.tmpdir
cleanup_tmp() {
    # This sanity check ensures we don't delete something else
    if test -f ${tmpd}/.tmpdir; then
        rm -rf ${tmpd}
    fi
}
trap cleanup_tmp EXIT

pkg_upgrade
pkg_install git

gitdir=$(realpath $(pwd))
# Create a temporary copy of this (using cp not git clone) so git doesn't
# try to read the submodules from the Internet again.  If we wanted to
# require a newer git, we could use `git worktree`.
cp -a ${gitdir} ${tmpd}/workdir
cd ${tmpd}/workdir
git log --pretty=oneline origin/master..$HEAD | while read logline; do
    commit=$(echo ${logline} | cut -f 1 -d ' ')
    git diff --name-only ${commit}^..${commit} > ${tmpd}/diff.txt
    git log -1 ${commit} > ${tmpd}/log.txt
    echo "Validating commit for submodules: $commit"
    git checkout -q "${commit}"
    git submodule update --init
    git submodule foreach --quiet 'echo $path'| while read submodule; do
        if grep -q -e '^'${submodule} ${tmpd}/diff.txt; then
            echo "Commit $commit modifies submodule: $submodule"
            expected_match="Update submodule: $submodule"
            if ! grep -q -e "$expected_match" ${tmpd}/log.txt; then
                sed -e 's,^,# ,' < ${tmpd}/log.txt
                echo "error: Commit message for ${commit} changes a submodule, but does not match regex ${expected_match}"
                exit 1
            fi
            echo "Verified commit $commit matches regexp ${expected_match}"
        fi
    done
done
