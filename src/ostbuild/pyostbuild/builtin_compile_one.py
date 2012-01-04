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

# ostbuild-compile-one-make wraps systems that implement the GNOME build API:
# http://people.gnome.org/~walters/docs/build-api.txt

import os,sys,subprocess,tempfile,re,shutil
from StringIO import StringIO
from multiprocessing import cpu_count
import select,time

from . import builtins
from .ostbuildlog import log, fatal
from .subprocess_helpers import run_sync

PREFIX = '/usr'

_BLACKLIST_REGEXPS = map(re.compile, 
                         [r'.*\.la$',
                          ])

_DEVEL_REGEXPS = map(re.compile,
                     [r'/usr/include/',
                      r'/usr/share/pkgconfig/',
                      r'/usr/share/aclocal/',
                      r'/(?:usr/)lib(?:|(?:32)|(?:64))/pkgconfig/.*\.pc$',
                      r'/(?:usr/)lib(?:|(?:32)|(?:64))/[^/]+\.so$'
                      r'/(?:usr/)lib(?:|(?:32)|(?:64))/[^/]+\.a$'
                      ])

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

        # libdir detection
        if os.path.isdir('/lib64'):
            libdir=os.path.join(PREFIX, 'lib64')
        else:
            libdir=os.path.join(PREFIX, 'lib')

        self.configargs = ['--build=' + self.build_target,
                      '--prefix=' + PREFIX,
                      '--libdir=' + libdir,
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

        self.ostbuild_resultdir=os.getcwd()
        self.ostbuild_meta=None

        for arg in args:
            if arg.startswith('--ostbuild-resultdir='):
                self.ostbuild_resultdir=arg[len('--ostbuild-resultdir='):]
            elif arg.startswith('--ostbuild-meta='):
                self.ostbuild_meta=arg[len('--ostbuild-meta='):]
            elif arg.startswith('--'):
                self.configargs.append(arg)
            else:
                self.makeargs.append(arg)

        self.metadata = {}

        if self.ostbuild_meta is None:
            output = subprocess.check_output(['ostbuild', 'autodiscover-meta'])
            ostbuild_meta_f = StringIO(output)
        else:
            ostbuild_meta_f = open(self.ostbuild_meta)

        for line in ostbuild_meta_f:
            (k,v) = line.split('=', 1)
            self.metadata[k.strip()] = v.strip()

        ostbuild_meta_f.close()

        for k in ['NAME', 'VERSION']:
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
    
        use_builddir = True
        doesnot_support_builddir = self._has_buildapi_configure_variable('no-builddir')
        if doesnot_support_builddir:
            log("Found no-builddir Build API variable; copying source tree to _build")
            if os.path.isdir('_build'):
                shutil.rmtree('_build')
            shutil.copytree('.', '_build', symlinks=True,
                            ignore=shutil.ignore_patterns('_build'))
            use_builddir = False
            builddir = '.'
    
        if use_builddir:
            builddir = '_build'
            log("Using build directory %r" % (builddir, ))
            if not os.path.isdir(builddir):
                os.mkdir(builddir)
    
        if use_builddir:
            args = ['../configure']
        else:
            args = ['./configure']
        args.extend(self.configargs)
        if use_builddir:
            run_sync(args, cwd=builddir)
        else:
            run_sync(args)

        makefile_path = os.path.join(builddir, 'Makefile')
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

        name = self.metadata['NAME']
        assert ',' not in name
        branch = self.metadata['BRANCH']
        assert ',' not in name
        version = self.metadata['VERSION']
        assert ',' not in version
    
        root_name = self.metadata.get('BUILDROOT', None)
        # TODO - pick up current sysroot version from ostree
        if root_name is None:
            root_name = 'unknown-' + self.build_target
            root_version = 'UNKNOWN'
        else:
            root_version = self.metadata.get('BUILDROOT_VERSION')
    
        artifact_prefix='artifact-%s,%s,%s,%s,%s' % (root_name, root_version, name, branch, version)

        tempdir = tempfile.mkdtemp(prefix='ostbuild-%s-' % (name,))
        self.tempfiles.append(tempdir)
        args = ['make', 'install', 'DESTDIR=' + tempdir]
        run_sync(args, cwd=builddir)
    
        devel_files = set()
        runtime_files = set()
    
        oldpwd=os.getcwd()
        os.chdir(tempdir)
        for root, dirs, files in os.walk('.'):
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
    
        if devel_files:
            self.make_artifact(artifact_prefix + '-devel', devel_files, tempdir=tempdir, resultdir=self.ostbuild_resultdir)
        self.make_artifact(artifact_prefix + '-runtime', runtime_files, tempdir=tempdir, resultdir=self.ostbuild_resultdir)

        for tmpname in self.tempfiles:
            assert os.path.isabs(tmpname)
            if os.path.isdir(tmpname):
                shutil.rmtree(tmpname)
            else:
                try:
                    os.unlink(tmpname)
                    pass
                except OSError, e:
                    pass
    
    def make_artifact(self, name, from_files, tempdir=None, resultdir=None):
        targz_name = name + '.tar.gz'
        (fd,filelist_temp)=tempfile.mkstemp(prefix='ostbuild-filelist-%s' % (name, ))
        os.close(fd)
        self.tempfiles.append(filelist_temp)
        f = open(filelist_temp, 'w')
        for filename in from_files:
            assert ('\n' not in filename)
            f.write(filename)
            f.write('\n')
        f.close()
        if resultdir:
            result_path = os.path.join(resultdir, targz_name)
        else:
            result_path = targz_name
        args = ['tar', '-c', '-z', '-C', tempdir, '-f', result_path, '-T', filelist_temp]
        run_sync(args)
        log("created: %s" % (os.path.abspath (result_path), ))
    
builtins.register(OstbuildCompileOne)
