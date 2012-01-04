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

from . import builtins
from .ostbuildlog import log, fatal
from .subprocess_helpers import run_sync

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
        
        args = parser.parse_args(argv)

        if args.meta is None:
            output = subprocess.check_output(['ostbuild-autodiscover-meta'])
            ostbuild_meta_f = StringIO(output)
        else:
            ostbuild_meta_f = open(args.meta)

        metadata = {}
        for line in ostbuild_meta_f:
            (k,v) = line.split('=', 1)
            metadata[k.strip()] = v.strip()
        
        for k in ['NAME']:
            if k not in metadata:
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
        
        rev = subprocess.check_output(['ostree', '--repo=' + args.repo, 'rev-parse', args.buildroot])
        rev=rev.strip()
        
        metadata['BUILDROOT'] = args.buildroot
        metadata['BUILDROOT_VERSION'] = rev
        
        rootdir = os.path.join(workdir, 'root-' + rev)
        rootdir_tmp = rootdir + '.tmp'
        builddir = os.path.join(rootdir, 'ostbuild');
        if not os.path.isdir(rootdir):
            if os.path.isdir(rootdir_tmp):
                shutil.rmtree(rootdir_tmp)
            child_args = ['ostree', '--repo=' + args.repo, 'checkout', '-U', rev, rootdir_tmp]
            run_sync(child_args)
            builddir_tmp = os.path.join(rootdir_tmp, 'ostbuild')
            os.mkdir(builddir_tmp)
            os.mkdir(os.path.join(builddir_tmp, 'source'))
            os.mkdir(os.path.join(builddir_tmp, 'results'))
            os.rename(rootdir_tmp, rootdir)
            log("Checked out root: %s" % (rootdir, ))
        else:
            log("Using existing root: %s" % (rootdir, ))
        
        sourcedir=os.path.join(builddir, 'source', metadata['NAME'])
        if not os.path.isdir(sourcedir):
            os.mkdir(sourcedir)
        
        output_metadata = open('_ostbuild-meta', 'w')
        for (k,v) in metadata.iteritems():
            output_metadata.write('%s=%s\n' % (k, v))
        output_metadata.close()
        
        chroot_sourcedir = os.path.join('/ostbuild', 'source', metadata['NAME'])
        
        # We need to search PATH here manually so we correctly pick up an
        # ostree install in e.g. ~/bin even though we're going to set PATH
        # below for our children inside the chroot.
        ostbuild_user_chroot_path = None
        for dirname in os.environ['PATH'].split(':'):
            path = os.path.join(dirname, 'ostbuild-user-chroot')
            if os.access(path, os.X_OK):
                ostbuild_user_chroot_path = path
                break
        if ostbuild_user_chroot_path is None:
            ostbuild_user_chroot_path = 'ostbuild-user-chroot'
        
        child_args = [ostbuild_user_chroot_path, '--unshare-pid', '--unshare-net', '--unshare-ipc',
                      '--mount-readonly', '/',
                      '--mount-proc', '/proc', 
                      '--mount-bind', '/dev', '/dev',
                      '--mount-bind', child_tmpdir, '/tmp',
                      '--mount-bind', os.getcwd(), chroot_sourcedir]
        if args.resultdir:
            child_args.extend(['--mount-bind', args.resultdir, '/ostbuild/results'])
        child_args.extend([rootdir, '/bin/sh'])
        if not args.debug_shell:
            child_args.extend(['-c',
                               'cd "%s" && ostbuild compile-one --ostbuild-resultdir=/ostbuild/results --ostbuild-meta=_ostbuild-meta' % (chroot_sourcedir, )
                               ])
        run_sync(child_args, env=BUILD_ENV)
        
        if workdir_is_tmp:
            shutil.rmtree(workdir)
                       
builtins.register(OstbuildChrootCompileOne)
