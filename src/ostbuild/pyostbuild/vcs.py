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

import os
import re
import urlparse
import shutil

from .subprocess_helpers import run_sync_get_output, run_sync

def get_mirrordir(mirrordir, keytype, uri, prefix=''):
    assert keytype == 'git'
    parsed = urlparse.urlsplit(uri)
    return os.path.join(mirrordir, prefix, keytype, parsed.scheme, parsed.netloc, parsed.path[1:])

def _fixup_submodule_references(mirrordir, cwd):
    submodules_status_text = run_sync_get_output(['git', 'submodule', 'status'], cwd=cwd)
    submodule_status_lines = submodules_status_text.split('\n')
    have_submodules = False
    for line in submodule_status_lines:
        if line == '': continue
        have_submodules = True
        line = line[1:]
        (sub_checksum, sub_name) = line.split(' ', 1)
        sub_url = run_sync_get_output(['git', 'config', '-f', '.gitmodules',
                                       'submodule.%s.url' % (sub_name, )], cwd=cwd)
        mirrordir = get_mirrordir(mirrordir, 'git', sub_url)
        run_sync(['git', 'config', 'submodule.%s.url' % (sub_name, ), 'file://' + mirrordir], cwd=cwd)
    return have_submodules

def get_vcs_checkout(mirrordir, keytype, uri, dest, branch, overwrite=True):
    module_mirror = get_mirrordir(mirrordir, keytype, uri)
    assert keytype == 'git'
    checkoutdir_parent=os.path.dirname(dest)
    if not os.path.isdir(checkoutdir_parent):
        os.makedirs(checkoutdir_parent)
    tmp_dest = dest + '.tmp'
    if os.path.isdir(tmp_dest):
        shutil.rmtree(tmp_dest)
    if os.path.islink(dest):
        os.unlink(dest)
    if os.path.isdir(dest):
        if overwrite:
            shutil.rmtree(dest)
        else:
            tmp_dest = dest
    if not os.path.isdir(tmp_dest):
        run_sync(['git', 'clone', '-q', '--origin', 'localmirror',
                  '--no-checkout', module_mirror, tmp_dest])
    else:
        run_sync(['git', 'fetch'], cwd=tmp_dest)
    run_sync(['git', 'checkout', '-q', branch], cwd=tmp_dest)
    run_sync(['git', 'remote', 'add', 'upstream', uri], cwd=tmp_dest)
    run_sync(['git', 'submodule', 'init'], cwd=tmp_dest)
    have_submodules = _fixup_submodule_references(mirrordir, tmp_dest)
    if have_submodules:
        run_sync(['git', 'submodule', 'update'], cwd=tmp_dest)
    if tmp_dest != dest:
        os.rename(tmp_dest, dest)
    return dest

def clean(keytype, checkoutdir):
    assert keytype in ('git', 'dirty-git')
    run_sync(['git', 'clean', '-d', '-f', '-x'], cwd=checkoutdir)
