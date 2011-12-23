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
                      r'/(?:usr/)lib(?:|(?:32)|(?:64))/pkgconfig/.*\.pc$',
                      r'/(?:usr/)lib(?:|(?:32)|(?:64))/[^/]+\.so$'
                      ])

class BuildSystemScanner(object):
    @classmethod
    def _find_file(cls, names):
        for name in names:
            if os.path.exists(name):
                return name
        return None

    @classmethod
    def get_configure_source_script(cls):
        return cls._find_file(('./configure.ac', './configure.in'))

    @classmethod
    def get_configure_script(cls):
        return cls._find_file(('./configure', ))

    @classmethod
    def get_bootstrap_script(cls):
        return cls._find_file(('./autogen.sh', ))

    @classmethod
    def get_silent_rules(cls):
        src = cls.get_configure_source_script()
        if not src:
            return False
        f = open(src)
        for line in f:
            if line.find('AM_SILENT_RULES') >= 0:
                f.close()
                return True
        f.close()
        return False

class OstbuildCompileOne(builtins.Builtin):
    name = "compile-one"
    short_description = "Build artifacts from the current source directory"

    def __init__(self):
        builtins.Builtin.__init__(self)
        self.tempfiles = []

    def _search_file(self, filename, pattern):
        f = open(filename)
        for line in f:
            if line.startswith(pattern):
                f.close()
                return line
        f.close()
        return None

    def _find_buildapi_makevariable(self, name, builddir='.'):
        var = '.%s:' % (name, )
        line = None
        path = os.path.join(builddir, 'Makefile.in')
        if os.path.exists(path):
            line = self._search_file(path, var)
        path = os.path.join(builddir, 'Makefile')
        if not line and os.path.exists(path):
            line = self._search_file(path, var)
        return line is not None

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
                self.ostbuild_resultdir=arg[len('ostbuild-resultdir='):]
            elif arg.startswith('ostbuild-meta='):
                self.ostbuild_meta=arg[len('ostbuild-meta='):]
            elif arg.startswith('--'):
                self.configargs.append(arg)
            else:
                self.makeargs.append(arg)

        self.metadata = {}

        if self.ostbuild_meta is None:
            output = subprocess.check_output(['ostbuild-autodiscover-meta'])
            ostbuild_meta_f = StringIO(output)
        else:
            ostbuild_meta_f = open(ostbuild_meta)

        for line in ostbuild_meta_f:
            (k,v) = line.split('=', 1)
            self.metadata[k.strip()] = v.strip()

        ostbuild_meta_f.close()

        for k in ['NAME', 'VERSION']:
            if k not in self.metadata:
                fatal('Missing required key "%s" in metadata' % (k, ))

        self.phase_bootstrap()

    def phase_bootstrap(self):
        have_configure = BuildSystemScanner.get_configure_script() 
        have_configure_source = BuildSystemScanner.get_configure_source_script()
        if not (have_configure or have_configure_source):
            fatal("No configure or bootstrap script detected; unknown buildsystem")
            return
    
        need_v1 = BuildSystemScanner.get_silent_rules()
        if need_v1:
            log("Detected AM_SILENT_RULES, adding --disable-silent-rules to configure")
            self.configargs.append('--disable-silent-rules')
    
        if have_configure:
            self.phase_configure()
        else:
            bootstrap = BuildSystemScanner.get_bootstrap_script()
            if bootstrap:
                log("Detected bootstrap script: %s, using it" % (bootstrap, ))
                args = [bootstrap]
                # Add NOCONFIGURE; GNOME style scripts use this
                env = dict(os.environ)
                env['NOCONFIGURE'] = '1'
                run_sync(args, env=env)
            else:
                log("No bootstrap script found; using generic autoreconf")
                run_sync(['autoreconf', '-f', '-i'])
            self.phase_configure()
    
    def phase_configure(self):
        use_builddir = True
        doesnot_support_builddir = self._find_buildapi_makevariable('buildapi-no-builddir')
        if doesnot_support_builddir:
            log("Found .buildapi-no-builddir; copying source tree to _build")
            shutil.rmtree('_build')
            os.mkdir('_build')
            shutil.copytree('.', '_build', symlinks=True,
                            ignore=shutil.ignore_patterns('_build'))
            use_builddir = False
    
        if use_builddir:
            builddir = '_build'
            log("Using build directory %r" % (builddir, ))
            if not os.path.isdir(builddir):
                os.mkdir(builddir)
    
        configstatus = 'config.status'
        if not os.path.exists(configstatus):
            if use_builddir:
                args = ['../configure']
            else:
                args = ['./configure']
            args.extend(self.configargs)
            if use_builddir:
                run_sync(args, cwd=builddir)
            else:
                run_sync(args)
        else:
            log("Found %s, skipping configure" % (configstatus, ))
        self.phase_build(builddir=builddir)
    
    build_status = False
    
    def phase_build(self, builddir=None):
        if not os.path.exists(os.path.join(builddir, 'Makefile')):
            fatal("No Makefile found")
        args = list(self.makeargs)
        user_specified_jobs = False
        for arg in args:
            if arg == '-j':
                user_specified_jobs = True
    
        if not user_specified_jobs:
            notparallel = self._find_buildapi_makevariable('NOTPARALLEL', builddir=builddir)
            if not notparallel:
                log("Didn't find NOTPARALLEL, using parallel make by default")
                args.extend(self.default_buildapi_jobs)
    
        run_sync(args, cwd=builddir)
    
        self.phase_make_artifacts(builddir=builddir)
    
    def make_artifact(self, name, from_files, tempdir=None, resultdir=None):
        targz_name = name + '.tar.gz'
        (fd,filelist_temp)=tempfile.mkstemp(prefix='ostree-filelist-%s' % (name, ))
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
    
    def phase_make_artifacts(self, builddir=None):
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

        tempdir = tempfile.mkdtemp(prefix='ostree-build-%s-' % (name,))
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

        self.phase_complete()

    def phase_complete(self):
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

builtins.register(OstbuildCompileOne)
