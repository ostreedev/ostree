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

from . import builtins
from .ostbuildlog import log, fatal
from .subprocess_helpers import run_sync, run_sync_get_output
from . import ostbuildrc
from . import buildutil
from . import kvfile

class BuildOptions(object):
    pass

class OstbuildBuild(builtins.Builtin):
    name = "build"
    short_description = "Rebuild all artifacts from the given manifest"

    def __init__(self):
        builtins.Builtin.__init__(self)

    def _ensure_vcs_mirror(self, name, keytype, uri, branch):
        assert keytype == 'git'
        mirror = os.path.join(self.srcdir, name)
        tmp_mirror = mirror + '.tmp'
        if os.path.isdir(tmp_mirror):
            shutil.rmtree(tmp_mirror)
        if not os.path.isdir(mirror):
            run_sync(['git', 'clone', '--mirror', uri, tmp_mirror])
            os.rename(tmp_mirror, mirror)
        return mirror

    def _get_vcs_checkout(self, name, keytype, mirrordir, branch):
        checkoutdir = os.path.join(self.srcdir, '_checkouts')
        if not os.path.isdir(checkoutdir):
            os.makedirs(checkoutdir)
        dest = os.path.join(checkoutdir, name)
        tmp_dest = dest + '.tmp'
        if os.path.isdir(dest):
            shutil.rmtree(dest)
        if os.path.isdir(tmp_dest):
            shutil.rmtree(tmp_dest)
        subprocess.check_call(['git', 'clone', '--depth=1', '-q', mirrordir, tmp_dest])
        subprocess.check_call(['git', 'checkout', '-q', branch], cwd=tmp_dest)
        subprocess.check_call(['git', 'submodule', 'update', '--init'], cwd=tmp_dest)
        os.rename(tmp_dest, dest)
        return dest

    def _get_vcs_version_from_checkout(self, name):
        vcsdir = os.path.join(self.srcdir, name)
        return subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=vcsdir)

    def _parse_src_key(self, srckey):
        idx = srckey.find(':')
        if idx < 0:
            raise ValueError("Invalid SRC uri=%s" % (srckey, ))
        keytype = srckey[:idx]
        if keytype not in ('git'):
            raise ValueError("Unsupported SRC uri=%s" % (srckey, ))
        uri = srckey[idx+1:]
        idx = uri.rfind('#')
        if idx < 0:
            branch = "master"
        else:
            branch = uri[idx+1:]
            uri = uri[0:idx]
        return (keytype, uri, branch)

    def _parse_artifact_vcs_version(self, ver):
        idx = ver.rfind('-')
        if idx > 0:
            vcs_ver = ver[idx+1:]
        else:
            vcs_ver = ver
        if not vcs_ver.startswith('g'):
            raise ValueError("Invalid artifact version '%s'" % (ver, ))
        return vcs_ver[1:]

    def _get_ostbuild_chroot_args(self, architecture):
        current_machine = os.uname()[4]
        if current_machine != architecture:
            args = ['setarch', architecture]
        else:
            args = []
        args.extend(['ostbuild', 'chroot-compile-one',
                     '--repo=' + self.repo])
        return args

    def _launch_debug_shell(self, architecture, buildroot, cwd=None):
        args = self._get_ostbuild_chroot_args(architecture)
        args.extend(['--buildroot=' + buildroot,
                     '--workdir=' + self.workdir,
                     '--debug-shell'])
        run_sync(args, cwd=cwd, fatal_on_error=False, keep_stdin=True)
        fatal("Exiting after debug shell")

    def _build_one_component(self, name, architecture, meta):
        (keytype, uri, branch) = self._parse_src_key(meta['SRC'])
        component_vcs_mirror = self._ensure_vcs_mirror(name, keytype, uri, branch)
        component_src = self._get_vcs_checkout(name, keytype, component_vcs_mirror, branch)
        buildroot = '%s-%s-devel' % (self.manifest['name'], architecture)
        branchname = 'artifacts/%s/%s/%s' % (buildroot, name, branch)
        current_buildroot_version = run_sync_get_output(['ostree', '--repo=' + self.repo,
                                                         'rev-parse', buildroot])
        current_buildroot_version = current_buildroot_version.strip()
        previous_commit_version = run_sync_get_output(['ostree', '--repo=' + self.repo,
                                                       'rev-parse', branchname],
                                                      stderr=open('/dev/null', 'w'),
                                                      none_on_error=True)
        if previous_commit_version is not None:
            log("Previous build of '%s' is %s" % (branchname, previous_commit_version))
            previous_artifact_version = run_sync_get_output(['ostree', '--repo=' + self.repo,
                                                             'show', '--print-metadata-key=ostbuild-artifact-version', previous_commit_version])
            previous_artifact_version = previous_artifact_version.strip()
            previous_buildroot_version = run_sync_get_output(['ostree', '--repo=' + self.repo,
                                                              'show', '--print-metadata-key=ostbuild-buildroot-version', previous_commit_version])
            previous_buildroot_version = previous_buildroot_version.strip()
            
            previous_vcs_version = self._parse_artifact_vcs_version(previous_artifact_version)
            current_vcs_version = self._get_vcs_version_from_checkout(name)
            vcs_version_matches = False
            if previous_vcs_version == current_vcs_version:
                vcs_version_matches = True
                log("VCS version is unchanged from '%s'" % (previous_vcs_version, ))
            else:
                log("VCS version is now '%s', was '%s'" % (current_vcs_version, previous_vcs_version))
            buildroot_version_matches = False
            if vcs_version_matches:    
                buildroot_version_matches = (current_buildroot_version == previous_buildroot_version)
                if buildroot_version_matches:
                    log("Already have build '%s' of src commit '%s' for '%s' in buildroot '%s'" % (previous_commit_version, previous_vcs_version, branchname, buildroot))
                    return
                else:
                    log("Buildroot is now '%s'" % (current_buildroot_version, ))
        else:
            log("No previous build for '%s' found" % (branchname, ))
        
        component_resultdir = os.path.join(self.workdir, name, 'results')
        if os.path.isdir(component_resultdir):
            shutil.rmtree(component_resultdir)
        os.makedirs(component_resultdir)

        chroot_args = self._get_ostbuild_chroot_args(architecture)
        chroot_args.extend(['--buildroot=' + buildroot,
                            '--workdir=' + self.workdir,
                            '--resultdir=' + component_resultdir])
        if self.buildopts.shell_on_failure:
            ecode = run_sync(chroot_args, cwd=component_src, fatal_on_error=False)
            if ecode != 0:
                self._launch_debug_shell(architecture, buildroot, cwd=component_src)
        else:
            run_sync(chroot_args, cwd=component_src, fatal_on_error=True)
        artifact_files = []
        for name in os.listdir(component_resultdir):
            if name.startswith('artifact-'):
                log("Generated artifact file: %s" % (name, ))
                artifact_files.append(os.path.join(component_resultdir, name))
        assert len(artifact_files) >= 1 and len(artifact_files) <= 2
        run_sync(['ostbuild', 'commit-artifacts',
                  '--repo=' + self.repo] + artifact_files)
        artifacts = []
        for filename in artifact_files:
            parsed = buildutil.parse_artifact_name(os.path.basename(filename))
            artifacts.append(parsed)
        def _sort_artifact(a, b):
            if a['type'] == b['type']:
                return 0
            elif a['type'] == 'runtime':
                return -1
            return 1
        artifacts.sort(_sort_artifact)
        return artifacts

    def _compose(self, suffix, artifacts):
        compose_contents = ['bases/' + self.manifest['base'] + '-' + suffix]
        compose_contents.extend(artifacts)
        child_args = ['ostree', '--repo=' + self.repo, 'compose',
                      '-b', self.manifest['name'] + '-' + suffix, '-s', 'Compose']
        child_args.extend(compose_contents)
        run_sync(child_args)
    
    def execute(self, argv):
        parser = argparse.ArgumentParser(description=self.short_description)
        parser.add_argument('--manifest', required=True)
        parser.add_argument('--start-at')
        parser.add_argument('--shell-on-failure', action='store_true')
        parser.add_argument('--debug-shell', action='store_true')

        args = parser.parse_args(argv)
        
        self.parse_config()

        self.buildopts = BuildOptions()
        self.buildopts.shell_on_failure = args.shell_on_failure

        self.manifest = json.load(open(args.manifest))

        if args.debug_shell:
            debug_shell_arch = self.manifest['architectures'][0]
            debug_shell_buildroot = '%s-%s-devel' % (self.manifest['name'], debug_shell_arch)
            self._launch_debug_shell(debug_shell_arch, debug_shell_buildroot)

        dirname = os.path.dirname(args.manifest)
        components = self.manifest['components']
        runtime_components = []
        devel_components = []
        runtime_artifacts = []
        devel_artifacts = []
        if args.start_at:
            start_at_index = -1 
            for i,component_name in enumerate(components):
                if component_name == args.start_at:
                    start_at_index = i
                    break
            if start_at_index == -1:
                fatal("Unknown component '%s' for --start-at" % (args.start_at, ))
        else:
            start_at_index = 0
            
        for component_name in components[start_at_index:]:
            for architecture in self.manifest['architectures']:
                path = os.path.join(dirname, component_name + '.txt')
                f = open(path)
                component_meta = kvfile.parse(f)
    
                artifact_branches = self._build_one_component(component_name, architecture, component_meta)
    
                target_component = component_meta.get('COMPONENT')
                if target_component == 'devel':
                    devel_components.append(component_name)
                else:
                    runtime_components.append(component_name)
                    for branch in artifact_branches:
                        if branch['type'] == 'runtime':
                            runtime_artifacts.append(branch)
                devel_artifacts.extend(artifact_branches)

                f.close()

                devel_branches = map(buildutil.branch_name_for_artifact, devel_artifacts)
                self._compose(architecture + '-devel', devel_branches)
                runtime_branches = map(buildutil.branch_name_for_artifact, runtime_artifacts)
                self._compose(architecture + '-runtime', runtime_branches)
        
builtins.register(OstbuildBuild)
