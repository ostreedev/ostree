#!/bin/bash
set -euo pipefail
dn=$(dirname $0)
n=0
for case in ${dn}/test-*; do
  echo "Running: $case"
  $case
  echo "ok $case"
  n=$(($n+1))
done
echo "Executed tests: $n"
exit 0

