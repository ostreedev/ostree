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

import os,sys,subprocess,tempfile,re,shutil
import copy
import argparse
import json
import time
import urlparse
from StringIO import StringIO

from . import builtins
from .ostbuildlog import log, fatal
from .subprocess_helpers import run_sync, run_sync_get_output
from . import ostbuildrc
from . import jsondb
from . import buildutil
from . import kvfile
from . import odict

class OstbuildResolve(builtins.Builtin):
    name = "resolve"
    short_description = "Download the source code for a given manifest"

    def __init__(self):
        builtins.Builtin.__init__(self)

    def _ensure_vcs_mirror(self, name, keytype, uri, branch):
        # FIXME - remove "name" from parameter list here - hash uri?
        mirror = buildutil.get_mirrordir(self.mirrordir, keytype, uri)
        tmp_mirror = mirror + '.tmp'
        if os.path.isdir(tmp_mirror):
            shutil.rmtree(tmp_mirror)
        if not os.path.isdir(mirror):
            run_sync(['git', 'clone', '--mirror', uri, tmp_mirror])
            run_sync(['git', 'config', 'gc.auto', '0'], cwd=tmp_mirror)
            os.rename(tmp_mirror, mirror)
        if branch is None:
            return mirror
        last_fetch_path = mirror + '.%s-lastfetch' % (name, )
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
            tmp_checkout = buildutil.get_mirrordir(self.mirrordir, keytype, uri, prefix='_tmp-checkouts')
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
                self._ensure_vcs_mirror(name + '-' + sub_name, keytype, sub_url, sub_checksum)
            shutil.rmtree(tmp_checkout)
            f = open(last_fetch_path, 'w')
            f.write(current_vcs_version + '\n')
            f.close()
        return mirror

    def _parse_src_key(self, srckey):
        idx = srckey.find(':')
        if idx < 0:
            raise ValueError("Invalid SRC uri=%s" % (srckey, ))
        keytype = srckey[:idx]
        if keytype not in ['git']:
            raise ValueError("Unsupported SRC uri=%s" % (srckey, ))
        uri = srckey[idx+1:]
        return (keytype, uri)

    def _resolve_component_meta(self, component_meta):
        result = dict(component_meta)
        orig_src = component_meta['src']

        did_expand = False
        for (vcsprefix, expansion) in self.manifest['vcsconfig'].iteritems():
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

        if 'branch' not in result:
            result['branch'] = 'master'

        return result
    
    def execute(self, argv):
        parser = argparse.ArgumentParser(description=self.short_description)
        parser.add_argument('--manifest', required=True)
        parser.add_argument('--fetch', action='store_true')
        parser.add_argument('components', nargs='*')

        args = parser.parse_args(argv)
        self.args = args
        
        self.parse_config()
        self.repo = ostbuildrc.get_key('repo')

        self.manifest = json.load(open(args.manifest))
        self.prefix = self.manifest['prefix']

        snapshot = copy.deepcopy(self.manifest)
        component_source_list = map(self._resolve_component_meta, self.manifest['components'])
        del snapshot['components']

        if args.fetch:
            if len(args.components) == 0:
                fetch_components = map(lambda x: x['name'], component_source_list)
            else:
                fetch_components = args.components
            for component_name in fetch_components:
                found = False
                for component in component_source_list:
                    if component['name'] == component_name:
                        found = True
                        break
                if not found:
                    fatal("Unknown component %r" % (component_name, ))
                (keytype, uri) = self._parse_src_key(component['src'])
                mirrordir = self._ensure_vcs_mirror(component_name, keytype, uri, None)
                log("Running git fetch for %s" % (component['name'], ))
                run_sync(['git', 'fetch'], cwd=mirrordir, log_initiation=False)
        else:
            fetch_components = []

        global_patches_meta = self._resolve_component_meta(self.manifest['patches'])
        (keytype, uri) = self._parse_src_key(global_patches_meta['src'])
        mirrordir = self._ensure_vcs_mirror(global_patches_meta['name'], keytype, uri, global_patches_meta['branch'])
        revision = buildutil.get_git_version_describe(mirrordir, global_patches_meta['branch'])
        global_patches_meta['revision'] = revision

        unique_component_names = set()
        for component in component_source_list:
            (keytype, uri) = self._parse_src_key(component['src'])
            name = component['name']

            if name in unique_component_names:
                fatal("Duplicate component name '%s'" % (name, ))
            unique_component_names.add(name)

            mirrordir = self._ensure_vcs_mirror(name, keytype, uri, component['branch'])
            revision = buildutil.get_git_version_describe(mirrordir,
                                                          component['branch'])
            component['revision'] = revision

            config_opts = list(self.manifest['config-opts'])
            config_opts.extend(component.get('config-opts', []))
            component['config-opts'] = config_opts

            patch_files = component.get('patches')
            if patch_files is not None:
                component['patches'] = copy.deepcopy(global_patches_meta)
                component['patches']['files'] = patch_files

        manifest_architectures = snapshot['architectures']

        ostree_prefix = snapshot['prefix']
        base_prefix = '%s/%s' % (snapshot['base']['name'], ostree_prefix)
        
        snapshot['architecture-buildroots'] = {}
        for architecture in manifest_architectures:
            snapshot['architecture-buildroots'][architecture] = '%s-%s-devel' % (base_prefix, architecture)

        components_by_name = {}
        component_ordering = []
        build_prev_component = None
        runtime_prev_component = None
        runtime_components = []
        devel_components = []

        builds = {}

        for component in component_source_list:
            base_name = component['name']
            name = '%s/%s' % (ostree_prefix, base_name)
            component['name'] = name

            components_by_name[name] = component

            if build_prev_component is not None:
                component['build-depends'] = [build_prev_component['name']]
            build_prev_component = component

            is_runtime = component.get('component', 'runtime') == 'runtime'

            if runtime_prev_component is not None:
                component['runtime-depends'] = [runtime_prev_component['name']]

            if is_runtime:
                runtime_prev_component = component
                runtime_components.append(component)
            devel_components.append(component)

            is_noarch = component.get('noarch', False)
            if is_noarch:
                # Just use the first specified architecture
                component_arches = [manifest_architectures[0]]
            else:
                component_arches = component.get('architectures', manifest_architectures)
            builds[name] = component_arches

        # We expanded these keys
        del snapshot['config-opts']
        del snapshot['vcsconfig']
        del snapshot['patches']
        del snapshot['architectures']

        targets_list = []
        snapshot['targets'] = targets_list
        for target_component_type in ['runtime', 'devel']:
            for architecture in manifest_architectures:
                target = {}
                targets_list.append(target)
                target['name'] = '%s-%s-%s' % (ostree_prefix, architecture, target_component_type)

                base_ref = '%s-%s-%s' % (base_prefix, architecture, target_component_type)
                target['base'] = {'name': base_ref}

                if target_component_type == 'runtime':
                    target_components = runtime_components
                else:
                    target_components = devel_components
                    
                contents = []
                for component in target_components:
                    builds_for_component = builds[component['name']]
                    if architecture not in builds_for_component:
                        continue
                    binary_name = '%s/%s' % (component['name'], architecture)
                    component_ref = {'name': binary_name}
                    if target_component_type == 'runtime':
                        component_ref['trees'] = ['/runtime']
                    else:
                        component_ref['trees'] = ['/runtime', '/devel', '/doc']
                    contents.append(component_ref)
                target['contents'] = contents

        for component in components_by_name.itervalues():
            del component['name']
        snapshot['components'] = components_by_name

        snapshot['00ostree-src-snapshot-version'] = 0

        current_time = time.time()

        src_db = self.get_src_snapshot_db()
        path = src_db.store(snapshot)
        log("Source snapshot: %s" % (path, ))
        
builtins.register(OstbuildResolve)
