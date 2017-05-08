#!/bin/bash

set -xeuo pipefail

dn=$(dirname $0)
for tn in ${dn}/itest-*.sh; do
    echo Executing: ${tn}
    ${tn}
done
