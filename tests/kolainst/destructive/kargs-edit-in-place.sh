#!/bin/bash

# Verify "ostree admin kargs edit-in-place" works

set -xeuo pipefail

. ${KOLA_EXT_DATA}/libinsttest.sh

sudo ostree admin kargs edit-in-place --append-if-missing=testarg
assert_file_has_content /boot/loader/entries/ostree-* testarg

echo "ok test `kargs edit-in-place --append-if-missing`"
