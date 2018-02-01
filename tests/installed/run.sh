#!/bin/bash

set -xeuo pipefail

dn=$(dirname $0)
for tn in ${dn}/itest-*.sh; do
    if [ -n "${TESTS+ }" ]; then
      tbn=$(basename "$tn" .sh)
      tbn=" ${tbn#itest-} "
      if [[ " $TESTS " != *$tbn* ]]; then
        echo "Skipping: ${tn}"
        continue
      fi
    fi
    echo Executing: ${tn}
    ${tn}
done
