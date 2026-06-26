#!/bin/bash

# Test that we didn't regress /etc/ostree/remotes.d handling

set -xeuo pipefail

. ${KOLA_EXT_DATA}/libinsttest.sh
date

prepare_tmpdir
trap _tmpdir_cleanup EXIT

ostree remote list > remotes.txt
if ! test -s remotes.txt; then
    # On container-native FCOS, there are no ostree remotes — that's expected.
    # On traditional ostree systems, missing remotes is a real regression.
    if [ "$(rpmostree_query_json '.deployments[0]."container-image-reference" // empty')" != "" ]; then
        echo "No ostree remotes found; skipping test on container-native system"
        exit 0
    fi
    assert_not_reached "no ostree remotes"
fi
date
