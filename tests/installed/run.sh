#!/bin/bash

set -xeuo pipefail

dn=$(dirname $0)
for tn in ${dn}/test-*.sh; do
    echo Executing: ${tn}
    ${tn}
done
