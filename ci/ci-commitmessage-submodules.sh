#!/bin/bash
set -xeuo pipefail

dn=$(dirname $0)
. ${dn}/libbuild.sh

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

# If passed the commit, use that. Otherwise, just use HEAD.
HEAD=${1:-HEAD}

tmpd=$(mktemp -d)
touch ${tmpd}/.tmpdir
cleanup_tmp() {
    # This sanity check ensures we don't delete something else
    if test -f ${tmpd}/.tmpdir; then
        rm -rf ${tmpd}
    fi
}
trap cleanup_tmp EXIT

if ! [ -x /usr/bin/git ]; then
  pkg_upgrade
  pkg_install git
fi

gitdir=$(realpath $(pwd))
# Create a temporary copy of this (using cp not git clone) so git doesn't
# try to read the submodules from the Internet again.  If we wanted to
# require a newer git, we could use `git worktree`.
cp -a ${gitdir} ${tmpd}/workdir
cd ${tmpd}/workdir
git log --pretty=oneline origin/master..$HEAD | while read logline; do
    commit=$(echo ${logline} | cut -f 1 -d ' ')
    # For merge commits, just check that they're empty (i.e. no conflict
    # resolution was needed). Otherwise, let's just error out. Conflicts should
    # be resolved by rebasing the PR.
    # https://stackoverflow.com/questions/3824050#comment82244548_13956422
    if [ "$(git rev-list --no-walk --count --merges ${commit})" -ne 0 ]; then
      if [ -n "$(git diff-tree ${commit})" ]; then
        echo "error: non-empty git merge: resolve conflicts by rebasing!"
        exit 1
      fi
      echo "Commit ${commit} is an empty merge commit; ignoring..."
      continue
    fi
    git diff --name-only ${commit}^..${commit} > ${tmpd}/diff.txt
    git log -1 ${commit} > ${tmpd}/log.txt
    echo "Validating commit for submodules: $commit"
    sed -e 's,^,# ,' < ${tmpd}/log.txt
    git checkout -q "${commit}"
    git submodule update --init
    git submodule foreach --quiet 'echo $path'| while read submodule; do
        if grep -q -e '^'${submodule} ${tmpd}/diff.txt; then
            echo "Commit $commit modifies submodule: $submodule"
            expected_match="Update submodule: $submodule"
            # check if it's from dependabot
            if grep -q -e '^Author: dependabot' ${tmpd}/log.txt; then
              echo "Commit $commit contains bump from Dependabot"
              continue
            fi
            if ! grep -q -e "$expected_match" ${tmpd}/log.txt; then
                echo "error: Commit message for ${commit} changes a submodule, but does not match regex ${expected_match}"
                exit 1
            fi
            echo "Verified commit $commit matches regexp ${expected_match}"
        fi
    done
done
