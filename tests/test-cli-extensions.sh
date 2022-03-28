#!/usr/bin/env bash
#
# Copyright (C) 2021 Red Hat Inc.
# SPDX-License-Identifier: LGPL-2.0+

set -euo pipefail

. $(dirname $0)/libtest.sh

echo '1..2'

# Test CLI extensions via $PATH.  If you change this, you may
# also want to change the corresponding destructive version in
# tests/kolainst/destructive/basic-misc.sh
mkdir -p ./localbin
ORIG_PATH="${PATH}"
export PATH="./localbin/:${PATH}"
echo '#!/bin/sh' >> ./localbin/ostree-env
echo 'env "$@"' >> ./localbin/ostree-env
chmod +x ./localbin/ostree-env
export A_CUSTOM_TEST_FLAG="myvalue"
${CMD_PREFIX} ostree env >out.txt
assert_file_has_content out.txt "^A_CUSTOM_TEST_FLAG=myvalue"
PATH="${ORIG_PATH}"
export -n A_CUSTOM_TEST_FLAG
rm -rf -- localbin

echo 'ok CLI extension localbin ostree-env'

if ${CMD_PREFIX} ostree nosuchcommand 2>err.txt; then
    assert_not_reached "missing CLI extension ostree-nosuchcommand succeeded"
fi
assert_file_has_content err.txt "Unknown command 'nosuchcommand'"
rm -f -- err.txt

echo 'ok CLI extension unknown ostree-nosuchcommand'
