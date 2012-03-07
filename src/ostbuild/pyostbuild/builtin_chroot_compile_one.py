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

import os,sys,re,subprocess,tempfile,shutil
from StringIO import StringIO
import argparse
import json

from . import builtins
from . import buildutil
from .ostbuildlog import log, fatal
from .subprocess_helpers import run_sync, run_sync_get_output

class OstbuildChrootCompileOne(builtins.Builtin):
    name = "chroot-compile-one"
    short_description = "Build artifacts from the current source directory in a chroot"

    def _compose_buildroot(self, component, dirpath):
        components = self.manifest['components']
        index = components.index(component)
        dependencies = components[:index]

        base = 'bases/%s' % (self.manifest['base'], )
        checkout_trees = [(base, '/')]
        for dep in dependencies:
            buildname = buildutil.manifest_buildname(self.manifest, dep)
            checkout_trees.append((buildname, '/runtime'))
            checkout_trees.append((buildname, '/devel'))

        for (branch, rootpath) in checkout_trees:
            run_sync(['ostree', '--repo=' + self.repo,
                      'checkout', '--user-mode',
                      '--union', '--subpath=' + rootpath,
                      branch, dirpath])

    def execute(self, argv):
        parser = argparse.ArgumentParser(description=self.short_description)
        parser.add_argument('--manifest', required=True)
        parser.add_argument('--pristine', action='store_true')
        parser.add_argument('--debug-shell', action='store_true')
        
        args = parser.parse_args(argv)

        self.parse_config()

        component_name = os.path.basename(os.getcwd())
        self.manifest = json.load(open(args.manifest))

        component = buildutil.find_component_in_manifest(self.manifest, component_name)
        self.metadata = component
        if component is None:
            fatal("Couldn't find component '%s' in manifest" % (component_name, ))
        if not args.pristine:
            self.metadata['src'] = 'dirty:worktree'
            self.metadata['revision'] = 'dirty-worktree'

        architecture = os.uname()[4]

        if 'name' not in self.metadata:
            sys.stderr.write('Missing required key "%s" in metadata' % (k, ))
            sys.exit(1)

        workdir = self.workdir
            
        log("Using working directory: %s" % (workdir, ))
        
        child_tmpdir=os.path.join(workdir, 'tmp')
        if os.path.isdir(child_tmpdir):
            log("Cleaning up previous tmpdir: %r" % (child_tmpdir, ))
            shutil.rmtree(child_tmpdir)
        os.mkdir(child_tmpdir)

        resultdir = os.path.join(self.workdir, 'results', component['name'])
        if os.path.isdir(resultdir):
            shutil.rmtree(resultdir)
        os.makedirs(resultdir)
        
        rootdir_prefix = os.path.join(workdir, 'roots')
        if not os.path.isdir(rootdir_prefix):
            os.makedirs(rootdir_prefix)
        rootdir = os.path.join(rootdir_prefix, component['name'])
        if os.path.isdir(rootdir):
            shutil.rmtree(rootdir)
        
        rootdir_tmp = rootdir + '.tmp'
        builddir = os.path.join(rootdir, 'ostbuild');
        if os.path.isdir(rootdir_tmp):
            shutil.rmtree(rootdir_tmp)
        os.mkdir(rootdir_tmp)
            
        self._compose_buildroot(component, rootdir_tmp)

        child_args = ['ostbuild', 'chroot-run-triggers', rootdir_tmp]
        run_sync(child_args)

        builddir_tmp = os.path.join(rootdir_tmp, 'ostbuild')
        os.mkdir(builddir_tmp)
        os.mkdir(os.path.join(builddir_tmp, 'source'))
        os.mkdir(os.path.join(builddir_tmp, 'results'))
        os.rename(rootdir_tmp, rootdir)
        log("Checked out buildroot: %s" % (rootdir, ))
        
        sourcedir=os.path.join(builddir, 'source', self.metadata['name'])
        if not os.path.isdir(sourcedir):
            os.mkdir(sourcedir)
        
        output_metadata = open('_ostbuild-meta.json', 'w')
        json.dump(self.metadata, output_metadata, indent=4, sort_keys=True)
        output_metadata.close()
        
        chroot_sourcedir = os.path.join('/ostbuild', 'source', self.metadata['name'])

        ostbuild_user_chroot_path = buildutil.find_user_chroot_path()
        
        child_args = [ostbuild_user_chroot_path, '--unshare-pid', '--unshare-net', '--unshare-ipc',
                      '--mount-readonly', '/',
                      '--mount-proc', '/proc', 
                      '--mount-bind', '/dev', '/dev',
                      '--mount-bind', child_tmpdir, '/tmp',
                      '--mount-bind', os.getcwd(), chroot_sourcedir,
                      '--mount-bind', resultdir, '/ostbuild/results',
                      '--chdir', chroot_sourcedir]
        if args.debug_shell:
            child_args.extend([rootdir, '/bin/sh'])
        else:
            child_args.extend([rootdir, '/usr/bin/ostbuild',
                               'compile-one',
                               '--ostbuild-resultdir=/ostbuild/results',
                               '--ostbuild-meta=_ostbuild-meta.json'])
            child_args.extend(self.metadata.get('config-opts', []))
        env_copy = dict(buildutil.BUILD_ENV)
        env_copy['PWD'] = chroot_sourcedir
        run_sync(child_args, env=env_copy, keep_stdin=args.debug_shell)

        recorded_meta = dict(self.metadata)
        del recorded_meta['revision']
        patches_recorded_meta = recorded_meta.get('patches')
        if patches_recorded_meta is not None:
            del patches_recorded_meta['revision']

        recorded_meta_path = os.path.join(resultdir, '_ostbuild-meta.json')
        recorded_meta_f = open(recorded_meta_path, 'w')
        json.dump(recorded_meta, recorded_meta_f, indent=4, sort_keys=True)
        recorded_meta_f.close()
        
builtins.register(OstbuildChrootCompileOne)
