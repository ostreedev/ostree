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
from . import buildutil
from .ostbuildlog import log, fatal

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
        run_sync(['git', 'remote', 'add', 'upstream', uri], cwd=tmp_dest)
    else:
        run_sync(['git', 'fetch', 'localmirror'], cwd=tmp_dest)
    run_sync(['git', 'checkout', '-q', branch], cwd=tmp_dest)
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

def parse_src_key(srckey):
    idx = srckey.find(':')
    if idx < 0:
        raise ValueError("Invalid SRC uri=%s" % (srckey, ))
    keytype = srckey[:idx]
    if keytype not in ['git', 'local']:
        raise ValueError("Unsupported SRC uri=%s" % (srckey, ))
    uri = srckey[idx+1:]
    return (keytype, uri)

def get_lastfetch_path(mirrordir, keytype, uri, branch):
    mirror = buildutil.get_mirrordir(mirrordir, keytype, uri)
    branch_safename = branch.replace('/','_').replace('.', '_')
    return mirror + '.lastfetch-%s' % (branch_safename, )

def ensure_vcs_mirror(mirrordir, keytype, uri, branch):
    mirror = buildutil.get_mirrordir(mirrordir, keytype, uri)
    tmp_mirror = mirror + '.tmp'
    if os.path.isdir(tmp_mirror):
        shutil.rmtree(tmp_mirror)
    if not os.path.isdir(mirror):
        run_sync(['git', 'clone', '--mirror', uri, tmp_mirror])
        run_sync(['git', 'config', 'gc.auto', '0'], cwd=tmp_mirror)
        os.rename(tmp_mirror, mirror)
    if branch is None:
        return mirror
    last_fetch_path = get_lastfetch_path(mirrordir, keytype, uri, branch)
    if os.path.exists(last_fetch_path):
        f = open(last_fetch_path)
        last_fetch_contents = f.read()
        f.close()
        last_fetch_contents = last_fetch_contents.strip()
    else:
        last_fetch_contents = None
    current_vcs_version = run_sync_get_output(['git', 'rev-parse', branch], cwd=mirror)
    current_vcs_version = current_vcs_version.strip()
    if current_vcs_version != last_fetch_contents:
        log("last fetch %r differs from branch %r" % (last_fetch_contents, current_vcs_version))
        tmp_checkout = buildutil.get_mirrordir(mirrordir, keytype, uri, prefix='_tmp-checkouts')
        if os.path.isdir(tmp_checkout):
            shutil.rmtree(tmp_checkout)
        parent = os.path.dirname(tmp_checkout)
        if not os.path.isdir(parent):
            os.makedirs(parent)
        run_sync(['git', 'clone', '-q', '--no-checkout', mirror, tmp_checkout])
        run_sync(['git', 'checkout', '-q', '-f', current_vcs_version], cwd=tmp_checkout)
        submodules = []
        submodules_status_text = run_sync_get_output(['git', 'submodule', 'status'], cwd=tmp_checkout)
        submodule_status_lines = submodules_status_text.split('\n')
        for line in submodule_status_lines:
            if line == '': continue
            line = line[1:]
            (sub_checksum, sub_name) = line.split(' ', 1)
            sub_url = run_sync_get_output(['git', 'config', '-f', '.gitmodules',
                                           'submodule.%s.url' % (sub_name, )], cwd=tmp_checkout)
            ensure_vcs_mirror(mirrordir, keytype, sub_url, sub_checksum)
        shutil.rmtree(tmp_checkout)
        f = open(last_fetch_path, 'w')
        f.write(current_vcs_version + '\n')
        f.close()
    return mirror

def fetch(mirrordir, keytype, uri, branch, keep_going=False):
    mirror = buildutil.get_mirrordir(mirrordir, keytype, uri)
    last_fetch_path = get_lastfetch_path(mirrordir, keytype, uri, branch)
    run_sync(['git', 'fetch'], cwd=mirror, log_initiation=False,
             fatal_on_error=not keep_going) 
    current_vcs_version = run_sync_get_output(['git', 'rev-parse', branch], cwd=mirror)
    if current_vcs_version is not None:
        current_vcs_version = current_vcs_version.strip()
        f = open(last_fetch_path, 'w')
        f.write(current_vcs_version + '\n')
        f.close()
    
