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
import tempfile

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

def parse_src_key(srckey):
    idx = srckey.find(':')
    if idx < 0:
        raise ValueError("Invalid SRC uri=%s" % (srckey, ))
    keytype = srckey[:idx]
    if keytype not in ['git']:
        raise ValueError("Unsupported SRC uri=%s" % (srckey, ))
    uri = srckey[idx+1:]
    return (keytype, uri)


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

def manifest_target(manifest, architecture):
    return '%s-%s-devel' % (manifest['name'], architecture)

def manifest_base(manifest, roottype, architecture):
    return 'bases/%s-%s-%s' % (manifest['base'],
                               architecture, roottype)

def manifest_buildname(manifest, component, architecture):
    return 'artifacts/%s/%s/%s' % (manifest_target (manifest, architecture),
                                   component['name'],
                                   component['branch'])

def manifest_buildroot_name(manifest, component, architecture):
    return 'buildroots/%s/%s/%s' % (manifest_target (manifest, architecture),
                                    component['name'],
                                    component['branch'])

def find_component_in_manifest(manifest, component_name):
    for component in manifest['components']:
        if component['name'] == component_name:
            return component
    return None

def compose(repo, target, artifacts):
    child_args = ['ostree', '--repo=' + repo, 'compose',
                  '-b', target, '-s', 'Compose']
    (fd, path) = tempfile.mkstemp(suffix='.txt', prefix='ostbuild-compose-')
    f = os.fdopen(fd, 'w')
    for artifact in artifacts:
        f.write(artifact)
        f.write('\n')
    f.close()
    child_args.extend(['-F', path])
    revision = run_sync_get_output(child_args, log_initiation=True).strip()
    os.unlink(path)
    return revision

def compose_buildroot(manifest, repo, buildroot_name, component, dependencies, architecture):
    base = manifest_base(manifest, 'devel', architecture)
    buildroot_contents = [base + ':/']
    for dep in dependencies:
        dep_buildname = manifest_buildname(manifest, dep, architecture)
        buildroot_contents.append(dep_buildname + ':/runtime')
        buildroot_contents.append(dep_buildname + ':/devel')

    return compose(repo, buildroot_name, buildroot_contents)
