#!/usr/bin/env bash

set -euo pipefail

if ! spatch --version 2>/dev/null; then
    skip "no spatch; get it from http://coccinelle.lip6.fr/"
fi

if test -z "${OSTREE_UNINSTALLED_SRCDIR:-}"; then
    skip "running installed?"
fi

. $(dirname $0)/libtest.sh

coccitests=$(ls $(dirname $0)/coccinelle/*.cocci)
echo "1.."$(echo ${coccitests} | wc -l)

for cocci in $(dirname $0)/coccinelle/*.cocci; do
    echo "Running: ${cocci}"
    spatch --very-quiet --dir ${OSTREE_UNINSTALLED_SRCDIR} ${cocci} > cocci.out
    if test -s cocci.out; then
        sed -e 's/^/# /' < cocci.out >&2
        fatal "Failed semantic patch: ${cocci}"
    fi
    echo ok ${cocci}
done
