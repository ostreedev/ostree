# Common definitions for installed, privileged tests
#
# Copyright (C) 2017 Colin Walters <walters@verbum.org>
#
# SPDX-License-Identifier: LGPL-2.0+

dn=$(dirname $0)
. ${dn}/libtest-core.sh

# Copy of bits from tap-test
test_tmpdir=
function _tmpdir_cleanup () {
    if test -n "${test_tmpdir}" && test -f ${test_tmpdir}/.testtmp; then
	      rm "${test_tmpdir}" -rf
    fi
}
prepare_tmpdir() {
    test_tmpdir=$(mktemp -d)
    touch ${test_tmpdir}/.testtmp
    cd ${test_tmpdir}
    echo ${test_tmpdir}
}

# This is copied from flatpak/flatpak/tests/test-webserver.sh
run_tmp_webserver() {
    dir=$1

    test -n ${test_tmpdir}

    cd ${dir}
    env PYTHONUNBUFFERED=1 setsid python -m SimpleHTTPServer 0 &>${test_tmpdir}/httpd-output &
    cd -
    child_pid=$!

    for x in $(seq 50); do
        # Snapshot the output
        cp ${test_tmpdir}/httpd-output{,.tmp}
        # If it's non-empty, see whether it matches our regexp
        if test -s ${test_tmpdir}/httpd-output.tmp; then
            sed -e 's,Serving HTTP on 0.0.0.0 port \([0-9]*\) \.\.\.,\1,' < ${test_tmpdir}/httpd-output.tmp > ${test_tmpdir}/httpd-port
            if ! cmp ${test_tmpdir}/httpd-output.tmp ${test_tmpdir}/httpd-port 1>/dev/null; then
                # If so, we've successfully extracted the port
                break
            fi
        fi
        sleep 0.1
    done
    port=$(cat ${test_tmpdir}/httpd-port)
    echo "http://127.0.0.1:${port}" > ${test_tmpdir}/httpd-address
    echo "$child_pid" > ${test_tmpdir}/httpd-pid
}

# Determine our origin refspec - we'll use this as a test base
rpmostree=$(which rpm-ostree 2>/dev/null)
if test -z "${rpmostree}"; then
    skip "no rpm-ostree, at some point point this to raw ostree too"
fi

# We need to be root
assert_streq $(id -u) 0

PYTHON=
for py in /usr/bin/python3 /usr/bin/python; do
    if ! test -x ${py}; then continue; fi
    export PYTHON=${py}
    break
done
if test -z "${PYTHON}"; then
    fatal "no python found"
fi

rpmostree_query_json() {
    query=$1
    rpm-ostree status --json | $PYTHON -c 'import json,sys; v=json.load(sys.stdin); print(v'${query}')'
}
host_refspec=$(rpmostree_query_json '["deployments"][0]["origin"]')
host_commit=$(rpmostree_query_json '["deployments"][0]["checksum"]')
host_osname=$(rpmostree_query_json '["deployments"][0]["osname"]')
