#!/usr/bin/env python3
#
# Copyright (C) 2017 Colin Walters <walters@verbum.org>
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
# License along with this library. If not, see <https://www.gnu.org/licenses/>.

from __future__ import division
from __future__ import print_function
import os
import sys
import random
import shutil
import subprocess
from multiprocessing import cpu_count

def fatal(msg):
    sys.stderr.write(msg)
    sys.stderr.write('\n')
    sys.exit(1)

# Create 20 files with content based on @dname + a serial, basically to have
# different files with different checksums.
def mktree(dname, serial=0):
    print('Creating tree', dname, file=sys.stderr)
    os.mkdir(dname, 0o755)
    for v in range(20):
        with open('{}/{}'.format(dname, v), 'w') as f:
            f.write('{} {} {}\n'.format(dname, serial, v))

subprocess.check_call(['ostree', '--repo=repo', 'init', '--mode=bare'])
with open('repo/config', 'a') as f:
    # like the bit in libtest, but let's do it unconditionally since
    # it's simpler, and we don't need xattr coverage for this
    f.write('disable-xattrs=true\n')

def commit(v):
    tdir='tree{}'.format(v)
    return ['ostree', '--repo=repo', 'commit', '--fsync=0', '-b', tdir, '--tree=dir='+tdir]
def prune():
    return ['ostree', '--repo=repo', 'prune', '--refs-only']

def wait_check(proc):
    pid = proc.pid
    proc.wait()
    if proc.returncode != 0:
        sys.stderr.write("process {} exited with code {}\n".format(proc.pid, proc.returncode))
        return False
    else:
        sys.stderr.write('PID {} exited OK\n'.format(pid))
        return True

print("1..2")

def run(n_committers, n_pruners):
    # The number of committers needs to be even since we only create half as
    # many trees
    n_committers += n_committers % 2

    cmds = []

    print('n_committers', n_committers, 'n_pruners', n_pruners, file=sys.stderr)
    n_trees = n_committers // 2
    for v in range(n_trees):
        mktree('tree{}'.format(v))

    for v in range(n_committers):
        cmds.append(commit(v // 2))
    for v in range(n_pruners):
        cmds.append(prune())

    random.shuffle(cmds)
    procs = []
    for cmd in cmds:
        proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL)
        print('PID {}'.format(proc.pid), *cmd, file=sys.stderr)
        procs.append(proc)
    failed = False
    for proc in procs:
        if not wait_check(proc):
            failed = True

    if failed:
        fatal('A child process exited abnormally')

    for v in range(n_trees):
        shutil.rmtree('tree{}'.format(v))

nproc = max(cpu_count() // 2, 8)
# No concurrent pruning
run(nproc, 0)
print("ok no concurrent prunes")

run(nproc, random.randrange(2, nproc // 2, 2))
print("ok concurrent prunes")
