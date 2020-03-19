#!/bin/bash

# Test that we didn't regress /etc/ostree/remotes.d handling

set -xeuo pipefail

. ${KOLA_EXT_DATA}/libinsttest.sh
date

prepare_tmpdir
trap _tmpdir_cleanup EXIT

ostree remote list > remotes.txt
if ! test -s remotes.txt; then
    assert_not_reached "no ostree remotes"
fi
date
