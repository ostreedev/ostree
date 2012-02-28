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
import time
import urlparse
import json
from StringIO import StringIO

from . import builtins
from .ostbuildlog import log, fatal
from .subprocess_helpers import run_sync, run_sync_get_output
from .subprocess_helpers import run_sync_monitor_log_file
from . import ostbuildrc
from . import buildutil
from . import kvfile
from . import odict
from . import vcs

class BuildOptions(object):
    pass

class OstbuildBuild(builtins.Builtin):
    name = "build"
    short_description = "Rebuild all artifacts from the given manifest"

    def __init__(self):
        builtins.Builtin.__init__(self)

    def _get_ostbuild_chroot_args(self, architecture):
        current_machine = os.uname()[4]
        if current_machine != architecture:
            args = ['setarch', architecture]
        else:
            args = []
        args.extend(['ostbuild', 'chroot-compile-one'])
        return args

    def _launch_debug_shell(self, architecture, buildroot, cwd=None):
        args = self._get_ostbuild_chroot_args(architecture)
        args.extend(['--buildroot=' + buildroot,
                     '--workdir=' + self.workdir,
                     '--debug-shell'])
        run_sync(args, cwd=cwd, fatal_on_error=False, keep_stdin=True)
        fatal("Exiting after debug shell")

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

    def _build_one_component(self, meta, architecture):
        name = meta['name']
        branch = meta['branch']

        target = buildutil.manifest_target(self.manifest, architecture)
        buildname = buildutil.manifest_buildname(self.manifest, meta, architecture)
        buildroot_name = buildutil.manifest_buildroot_name(self.manifest, meta, architecture)

        (keytype, uri) = buildutil.parse_src_key(meta['src'])

        mirror = buildutil.get_mirrordir(self.mirrordir, keytype, uri)
        checkoutdir = os.path.join(self.workdir, 'src', name)
        component_src = vcs.get_vcs_checkout(self.mirrordir, keytype, uri, checkoutdir, branch,
                                             overwrite=not self.args.debug_shell)

        current_vcs_version = meta['revision']

        previous_build_version = run_sync_get_output(['ostree', '--repo=' + self.repo,
                                                      'rev-parse', buildname],
                                                     stderr=open('/dev/null', 'w'),
                                                     none_on_error=True)
        if previous_build_version is not None:
            log("Previous build of '%s' is %s" % (buildname, previous_build_version))

            previous_vcs_version = run_sync_get_output(['ostree', '--repo=' + self.repo,
                                                        'show', '--print-metadata-key=ostbuild-revision',
                                                        previous_build_version])
            previous_vcs_version = previous_vcs_version.strip()

            vcs_version_matches = False
            if previous_vcs_version == current_vcs_version:
                vcs_version_matches = True
                log("VCS version is unchanged from '%s'" % (previous_vcs_version, ))
                if self.buildopts.skip_built:
                    return False
            else:
                log("VCS version is now '%s', was '%s'" % (current_vcs_version, previous_vcs_version))
        else:
            log("No previous build for '%s' found" % (buildname, ))

        artifact_meta = dict(meta)

        metadata_path = os.path.join(component_src, '_ostbuild-meta.json')
        f = open(metadata_path, 'w')
        json.dump(artifact_meta, f)
        f.close()
        
        patches = meta.get('patches')
        if patches is not None:
            for patch in patches:
                patch_path = os.path.join(self.patchdir, patch)
                run_sync(['git', 'am', '--ignore-date', '-3', patch_path], cwd=component_src)
        
        logdir = os.path.join(self.workdir, 'logs', 'compile', name)
        old_logdir = os.path.join(self.workdir, 'old-logs', 'compile', name)
        if not os.path.isdir(logdir):
            os.makedirs(logdir)
        if not os.path.isdir(old_logdir):
            os.makedirs(old_logdir)
        log_path = os.path.join(logdir, '%s.log' % (name, ))
        if os.path.isfile(log_path):
            curtime = int(time.time())
            saved_name = '%s-%d.log' % (name, int(time.time()),)
            os.rename(log_path, os.path.join(old_logdir, saved_name))

        log("Logging to %s" % (log_path, ))
        f = open(log_path, 'w')
        chroot_args = self._get_ostbuild_chroot_args(architecture)
        chroot_args.extend(['--meta=' + metadata_path])
        if self.buildopts.shell_on_failure:
            ecode = run_sync_monitor_log_file(chroot_args, log_path, cwd=component_src, fatal_on_error=False)
            if ecode != 0:
                self._launch_debug_shell(architecture, buildroot_name, cwd=component_src)
        else:
            run_sync_monitor_log_file(chroot_args, log_path, cwd=component_src)

        args = ['ostree', '--repo=' + self.repo,
                'commit', '-b', buildname, '-s', 'Build',
                '--add-metadata-string=ostbuild-name=' + artifact_meta['name'],
                '--add-metadata-string=ostbuild-revision=' + artifact_meta['revision'],
                '--owner-uid=0', '--owner-gid=0', '--no-xattrs', 
                '--skip-if-unchanged']

        setuid_files = artifact_meta.get('setuid', [])
        statoverride_path = None
        if len(setuid_files) > 0:
            (fd, statoverride_path) = tempfile.mkstemp(suffix='.txt', prefix='ostbuild-statoverride-')
            f = os.fdopen(fd, 'w')
            for path in setuid_files:
                f.write('+2048 ' + path)
            f.close()
            args.append('--statoverride=' + statoverride_path)

        component_resultdir = os.path.join(self.workdir, 'results', name)
            
        run_sync(args, cwd=component_resultdir)
        if statoverride_path is not None:
            os.unlink(statoverride_path)
        return True

    def _compose_arch(self, architecture, components):
        runtime_base = buildutil.manifest_base(self.manifest, 'runtime', architecture)
        devel_base = buildutil.manifest_base(self.manifest, 'devel', architecture)
        runtime_contents = [runtime_base + ':/']
        devel_contents = [devel_base + ':/']

        for component in components:
            branch = buildutil.manifest_buildname(self.manifest, component, architecture)
            runtime_contents.append(branch + ':/runtime')
            devel_contents.append(branch + ':/runtime')
            # For now just hardcode docs going in devel
            devel_contents.append(branch + ':/doc')
            devel_contents.append(branch + ':/devel')

        buildutil.compose(self.repo, '%s-%s-%s' % (self.manifest['name'], architecture, 'runtime'),
                          runtime_contents)
        buildutil.compose(self.repo, '%s-%s-%s' % (self.manifest['name'], architecture, 'devel'),
                          devel_contents)
    
    def execute(self, argv):
        parser = argparse.ArgumentParser(description=self.short_description)
        parser.add_argument('--skip-built', action='store_true')
        parser.add_argument('--recompose', action='store_true')
        parser.add_argument('--start-at')
        parser.add_argument('--shell-on-failure', action='store_true')
        parser.add_argument('--debug-shell', action='store_true')
        parser.add_argument('components', nargs='*')

        args = parser.parse_args(argv)
        self.args = args
        
        self.parse_config()

        self.buildopts = BuildOptions()
        self.buildopts.shell_on_failure = args.shell_on_failure
        self.buildopts.skip_built = args.skip_built

        build_manifest_path = os.path.join(self.workdir, 'snapshot.json')
        self.manifest = json.load(open(build_manifest_path))

        self.patchdir = os.path.join(self.workdir, 'patches')

        components = self.manifest['components']
        if args.recompose:
            build_components = []
        elif len(args.components) == 0:
            build_components = components
        else:
            build_components = []
            for name in args.components:
                found = False
                for child in components:
                    if child['name'] == name:
                        found = True
                        build_components.append(child)
                        break
                if not found:
                    fatal("Unknown component %r" % (name, ))

        start_at_index = -1
        if args.start_at is not None:
            if build_components != components:
                fatal("Can't specify --start-at with component list")
            for i,component in enumerate(build_components):
                if component['name'] == args.start_at:
                    start_at_index = i
                    break
            if start_at_index == -1:
                fatal("Unknown component %r specified for --start-at" % (args.start_at, ))
        else:
            start_at_index = 0

        for component in build_components[start_at_index:]:
            index = components.index(component)
            for architecture in self.manifest['architectures']:
                self._build_one_component(component, architecture)

        for architecture in self.manifest['architectures']:
            self._compose_arch(architecture, components)
        
builtins.register(OstbuildBuild)
