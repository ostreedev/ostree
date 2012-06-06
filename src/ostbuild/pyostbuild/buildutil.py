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
import StringIO

from . import ostbuildrc
from .ostbuildlog import log, fatal
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
    if keytype not in ['git', 'local']:
        raise ValueError("Unsupported SRC uri=%s" % (srckey, ))
    uri = srckey[idx+1:]
    return (keytype, uri)


def get_mirrordir(mirrordir, keytype, uri, prefix=''):
    if keytype != 'git':
        fatal("Unhandled keytype '%s' for uri '%s'" % (keytype, uri))
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

def ref_to_unix_name(ref):
    return ref.replace('/', '.')

def tsort_components(components, key):
    (fd, path) = tempfile.mkstemp(suffix='.txt', prefix='ostbuild-tsort-')
    f = os.fdopen(fd, 'w')
    for name,component in components.iteritems():
        build_prev = component.get(key)
        if (build_prev is not None and len(build_prev) > 0):
            for dep_name in build_prev:
                f.write('%s %s\n' % (name, dep_name))
    f.close()
    
    output = run_sync_get_output(['tsort', path])
    os.unlink(path)
    output_stream = StringIO.StringIO(output)
    result = []
    for line in output_stream:
        result.append(line.strip())
    return result

def _recurse_depends(depkey, component_name, components, dep_names):
    component = components[component_name]
    depends = component.get(depkey)
    if (depends is None or len(depends) == 0):
        return
    for depname in depends:
        dep_names.add(depname)
        _recurse_depends(depkey, depname, components, dep_names)

def _sorted_depends(deptype, component_name, components):
    dep_names = set()
    _recurse_depends(deptype, component_name, components, dep_names)
    dep_components = {}
    for component_name in dep_names:
        dep_components[component_name] = components[component_name]
    result = tsort_components(dep_components, deptype)
    result.reverse()
    return result
    
def build_depends(component_name, components):
    return _sorted_depends('build-depends', component_name, components)

def runtime_depends(component_name, components):
    return _sorted_depends('runtime-depends', component_name, components)

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

def get_base_user_chroot_args():
    path = find_user_chroot_path()
    args = [path, '--unshare-pid', '--unshare-ipc']
    if not ostbuildrc.get_key('preserve_net', default=False):
        args.append('--unshare-net')
    return args

    
def resolve_component_meta(snapshot, component_meta):
    result = dict(component_meta)
    orig_src = component_meta['src']

    did_expand = False
    for (vcsprefix, expansion) in snapshot['vcsconfig'].iteritems():
        prefix = vcsprefix + ':'
        if orig_src.startswith(prefix):
            result['src'] = expansion + orig_src[len(prefix):]
            did_expand = True
            break

    name = component_meta.get('name')
    if name is None:
        if did_expand:
            src = orig_src
            idx = src.rindex(':')
            name = src[idx+1:]
        else:
            src = result['src']
            idx = src.rindex('/')
            name = src[idx+1:]
        if name.endswith('.git'):
            name = name[:-4]
        name = name.replace('/', '-')
        result['name'] = name

    branch_or_tag = result.get('branch') or result.get('tag')
    if branch_or_tag is None:
        result['branch'] = 'master'

    return result
