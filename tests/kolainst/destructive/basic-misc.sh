#!/bin/bash

# Random misc tests

set -xeuo pipefail

. ${KOLA_EXT_DATA}/libinsttest.sh

echo "1..1"
date

# Test CLI extensions installed alongside the system
extdir=/usr/libexec/libostree/ext/
mkdir -p "${extdir}"
ln -sr /usr/bin/env ${extdir}/ostree-env

env TESTENV=foo ostree env > out.txt
assert_file_has_content out.text TESTENV=foo
rm -vf "${extdir}/ostree-env"
echo "ok env"

# End test
date
