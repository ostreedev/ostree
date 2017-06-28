#!/bin/bash
#
# Copyright © 2017 Endless Mobile, Inc.
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

echo '1..6'

cd ${test_tmpdir}

do_commit() {
    local repo=$1
    local branch=$2
    shift 2

    mkdir -p files
    pushd files
    ${CMD_PREFIX} ostree --repo="../${repo}" commit -s "Test ${repo} commit for branch ${branch}" -b "${branch}" --gpg-homedir="${TEST_GPG_KEYHOME}" --gpg-sign="${TEST_GPG_KEYID_1}" "$@" > "../${branch}-checksum"
    popd
}

do_summary() {
    local repo=$1
    shift 1

    ${CMD_PREFIX} ostree "--repo=${repo}" summary --update --gpg-homedir="${TEST_GPG_KEYHOME}" --gpg-sign="${TEST_GPG_KEYID_1}"
}

do_collection_ref_show() {
    local repo=$1
    local branch=$2
    shift 2

    echo -n "collection ID: "
    if ${CMD_PREFIX} ostree "--repo=${repo}" show --print-metadata-key=ostree.collection-binding $(cat "${branch}-checksum")
    then :
    else return 1
    fi
    echo -n "refs: "
    if ${CMD_PREFIX} ostree "--repo=${repo}" show --print-metadata-key=ostree.ref-binding $(cat "${branch}-checksum")
    then return 0
    else return 1
    fi
}

ensure_no_collection_ref() {
    local repo=$1
    local branch=$2
    local error=$3
    shift 3

    if do_collection_ref_show "${repo}" "${branch}" >/dev/null
    then
	assert_not_reached "${error}"
    fi
}

do_join() {
    local IFS=', '
    echo "$*"
}

ensure_collection_ref() {
    local repo=$1
    local branch=$2
    local collection_id=$3
    shift 3
    local refs=$(do_join "$@")

    do_collection_ref_show "${repo}" "${branch}" >"${branch}-meta"
    assert_file_has_content "${branch}-meta" "^collection ID: '${collection_id}'$"
    assert_file_has_content "${branch}-meta" "^refs: \['${refs}'\]$"
}

do_remote_add() {
    local repo=$1
    local remote_repo=$2
    shift 2

    ${CMD_PREFIX} ostree "--repo=${repo}" remote add "${remote_repo}-remote" "file://$(pwd)/${remote_repo}" "$@" --gpg-import="${test_tmpdir}/gpghome/key1.asc"
}

do_pull() {
    local repo=$1
    local remote_repo=$2
    local branch=$3
    shift 3

    if ${CMD_PREFIX} ostree "--repo=${repo}" pull "${remote_repo}-remote" "${branch}"
    then return 0
    else return 1
    fi
}

do_local_pull() {
    local repo=$1
    local remote_repo=$2
    local branch=$3
    shift 3

    if ${CMD_PREFIX} ostree "--repo=${repo}" pull-local "${remote_repo}" "${branch}"
    then return 0
    else return 1
    fi
}

# Create a repo without the collection ID.
mkdir no-collection-repo
ostree_repo_init no-collection-repo
do_commit no-collection-repo goodncref1
do_commit no-collection-repo sortofbadncref1 --add-metadata-string=ostree.collection-binding=org.example.Ignored
do_summary no-collection-repo
ensure_no_collection_ref \
    no-collection-repo \
    goodncref1 \
    "commits in repository without collection ID shouldn't normally contain the ostree.commit.collection metadata information"
ensure_collection_ref no-collection-repo sortofbadncref1 'org.example.Ignored' 'sortofbadncref1'

echo "ok 1 setup remote repo without collection ID"

# Create a repo with a collection ID.
mkdir collection-repo
ostree_repo_init collection-repo
do_commit collection-repo badcref1 # has no collection ref
# We set the repo collection ID in this hacky way to get the commit
# without the collection ID.
echo "collection-id=org.example.CollectionRepo" >>collection-repo/config
do_commit collection-repo badcref2 --add-metadata-string=ostree.collection-binding=org.example.Whatever
do_commit collection-repo badcref3 --add-metadata-string=ostree.collection-binding=
do_commit collection-repo goodcref1
# create a badcref4 ref with a commit that has goodcref1 in its collection ref metadata
${CMD_PREFIX} ostree --repo=collection-repo refs --create=badcref4 $(cat goodcref1-checksum)
do_summary collection-repo
ensure_no_collection_ref \
    collection-repo \
    badcref1 \
    "commit in badcref1 should not have the collection ref metadata information"
ensure_collection_ref collection-repo badcref2 'org.example.Whatever' 'badcref2'
ensure_collection_ref collection-repo badcref3 '' 'badcref3'
ensure_collection_ref collection-repo goodcref1 'org.example.CollectionRepo' 'goodcref1'

echo "ok 2 setup remote repo with collection ID"

# Create a local repo without the collection ID.
mkdir no-collection-local-repo
ostree_repo_init no-collection-local-repo
do_commit no-collection-local-repo goodnclref1
do_commit no-collection-local-repo sortofbadnclref1 --add-metadata-string=ostree.collection-binding=org.example.IgnoredLocal
do_summary no-collection-local-repo
ensure_no_collection_ref \
    no-collection-local-repo \
    goodnclref1 \
    "commits in repository without collection ID shouldn't normally contain the ostree.commit.collection metadata information"
ensure_collection_ref no-collection-local-repo sortofbadnclref1 'org.example.IgnoredLocal' 'sortofbadnclref1'

echo "ok 3 setup local repo without collection ID"

# Create a local repo with a collection ID.
mkdir collection-local-repo
ostree_repo_init collection-local-repo
do_commit collection-local-repo badclref1 # has no collection ref
# We set the repo collection ID in this hacky way to get the commit
# without the collection ID.
echo "collection-id=org.example.CollectionRepoLocal" >>collection-local-repo/config
do_commit collection-local-repo badclref2 --add-metadata-string=ostree.collection-binding=org.example.WhateverLocal
do_commit collection-local-repo badclref3 --add-metadata-string=ostree.collection-binding=
do_commit collection-local-repo goodclref1
# create a badclref4 ref with a commit that has goodclref1 in its collection ref metadata
${CMD_PREFIX} ostree --repo=collection-local-repo refs --create=badclref4 $(cat goodclref1-checksum)
do_summary collection-local-repo
ensure_no_collection_ref \
    collection-local-repo \
    badclref1 \
    "commit in badclref1 should not have the collection ref metadata information"
ensure_collection_ref collection-local-repo badclref2 'org.example.WhateverLocal' 'badclref2'
ensure_collection_ref collection-local-repo badclref3 '' 'badclref3'
ensure_collection_ref collection-local-repo goodclref1 'org.example.CollectionRepoLocal' 'goodclref1'

echo "ok 4 setup local repo with collection ID"

# Create a local repository where we pull the branches from the remotes as normal, using GPG.
mkdir local
ostree_repo_init local
do_remote_add local collection-repo --collection-id org.example.CollectionRepo
do_remote_add local no-collection-repo

do_pull local no-collection-repo goodncref1
do_pull local no-collection-repo sortofbadncref1
if do_pull local collection-repo badcref1
then
    assert_not_reached "pulling a commit without collection ID from a repo with collection ID should fail"
fi
if do_pull local collection-repo badcref2
then
    assert_not_reached "pulling a commit with a mismatched collection ID from a repo with collection ID should fail"
fi
if do_pull local collection-repo badcref3
then
    assert_not_reached "pulling a commit with empty collection ID from repo with collection ID should fail"
fi
do_pull local collection-repo goodcref1
if do_pull local collection-repo badcref4
then
    assert_not_reached "pulling a commit that was not requested from repo with collection ID should fail"
fi

echo "ok 5 pull refs from remote repos"

do_local_pull local no-collection-local-repo goodnclref1
do_local_pull local no-collection-local-repo sortofbadnclref1
if do_local_pull local collection-local-repo badclref1
then
    assert_not_reached "pulling a commit without collection ID from a repo with collection ID should fail"
fi
if do_local_pull local collection-local-repo badclref2
then
    assert_not_reached "pulling a commit with a mismatched collection ID from a repo with collection ID should fail"
fi
if do_local_pull local collection-local-repo badclref3
then
    assert_not_reached "pulling a commit with empty collection ID from repo with collection ID should fail"
fi
do_local_pull local collection-local-repo goodclref1
if do_local_pull local collection-local-repo badclref4
then
    assert_not_reached "pulling a commit that was not requested from repo with collection ID should fail"
fi

echo "ok 6 pull refs from local repos"
