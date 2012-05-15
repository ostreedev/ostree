# Copyright (C) 2011,2012 Colin Walters <walters@verbum.org>
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

# ostbuild-compile-one-make wraps systems that implement the GNOME build API:
# http://people.gnome.org/~walters/docs/build-api.txt

import os,sys,stat,subprocess,tempfile,re,shutil
from StringIO import StringIO
import json
from multiprocessing import cpu_count
import select,time

from . import builtins
from .ostbuildlog import log, fatal
from .subprocess_helpers import run_sync, run_sync_get_output

PREFIX = '/usr'

_DOC_DIRS = ['usr/share/doc',
             'usr/share/gtk-doc',
             'usr/share/man',
             'usr/share/info']

_DEVEL_DIRS = ['usr/include',
               'usr/share/aclocal',
               'usr/share/pkgconfig',
               'usr/lib/pkgconfig']

class OstbuildCompileOne(builtins.Builtin):
    name = "compile-one"
    short_description = "Build artifacts from the current source directory"

    def __init__(self):
        builtins.Builtin.__init__(self)
        self.tempfiles = []

    def _has_buildapi_configure_variable(self, name):
        var = '#buildapi-variable-%s' % (name, )
        for line in open('configure'):
            if line.find(var) >= 0:
                return True
        return False

    def execute(self, args):
        self.default_buildapi_jobs = ['-j', '%d' % (cpu_count() * 2, )]

        starttime = time.time()
        
        uname=os.uname()
        kernel=uname[0].lower()
        machine=uname[4]
        self.build_target='%s-%s' % (machine, kernel)

        self.configargs = ['--build=' + self.build_target,
                      '--prefix=' + PREFIX,
                      '--libdir=' + os.path.join(PREFIX, 'lib'),
                      '--sysconfdir=/etc',
                      '--localstatedir=/var',
                      '--bindir=' + os.path.join(PREFIX, 'bin'),
                      '--sbindir=' + os.path.join(PREFIX, 'sbin'),
                      '--datadir=' + os.path.join(PREFIX, 'share'),
                      '--includedir=' + os.path.join(PREFIX, 'include'),
                      '--libexecdir=' + os.path.join(PREFIX, 'libexec'),
                      '--mandir=' + os.path.join(PREFIX, 'share', 'man'),
                      '--infodir=' + os.path.join(PREFIX, 'share', 'info')]
        self.makeargs = ['make']

        self.ostbuild_resultdir='_ostbuild-results'
        self.ostbuild_meta_path='_ostbuild-meta.json'

        chdir = None
        opt_install = False

        for arg in args:
            if arg.startswith('--ostbuild-resultdir='):
                self.ostbuild_resultdir=arg[len('--ostbuild-resultdir='):]
            elif arg.startswith('--ostbuild-meta='):
                self.ostbuild_meta_path=arg[len('--ostbuild-meta='):]
            elif arg.startswith('--chdir='):
                os.chdir(arg[len('--chdir='):])
            else:
                self.makeargs.append(arg)
        
        f = open(self.ostbuild_meta_path)
        self.metadata = json.load(f)
        f.close()

        self.configargs.extend(self.metadata.get('config-opts', []))

        if self.metadata.get('rm-configure', False):
            configure_path = 'configure'
            if os.path.exists(configure_path):
                os.unlink(configure_path)

        autogen_script = None
        if not os.path.exists('configure'):
            log("No 'configure' script found, looking for autogen/bootstrap")
            for name in ['autogen', 'autogen.sh', 'bootstrap']:
                if os.path.exists(name):
                    log("Using bootstrap script '%s'" % (name, ))
                    autogen_script = name
            if autogen_script is None:
                fatal("No configure or autogen script detected; unknown buildsystem")

        if autogen_script is not None:
            env = dict(os.environ)
            env['NOCONFIGURE'] = '1'
            run_sync(['./' + autogen_script], env=env)
        else:
            log("Using existing 'configure' script")
            
        builddir = '_build'

        use_builddir = True
        doesnot_support_builddir = self._has_buildapi_configure_variable('no-builddir')
        if doesnot_support_builddir:
            log("Found no-builddir Build API variable; copying source tree to _build")
            if os.path.isdir('_build'):
                shutil.rmtree('_build')
            shutil.copytree('.', '_build', symlinks=True,
                            ignore=shutil.ignore_patterns('_build'))
            use_builddir = False
    
        if use_builddir:
            log("Using build directory %r" % (builddir, ))
            if not os.path.isdir(builddir):
                os.mkdir(builddir)
    
        if use_builddir:
            args = ['../configure']
        else:
            args = ['./configure']
        args.extend(self.configargs)
        run_sync(args, cwd=builddir)

        makefile_path = None
        for name in ['Makefile', 'makefile', 'GNUmakefile']:
            makefile_path = os.path.join(builddir, name)
            if os.path.exists(makefile_path):
                break
        if makefile_path is None:
            fatal("No Makefile found")

        args = list(self.makeargs)
        user_specified_jobs = False
        for arg in args:
            if arg == '-j':
                user_specified_jobs = True
    
        if not user_specified_jobs:
            has_notparallel = False
            for line in open(makefile_path):
                if line.startswith('.NOTPARALLEL'):
                    has_notparallel = True
                    log("Found .NOTPARALLEL")

            if not has_notparallel:
                log("Didn't find NOTPARALLEL, using parallel make by default")
                args.extend(self.default_buildapi_jobs)
    
        run_sync(args, cwd=builddir)

        tempdir = tempfile.mkdtemp(prefix='ostbuild-destdir-%s' % (self.metadata['name'].replace('/', '_'), ))
        self.tempfiles.append(tempdir)
        args = ['make', 'install', 'DESTDIR=' + tempdir]
        run_sync(args, cwd=builddir)
    
        runtime_path = os.path.join(self.ostbuild_resultdir, 'runtime')
        devel_path = os.path.join(self.ostbuild_resultdir, 'devel')
        doc_path = os.path.join(self.ostbuild_resultdir, 'doc')
        for artifact_type in ['runtime', 'devel', 'doc']:
            resultdir = os.path.join(self.ostbuild_resultdir, artifact_type)
            if os.path.isdir(resultdir):
                shutil.rmtree(resultdir)
            os.makedirs(resultdir)

        # Remove /var from the install - components are required to
        # auto-create these directories on demand.
        varpath = os.path.join(tempdir, 'var')
        if os.path.isdir(varpath):
            shutil.rmtree(varpath)

        # Move symbolic links for shared libraries as well
        # as static libraries.  And delete all .la files.
        for libdirname in ['lib', 'usr/lib']:
            path = os.path.join(tempdir, libdirname)
            if not os.path.isdir(path):
                continue
            for filename in os.listdir(path):
                subpath = os.path.join(path, filename)
                if filename.endswith('.la'):
                    os.unlink(subpath)
                    continue
                if not ((filename.endswith('.so')
                         and os.path.islink(filename))
                        or filename.endswith('.a')):
                    continue
                dest = os.path.join(devel_path, libdirname, filename)
                self._install_and_unlink(subpath, dest)

        for dirname in _DEVEL_DIRS:
            dirpath = os.path.join(tempdir, dirname)
            if os.path.isdir(dirpath):
                dest = os.path.join(devel_path, dirname)
                self._install_and_unlink(dirpath, dest)

        for dirname in _DOC_DIRS:
            dirpath = os.path.join(tempdir, dirname)
            if os.path.isdir(dirpath):
                dest = os.path.join(doc_path, dirname)
                self._install_and_unlink(dirpath, dest)
    
        for filename in os.listdir(tempdir):
            src_path = os.path.join(tempdir, filename)
            dest_path = os.path.join(runtime_path, filename)
            self._install_and_unlink(src_path, dest_path)

        for tmpname in self.tempfiles:
            assert os.path.isabs(tmpname)
            if os.path.isdir(tmpname):
                shutil.rmtree(tmpname)
            else:
                try:
                    os.unlink(tmpname)
                except OSError, e:
                    pass

        endtime = time.time()

        log("Compilation succeeded; %d seconds elapsed" % (int(endtime - starttime),))
        log("Results placed in %s" % (self.ostbuild_resultdir, ))

    def _install_and_unlink(self, src, dest):
        statsrc = os.lstat(src)
        dirname = os.path.dirname(dest)
        if not os.path.isdir(dirname):
            os.makedirs(dirname)

        if stat.S_ISDIR(statsrc.st_mode):
            if not os.path.isdir(dest):
                os.mkdir(dest)
            for filename in os.listdir(src):
                src_child = os.path.join(src, filename)
                dest_child = os.path.join(dest, filename)

                self._install_and_unlink(src_child, dest_child)
            os.rmdir(src)
        else:
            try:
                os.rename(src, dest)
            except OSError, e:
                if stat.S_ISLNK(statsrc.st_mode):
                    linkto = os.readlink(src)
                    os.symlink(linkto, dest)
                else:
                    shutil.copy2(src, dest)
                os.unlink(src)
    
builtins.register(OstbuildCompileOne)
