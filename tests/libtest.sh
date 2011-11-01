# Source library for shell script tests
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
#
# Author: Colin Walters <walters@verbum.org>

cd `dirname $0`
SRCDIR=`pwd`
cd -
TMPDIR=${TMPDIR:-/tmp}
export TMPDIR
test_tmpdir=`mktemp -d "$TMPDIR/ostree-tests.XXXXXXXXXX"`
cd "$test_tmpdir"
touch "$test_tmpdir/.test$$"

die () {
    if test -z "$OT_TESTS_SAVE_TEMPS"; then
        test -f "$test_tmpdir/.test$$" && rm -rf "$test_tmpdir"
    else
        echo "Temporary files saved in $test_tmpdir"
    fi
}

assert_has_file () {
    test -f "$1" || (echo "Couldn't find '$1'"; exit 1)
}

assert_not_has_file () {
    if test -f "$1"; then
	echo "File '$1' exists"; exit 1
    fi
}

assert_file_has_content () {
    if ! grep -q "$2" "$1"; then
	echo "File '$1' doesn't match regexp '$2'"; exit 1
    fi
}

setup_test_repository1 () {
    mkdir files
    cd files
    ht_files=`pwd`
    export ht_files
    echo first > firstfile
    echo second > secondfile

    mkdir ../repo
    ot_repo="--repo=../repo"
    export ot_repo
    ostree init $ot_repo
    ostree commit $ot_repo -b test -s "Test Commit 1" -m "Commit body first" --add=firstfile
    ostree commit $ot_repo -b test -s "Test Commit 2" -m "Commit body second" --add=secondfile
    ostree fsck -q $ot_repo
}

setup_test_repository2 () {
    mkdir files
    cd files
    ht_files=`pwd`
    export ht_files
    echo first > firstfile
    mkdir baz
    echo moo > baz/cow
    echo alien > baz/saucer
    mkdir baz/deeper
    echo hi > baz/deeper/ohyeah
    mkdir baz/another/
    echo x > baz/another/y

    cd ..
    mkdir repo
    cd repo
    ot_repo="--repo=`pwd`"
    cd ../files
    export ot_repo
    ostree init $ot_repo
    ostree commit $ot_repo -b test2 -s "Test Commit 1" -m "Commit body first" --add=firstfile
    ostree commit $ot_repo -b test2 -s "Test Commit 2" -m "Commit body second" --add=baz/cow  --add=baz/saucer --add=baz/deeper/ohyeah --add=baz/another/y
    ostree fsck -q $ot_repo
}

setup_fake_remote_repo1() {
    oldpwd=`pwd`
    mkdir ostree-srv
    cd ostree-srv
    mkdir gnomerepo
    ostree init --archive --repo=gnomerepo
    mkdir gnomerepo-files
    cd gnomerepo-files 
    echo first > firstfile
    mkdir baz
    echo moo > baz/cow
    echo alien > baz/saucer
    find | grep -v '^\.$' | ostree commit --repo=${test_tmpdir}/ostree-srv/gnomerepo -b main -s "A remote commit" -m "Some Commit body" --from-stdin
    mkdir baz/deeper
    ostree commit --repo=${test_tmpdir}/ostree-srv/gnomerepo -b main -s "Add deeper" --add=baz/deeper
    echo hi > baz/deeper/ohyeah
    mkdir baz/another/
    echo x > baz/another/y
    find | grep -v '^\.$' | ostree commit --repo=${test_tmpdir}/ostree-srv/gnomerepo -b main -s "The rest" --from-stdin
    cd ..
    rm -rf gnomerepo-files
    
    cd ${test_tmpdir}
    mkdir ${test_tmpdir}/httpd
    cd httpd
    cp $(command -v ostree-http-backend) .
    chmod a+x ostree-http-backend
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
    ${SRCDIR}/tmpdir-lifecycle ${SRCDIR}/run-apache `pwd`/httpd.conf ${test_tmpdir}/httpd-address
    cd ${oldpwd} 
}

trap 'die' EXIT
