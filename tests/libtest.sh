# Source library for shell script tests
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

SRCDIR=$(dirname $0)
test_tmpdir=$(pwd)

export G_DEBUG=fatal-warnings

if test -n "${OT_TESTS_DEBUG}"; then
    set -x
fi

if test -n "$OT_TESTS_VALGRIND"; then
    CMD_PREFIX="env G_SLICE=always-malloc valgrind -q --leak-check=full --num-callers=30 --suppressions=${SRCDIR}/ostree-valgrind.supp"
fi

assert_streq () {
    test "$1" = "$2" || (echo 1>&2 "$1 != $2"; exit 1)
}

assert_has_file () {
    test -f "$1" || (echo 1>&2 "Couldn't find '$1'"; exit 1)
}

assert_not_has_file () {
    if test -f "$1"; then
	echo 1>&2 "File '$1' exists"; exit 1
    fi
}

assert_not_file_has_content () {
    if grep -q -e "$2" "$1"; then
	echo 1>&2 "File '$1' incorrectly matches regexp '$2'"; exit 1
    fi
}

assert_file_has_content () {
    if ! grep -q -e "$2" "$1"; then
	echo 1>&2 "File '$1' doesn't match regexp '$2'"; exit 1
    fi
}

setup_test_repository () {
    mode=$1
    shift

    oldpwd=`pwd`

    cd ${test_tmpdir}
    mkdir repo
    cd repo
    ot_repo="--repo=`pwd`"
    export OSTREE="${CMD_PREFIX} ostree ${ot_repo}"
    if test -n "$mode"; then
	$OSTREE init --mode=${mode}
    else
	$OSTREE init
    fi

    cd ${test_tmpdir}
    mkdir files
    cd files
    ot_files=`pwd`
    export ht_files
    ln -s nosuchfile somelink
    echo first > firstfile

    cd ${test_tmpdir}/files
    $OSTREE commit -b test2 -s "Test Commit 1" -m "Commit body first"

    mkdir baz
    mkfifo baz/afifo  # named pipe
    echo moo > baz/cow
    echo alien > baz/saucer
    mkdir baz/deeper
    echo hi > baz/deeper/ohyeah
    ln -s nonexistent baz/alink
    mkdir baz/another/
    echo x > baz/another/y

    cd ${test_tmpdir}/files
    $OSTREE commit -b test2 -s "Test Commit 2" -m "Commit body second"
    $OSTREE fsck -q

    cd $oldpwd
}

setup_fake_remote_repo1() {
    if ! test -x ${SRCDIR}/run-apache; then
	exit 77
    fi
    mode=$1
    shift
    oldpwd=`pwd`
    mkdir ostree-srv
    cd ostree-srv
    mkdir gnomerepo
    ${CMD_PREFIX} ostree --repo=gnomerepo init --mode=$mode
    mkdir gnomerepo-files
    cd gnomerepo-files 
    echo first > firstfile
    mkdir baz
    echo moo > baz/cow
    echo alien > baz/saucer
    ${CMD_PREFIX} ostree  --repo=${test_tmpdir}/ostree-srv/gnomerepo commit -b main -s "A remote commit" -m "Some Commit body"
    mkdir baz/deeper
    ${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo commit -b main -s "Add deeper"
    echo hi > baz/deeper/ohyeah
    mkdir baz/another/
    echo x > baz/another/y
    ${CMD_PREFIX} ostree --repo=${test_tmpdir}/ostree-srv/gnomerepo commit -b main -s "The rest"
    cd ..
    rm -rf gnomerepo-files
    
    cd ${test_tmpdir}
    mkdir ${test_tmpdir}/httpd
    cd httpd
    cat >httpd.conf <<EOF
ServerRoot ${test_tmpdir}/httpd
PidFile pid
LogLevel crit
ErrorLog log
LockFile lock
ServerName localhost

LoadModule alias_module modules/mod_alias.so
LoadModule cgi_module modules/mod_cgi.so
LoadModule env_module modules/mod_env.so

StartServers 1

# SetEnv OSTREE_REPO_PREFIX ${test_tmpdir}/ostree-srv
Alias /ostree/ ${test_tmpdir}/ostree-srv/
# ScriptAlias /ostree/  ${test_tmpdir}/httpd/ostree-http-backend/
EOF
    ${SRCDIR}/tmpdir-lifecycle ${SRCDIR}/run-apache `pwd`/httpd.conf ${test_tmpdir}/httpd-address &
    for i in $(seq 5); do
	if ! test -f ${test_tmpdir}/httpd-address; then
	    sleep 1
	else
	    break
	fi
    done
    if ! test -f ${test_tmpdir}/httpd-address; then
	echo "Error: timed out waiting for httpd-address file"
	exit 1
    fi
    cd ${oldpwd} 

    export OSTREE="ostree --repo=repo"
}
