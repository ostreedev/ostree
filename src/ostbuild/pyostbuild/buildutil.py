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

from .subprocess_helpers import run_sync_get_output

BUILD_ENV = {
    'HOME' : '/', 
    'HOSTNAME' : 'ostbuild',
    'LANG': 'C',
    'PATH' : '/usr/bin:/bin:/usr/sbin:/sbin',
    'SHELL' : '/bin/bash',
    'TERM' : 'vt100',
    'TMPDIR' : '/tmp',
    'TZ': 'EST5EDT'
    }

def get_mirrordir(mirrordir, keytype, uri, prefix=''):
    assert keytype == 'git'
    parsed = urlparse.urlsplit(uri)
    return os.path.join(mirrordir, prefix, keytype, parsed.scheme, parsed.netloc, parsed.path[1:])

def find_user_chroot_path():
    # We need to search PATH here manually so we correctly pick up an
    # ostree install in e.g. ~/bin even though we're going to set PATH
    # below for our children inside the chroot.
    ostbuild_user_chroot_path = None
    for dirname in os.environ['PATH'].split(':'):
        path = os.path.join(dirname, 'linux-user-chroot')
        if os.access(path, os.X_OK):
            ostbuild_user_chroot_path = path
            break
    if ostbuild_user_chroot_path is None:
        ostbuild_user_chroot_path = 'linux-user-chroot'
    return ostbuild_user_chroot_path

def branch_name_for_artifact(a):
    return 'artifacts/%s/%s/%s' % (a['buildroot'],
                                   a['name'],
                                   a['branch'])

def get_git_version_describe(dirpath, commit=None):
    args = ['git', 'describe', '--long', '--abbrev=42', '--always']
    if commit is not None:
        args.append(commit)
    version = run_sync_get_output(args, cwd=dirpath)
    return version.strip()
