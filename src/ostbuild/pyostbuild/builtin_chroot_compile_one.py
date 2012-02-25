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

    def execute(self, argv):
        parser = argparse.ArgumentParser(description=self.short_description)
        parser.add_argument('--meta')
        parser.add_argument('--debug-shell', action='store_true')
        
        args = parser.parse_args(argv)

        self.parse_config()

        component_name = os.path.basename(os.getcwd())
        build_manifest_path = os.path.join(self.workdir, 'manifest.json')
        self.manifest = json.load(open(build_manifest_path))

        if args.meta is not None:
            f = open(args.meta)
            self.metadata = json.load(f)
            f.close()

            component = buildutil.find_component_in_manifest(self.manifest, self.metadata['name'])
        else:
            component = buildutil.find_component_in_manifest(self.manifest, component_name)
            if component is None:
                fatal("Couldn't find component '%s' in manifest" % (component_name, ))

            self.metadata = component
            self.metadata['src'] = 'dirty:worktree'
            self.metadata['revision'] = 'dirty-worktree'

        components = self.manifest['components']
        index = components.index(component)
        dependencies = components[:index]

        architecture = os.uname()[4]
        buildroot_name = buildutil.manifest_buildroot_name(self.manifest, self.metadata, architecture)
        buildroot_version = buildutil.compose_buildroot(self.manifest, self.repo, buildroot_name,
                                                        self.metadata, dependencies, architecture)
        self.metadata['buildroot-name'] = buildroot_name
        self.metadata['buildroot-version'] = buildroot_version

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
        rootdir = os.path.join(rootdir_prefix, buildroot_version)
        
        rootdir_tmp = rootdir + '.tmp'
        builddir = os.path.join(rootdir, 'ostbuild');
        if not os.path.isdir(rootdir):
            if os.path.isdir(rootdir_tmp):
                shutil.rmtree(rootdir_tmp)
            child_args = ['ostree', '--repo=' + self.repo, 'checkout', '-U', buildroot_version, rootdir_tmp]
            run_sync(child_args)
            child_args = ['ostbuild', 'chroot-run-triggers', rootdir_tmp]
            run_sync(child_args)
            builddir_tmp = os.path.join(rootdir_tmp, 'ostbuild')
            os.mkdir(builddir_tmp)
            os.mkdir(os.path.join(builddir_tmp, 'source'))
            os.mkdir(os.path.join(builddir_tmp, 'results'))
            os.rename(rootdir_tmp, rootdir)
            log("Checked out root: %s" % (rootdir, ))
        else:
            log("Using existing root: %s" % (rootdir, ))
        
        sourcedir=os.path.join(builddir, 'source', self.metadata['name'])
        if not os.path.isdir(sourcedir):
            os.mkdir(sourcedir)
        
        output_metadata = open('_ostbuild-meta.json', 'w')
        json.dump(self.metadata, output_metadata)
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
        
builtins.register(OstbuildChrootCompileOne)
