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
from .subprocess_helpers import run_sync

PREFIX = '/usr'

_BLACKLIST_REGEXPS = map(re.compile, 
                         [r'.*\.la$',
                          ])

_RUNTIME_DIRS = ['/etc']

_DOC_DIRS = ['/usr/share/doc',
             '/usr/share/gtk-doc',
             '/usr/share/man',
             '/usr/share/info']

_DEVEL_DIRS = ['/usr/include',
               '/usr/share/aclocal',
               '/usr/share/pkgconfig',
               '/usr/lib/pkgconfig']

_DEVEL_REGEXPS = map(re.compile,
                     [r'/(?:usr/)lib/[^/]+\.(?:so|a)$'])

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

        self.ostbuild_resultdir=None
        self.ostbuild_meta=None

        chdir = None

        for arg in args:
            if arg.startswith('--ostbuild-resultdir='):
                self.ostbuild_resultdir=arg[len('--ostbuild-resultdir='):]
            elif arg.startswith('--ostbuild-meta='):
                self.ostbuild_meta=arg[len('--ostbuild-meta='):]
            elif arg.startswith('--chdir='):
                os.chdir(arg[len('--chdir='):])
            elif arg.startswith('--'):
                self.configargs.append(arg)
            else:
                self.makeargs.append(arg)
        
        if self.ostbuild_resultdir is None:
            fatal("Must specify --ostbuild-resultdir=")

        self.metadata = {}

        if self.ostbuild_meta is None:
            output = subprocess.check_output(['ostbuild', 'autodiscover-meta'])
            self.metadata = json.loads(output)
        else:
            f = open(self.ostbuild_meta)
            self.metadata = json.load(f)
            f.close()

        for k in ['name', 'version']:
            if k not in self.metadata:
                fatal('Missing required key "%s" in metadata' % (k, ))

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

        if use_builddir:
            makefile_path = os.path.join(builddir, 'Makefile')
        else:
            makefile_path = 'Makefile'
        if not os.path.exists(makefile_path):
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

        name = self.metadata['name']
        assert ',' not in name
        branch = self.metadata['branch']
        assert ',' not in name
        version = self.metadata['version']
        assert ',' not in version
    
        root_name = self.metadata.get('buildroot', None)
        # TODO - pick up current sysroot version from ostree
        if root_name is None:
            root_name = 'unknown-' + self.build_target
            root_version = 'UNKNOWN'
        else:
            root_version = self.metadata.get('buildroot-version')
    
        tempdir = tempfile.mkdtemp(prefix='ostbuild-%s-' % (name,))
        self.tempfiles.append(tempdir)
        args = ['make', 'install', 'DESTDIR=' + tempdir]
        run_sync(args, cwd=builddir)
    
        devel_files = set()
        doc_files = set()
        runtime_files = set()
    
        oldpwd=os.getcwd()
        os.chdir(tempdir)
        for root, dirs, files in os.walk('.'):
            deleted_dirs = set() 
            for dirname in dirs:
                path = os.path.join(root, dirname)
                subpath = path[1:]
                matched = False
                for runtime_name in _RUNTIME_DIRS:
                    if subpath.startswith(runtime_name):
                        runtime_files.add(path)
                        matched = True
                        break
                if not matched:
                    for devel_name in _DEVEL_DIRS:
                        if subpath.startswith(devel_name):
                            devel_files.add(path)
                            matched = True
                            break
                if not matched:
                    for doc_name in _DOC_DIRS:
                        if subpath.startswith(doc_name):
                            doc_files.add(path)
                            matched = True
                            break
                if matched:
                    deleted_dirs.add(dirname)
            for dirname in deleted_dirs:
                dirs.remove(dirname)
    
            for filename in files:
                path = os.path.join(root, filename)
    
                blacklisted = False
                for r in _BLACKLIST_REGEXPS:
                    if r.match(path):
                        blacklisted = True
                        break
    
                if blacklisted:
                    continue

                matched = False
                for r in _DEVEL_REGEXPS:
                    if not r.match(path[1:]):
                        continue
                    devel_files.add(path)
                    matched = True
                    break
                if not matched:    
                    runtime_files.add(path)
        os.chdir(oldpwd)
    
        self.make_artifact('devel', devel_files, tempdir=tempdir)
        self.make_artifact('doc', doc_files, tempdir=tempdir)
        self.make_artifact('runtime', runtime_files, tempdir=tempdir)

        for tmpname in self.tempfiles:
            assert os.path.isabs(tmpname)
            if os.path.isdir(tmpname):
                shutil.rmtree(tmpname)
            else:
                try:
                    os.unlink(tmpname)
                except OSError, e:
                    pass

    def _rename_or_copy(self, src, dest):
        statsrc = os.lstat(src)
        statdest = os.lstat(os.path.dirname(dest))

        if stat.S_ISDIR(statsrc.st_mode):
            if not os.path.isdir(dest):
                os.mkdir(dest)
            for filename in os.listdir(src):
                src_child = os.path.join(src, filename)
                dest_child = os.path.join(dest, filename)

                self._rename_or_copy(src_child, dest_child)
        else:
            try:
                os.rename(src, dest)
            except OSError, e:
                if stat.S_ISLNK(statsrc.st_mode):
                    linkto = os.readlink(src)
                    os.symlink(linkto, dest)
                else:
                    shutil.copy2(src, dest)
    
    def make_artifact(self, dirtype, from_files, tempdir):
        resultdir = os.path.join(self.ostbuild_resultdir, dirtype)
        if os.path.isdir(resultdir):
            shutil.rmtree(resultdir)
        os.makedirs(resultdir)
                                 
        for filename in from_files:
            if filename.startswith('./'):
                filename = filename[2:]
            src_path = os.path.join(tempdir, filename)
            dest_path = os.path.join(resultdir, filename)
            dest_dir = os.path.dirname(dest_path)
            if not os.path.isdir(dest_dir):
                os.makedirs(dest_dir)
            try:
                self._rename_or_copy(src_path, dest_path)
            except OSError, e:
                fatal("Failed to copy %r to %r: %d %s" % (src_path, dest_path, e.errno, e.strerror))
        log("created: %s" % (os.path.abspath (resultdir), ))
    
builtins.register(OstbuildCompileOne)
