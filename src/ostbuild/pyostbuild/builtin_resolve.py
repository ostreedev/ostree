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
import argparse
import json
from StringIO import StringIO

from . import builtins
from .ostbuildlog import log, fatal
from .subprocess_helpers import run_sync, run_sync_get_output
from . import ostbuildrc
from . import buildutil
from . import kvfile
from . import odict

class OstbuildResolve(builtins.Builtin):
    name = "resolve"
    short_description = "Download the source code for a given manifest"

    def __init__(self):
        builtins.Builtin.__init__(self)

    def _ensure_vcs_mirror(self, name, keytype, uri, branch):
        assert keytype == 'git'
        mirror = os.path.join(self.mirrordir, name)
        tmp_mirror = mirror + '.tmp'
        if os.path.isdir(tmp_mirror):
            shutil.rmtree(tmp_mirror)
        if not os.path.isdir(mirror):
            run_sync(['git', 'clone', '--mirror', uri, tmp_mirror])
            os.rename(tmp_mirror, mirror)
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
        parser.add_argument('--fetch', action='store_true')
        parser.add_argument('components', nargs='*')

        args = parser.parse_args(argv)
        self.args = args
        
        self.parse_config()

        manifest_path = self.ostbuildrc.get_key('manifest')
        self.manifest = json.load(open(manifest_path))

        self.resolved_components = map(self._resolve_component_meta, self.manifest['components'])
        for component in self.resolved_components:
            (keytype, uri) = self._parse_src_key(component['src'])
            mirrordir = self._ensure_vcs_mirror(component['name'],
                                                keytype, uri,
                                                component['branch'])
            revision = buildutil.get_git_version_describe(mirrordir,
                                                          component['branch'])
            component['revision'] = revision

        if args.fetch:
            if len(args.components) == 0:
                fetch_components = map(lambda x: x['name'], self.resolved_components)
            else:
                fetch_components = args.components
            for component_name in fetch_components:
                found = False
                for component in self.resolved_components:
                    if component['name'] == component_name:
                        found = True
                        break
                if not found:
                    fatal("Unknown component %r" % (component_name, ))
                mirrordir = self._ensure_vcs_mirror(component['name'],
                                                    keytype, uri,
                                                    component['branch'])
                log("Running git fetch for %s" % (component['name'], ))
                run_sync(['git', 'fetch'], cwd=mirrordir, log_initiation=False)

        self.manifest['components'] = self.resolved_components

        out_manifest = os.path.join(self.workdir, 'manifest.json')
        patchdir = os.path.join(self.workdir, 'patches')
        if not os.path.isdir(patchdir):
            os.mkdir(patchdir)
        all_patches = {}
        for component in self.resolved_components:
            patches = component.get('patches', [])
            for patch in patches:
                all_patches[patch] = True
        for patch in all_patches:
            src = os.path.join(os.path.dirname(manifest_path),
                               patch)
            dest = os.path.join(patchdir, patch)
            shutil.copy(src, dest)
        
        f = open(out_manifest, 'w')
        json.dump(self.manifest, f, indent=4)
        f.close()
        print "Created: %s, %d patches" % (out_manifest, len(all_patches.keys()))
        
builtins.register(OstbuildResolve)
