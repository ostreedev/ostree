#!/usr/bin/env python
#
# Copyright (C) 2017 Dan Nicholson <nicholson@endlessm.com>
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

from __future__ import print_function
from contextlib import contextmanager
import os
import multiprocessing
import subprocess
import sys
import time
import yaml

try:
    import gi
except ImportError:
    print('1..0 # SKIP no python gi module')
    sys.exit(0)

gi.require_version('OSTree', '1.0')
from gi.repository import Gio, GLib, OSTree

# See if the experimental API support is included
proc = subprocess.Popen(['ostree', '--version'], stdout=subprocess.PIPE)
data = yaml.safe_load(proc.stdout)
proc.communicate()
if proc.returncode != 0:
    raise subprocess.CalledProcessError('ostree --version failed')
if 'experimental' not in data['libostree']['Features']:
    print('1..0 # SKIP no experimental API')
    sys.exit(0)

@contextmanager
def repo_lock(repo, exclusive, cancellable=None):
    """Ensure lock popped after action"""
    repo.lock_push(exclusive, cancellable)
    try:
        yield
    finally:
        repo.lock_pop(cancellable)

print('1..11')

subprocess.check_call(['ostree', '--repo=repo', 'init', '--mode=bare'])
# like the bit in libtest, but let's do it unconditionally since it's simpler,
# and we don't need xattr coverage for this
subprocess.check_call(['ostree', '--repo=repo', 'config', 'set',
                       'core.disable-xattrs', 'true'])

# Set the default lock timeout to 0 seconds to speed up the tests
subprocess.check_call(['ostree', '--repo=repo', 'config', 'set',
                       'core.lock-timeout', '0'])

# Open 2 repo objects to operate on concurrently
repo_file = Gio.File.new_for_path('repo')
repo1 = OSTree.Repo.new(repo_file)
repo1.open()
repo2 = OSTree.Repo.new(repo_file)
repo2.open()

# Recursive locking
with repo_lock(repo1, False), repo_lock(repo1, False):
    pass
print('ok recursive shared lock')

with repo_lock(repo1, True), repo_lock(repo1, True):
    pass
print('ok recursive exclusive lock')

with repo_lock(repo1, False), repo_lock(repo1, True):
    pass
print('ok recursive upgrade lock')

with repo_lock(repo1, True), repo_lock(repo1, False):
    pass
print('ok recursive non-upgrade lock')

# Both repos can get shared locks
with repo_lock(repo1, False), repo_lock(repo2, False):
    pass
print('ok concurrent shared locks')

# Both repos cannot get exclusive locks
with repo_lock(repo1, True):
    try:
        with repo_lock(repo2, True):
            pass
        raise Exception('Both repos took exclusive locks')
    except GLib.Error as err:
        if not err.matches(Gio.io_error_quark(),
                           Gio.IOErrorEnum.WOULD_BLOCK):
            raise
print('ok concurrent exclusive locks fails')

# Repo cannot get exclusive lock while other is shared and vice-versa
with repo_lock(repo1, True):
    try:
        with repo_lock(repo2, False):
            pass
        raise Exception('repo2 got shared lock when repo1 exclusive')
    except GLib.Error as err:
        if not err.matches(Gio.io_error_quark(),
                           Gio.IOErrorEnum.WOULD_BLOCK):
            raise
print('ok concurrent shared while exclusive fails')

with repo_lock(repo1, False):
    try:
        with repo_lock(repo2, True):
            pass
        raise Exception('repo2 got exclusive lock when repo1 shared')
    except GLib.Error as err:
        if not err.matches(Gio.io_error_quark(),
                           Gio.IOErrorEnum.WOULD_BLOCK):
            raise
print('ok concurrent exclusive while shared fails')

# Make sure recursive lock doesn't downgrade
with repo_lock(repo1, True), repo_lock(repo1, False):
    try:
        with repo_lock(repo2, False):
            pass
        raise Exception('repo2 got shared lock when repo1 exclusive')
    except GLib.Error as err:
        if not err.matches(Gio.io_error_quark(),
                           Gio.IOErrorEnum.WOULD_BLOCK):
            raise
print('ok recursive lock does not downgrade')

# Make sure recursive lock downgrades when unwinding
with repo_lock(repo1, False):
    with repo_lock(repo1, True):
        pass
    with repo_lock(repo2, False):
        pass
print('ok recursive lock downgraded when popped')

# Set the default lock timeout to 3 seconds to test the lock timeout
# behavior
subprocess.check_call(['ostree', '--repo=repo', 'config', 'set',
                       'core.lock-timeout', '3'])
repo1.reload_config()
repo2.reload_config()

def repo_lock_sleep(exclusive=True, sleep=0, cancellable=None):
    repo = OSTree.Repo.new(repo_file)
    repo.open()
    with repo_lock(repo, exclusive, cancellable):
        if sleep > 0:
            time.sleep(sleep)

proc1 = multiprocessing.Process(target=repo_lock_sleep, kwargs={'sleep': 1})
proc2 = multiprocessing.Process(target=repo_lock_sleep)
proc1.start()
proc2.start()
proc1.join()
proc2.join()
assert proc1.exitcode == 0
assert proc2.exitcode == 0
print('ok concurrent exclusive locks wait')
