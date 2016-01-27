#!/bin/bash
#
# Copyright (C) 2014 Owen Taylor <otaylor@redhat.com>
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

set -euo pipefail

. $(dirname $0)/libtest.sh

echo "1..1"

echo "Testing:" 1>&2
test_recursive() {
    local cmd=$1
    local root=$2

    echo "$cmd" 1>&2
    $cmd --help 1>out 2>err
    # --help message goes to standard output
    if [ "$root" = "1" ] ; then
        assert_file_has_content out "[Uu]sage"
        assert_file_has_content out "$cmd"
    fi
    assert_file_empty err
    builtins=`sed -n '/^Builtin commands/,/^[^ ]/p' <out | tail -n +2`
    if [ "$builtins" != "" ] ; then
        # A command with subcommands
        # Running the command without a subcommand should produce the help output, but fail
        set +e
        $cmd 1>out 2>err
        if [ $? = 0 ] ; then
	    echo 1>&2 "missing subcommand but 0 exit status"; exit 1
        fi
        set -euo pipefail
        # error message and usage goes to standard error
        assert_file_has_content err "[Uu]sage"
        assert_file_has_content err "$cmd"
        assert_file_has_content err "Builtin commands"
        assert_file_empty out

        for subcmd in $builtins ; do
            test_recursive "$cmd $subcmd" 0
        done
    fi
}

test_recursive ostree 1

echo "ok help option is properly supported"
