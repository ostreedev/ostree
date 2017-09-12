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

test_usage_output() {
    file=$1; shift
    cmd=$1; shift
    assert_file_has_content $file '^Usage'
    # check that it didn't print twice somehow
    if [ "$(grep --count '^Usage' $file)" != 1 ]; then
      _fatal_print_file "$file" "File '$file' has '^Usage' more than once."
    fi
    assert_file_has_content $file "$cmd"
}

# check that we found at least one command with subcommands
found_subcommands=0

test_recursive() {
    local cmd=$1

    echo "$cmd" 1>&2
    $cmd --help 1>out 2>err
    # --help message goes to standard output
    test_usage_output out "$cmd"
    assert_file_empty err

    builtins=`sed -n '/^Builtin \("[^"]*" \)\?Commands:$/,/^$/p' <out | tail -n +2`
    if [ "$builtins" != "" ] ; then

        found_subcommands=1

        # A command with subcommands
        # Running the command without a subcommand should produce the help output, but fail
        rc=0
        $cmd 1>out 2>err || rc=$?
        if [ $rc = 0 ] ; then
            assert_not_reached "missing subcommand but 0 exit status"
        fi

        # error message and usage goes to standard error
        test_usage_output err "$cmd"
        assert_file_has_content err 'No \("[^"]*" sub\)\?command specified'
        assert_file_empty out

        rc=0
        $cmd non-existent-subcommand 1>out 2>err || rc=$?
        if [ $rc = 0 ] ; then
            assert_not_reached "non-existent subcommand but 0 exit status"
        fi

        test_usage_output err "$cmd"
        assert_file_has_content err 'Unknown \("[^"]*" sub\)\?command'
        assert_file_empty out

        for subcmd in $builtins; do
            test_recursive "$cmd $subcmd"
        done
    fi
}

test_recursive ostree
if [ $found_subcommands != 1 ]; then
  assert_not_reached "no ostree commands with subcommands found!"
fi

echo "ok help option is properly supported"
