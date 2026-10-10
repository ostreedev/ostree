#!/usr/bin/env bash
#
# Copyright (C) 2021 Red Hat Inc.
# SPDX-License-Identifier: LGPL-2.0+

set -euo pipefail

. $(dirname $0)/libtest.sh

echo '1..3'

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

# Test that the subcommand verb is not passed to the external command.
mkdir -p ./localbin
ORIG_PATH="${PATH}"
export PATH="./localbin/:${PATH}"
echo '#!/bin/sh' >> ./localbin/ostree-echoargs
echo 'echo "argc=$#" ; for a in "$@"; do echo "arg=$a"; done' >> ./localbin/ostree-echoargs
chmod +x ./localbin/ostree-echoargs
${CMD_PREFIX} ostree echoargs foo bar >out.txt
assert_file_has_content out.txt "^argc=2"
assert_file_has_content out.txt "^arg=foo"
assert_file_has_content out.txt "^arg=bar"
assert_not_file_has_content out.txt "arg=echoargs"
PATH="${ORIG_PATH}"
rm -rf -- localbin

echo 'ok CLI extension does not pass verb to external command'

if ${CMD_PREFIX} ostree nosuchcommand 2>err.txt; then
    assert_not_reached "missing CLI extension ostree-nosuchcommand succeeded"
fi
assert_file_has_content err.txt "Unknown command 'nosuchcommand'"
rm -f -- err.txt

echo 'ok CLI extension unknown ostree-nosuchcommand'
