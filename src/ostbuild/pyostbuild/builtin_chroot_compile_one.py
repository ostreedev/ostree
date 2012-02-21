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
        parser.add_argument('--workdir')
        parser.add_argument('--repo', required=True)
        parser.add_argument('--resultdir')
        parser.add_argument('--buildroot', required=True)
        parser.add_argument('--meta')
        parser.add_argument('--debug-shell', action='store_true')
        
        (args, rest_args) = parser.parse_known_args(argv)

        if args.meta is None:
            output = run_sync_get_output(['ostbuild', 'autodiscover-meta'])
            self.metadata = json.loads(output)
        else:
            f = open(args.meta)
            self.metadata = json.load(f)
            f.close()

        for k in ['name']:
            if k not in self.metadata:
                sys.stderr.write('Missing required key "%s" in metadata' % (k, ))
                sys.exit(1)
        
        workdir_is_tmp = (args.workdir is None)
        if workdir_is_tmp:
            workdir = tempfile.mkdtemp(prefix='ostbuild-chroot-compile-')
        else:
            workdir = args.workdir
            
        log("Using working directory: %s" % (workdir, ))
        
        child_tmpdir=os.path.join(workdir, 'tmp')
        if os.path.isdir(child_tmpdir):
            log("Cleaning up previous tmpdir: %r" % (child_tmpdir, ))
            shutil.rmtree(child_tmpdir)
        os.mkdir(child_tmpdir)
        
        rev = run_sync_get_output(['ostree', '--repo=' + args.repo, 'rev-parse', args.buildroot])
        rev = rev.strip()
        
        self.metadata['buildroot'] = args.buildroot
        self.metadata['buildroot-version'] = rev
        
        rootdir_prefix = os.path.join(workdir, 'roots')
        if not os.path.isdir(rootdir_prefix):
            os.makedirs(rootdir_prefix)
        rootdir = os.path.join(rootdir_prefix, rev)
        
        rootdir_tmp = rootdir + '.tmp'
        builddir = os.path.join(rootdir, 'ostbuild');
        if not os.path.isdir(rootdir):
            if os.path.isdir(rootdir_tmp):
                shutil.rmtree(rootdir_tmp)
            child_args = ['ostree', '--repo=' + args.repo, 'checkout', '-U', rev, rootdir_tmp]
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
        
        output_metadata = open('_ostbuild-meta', 'w')
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
                      '--chdir', chroot_sourcedir]
        if args.resultdir:
            child_args.extend(['--mount-bind', args.resultdir, '/ostbuild/results'])
        if args.debug_shell:
            child_args.extend([rootdir, '/bin/sh'])
        else:
            child_args.extend([rootdir, '/usr/bin/ostbuild',
                               'compile-one',
                               '--ostbuild-resultdir=/ostbuild/results',
                               '--ostbuild-meta=_ostbuild-meta'])
            child_args.extend(rest_args)
        env_copy = dict(buildutil.BUILD_ENV)
        env_copy['PWD'] = chroot_sourcedir
        run_sync(child_args, env=env_copy, keep_stdin=args.debug_shell)
        
        if workdir_is_tmp:
            shutil.rmtree(workdir)
                       
builtins.register(OstbuildChrootCompileOne)
