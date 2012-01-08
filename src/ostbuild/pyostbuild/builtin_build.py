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
        mirror = os.path.join(self.mirrordir, name)
        tmp_mirror = mirror + '.tmp'
        if os.path.isdir(tmp_mirror):
            shutil.rmtree(tmp_mirror)
        if not os.path.isdir(mirror):
            run_sync(['git', 'clone', '--mirror', uri, tmp_mirror])
            os.rename(tmp_mirror, mirror)
        return mirror

    def _get_vcs_checkout(self, name, keytype, mirrordir, branch):
        checkoutdir = os.path.join(self.workdir, 'src')
        if not os.path.isdir(checkoutdir):
            os.makedirs(checkoutdir)
        dest = os.path.join(checkoutdir, name)
        tmp_dest = dest + '.tmp'
        if os.path.isdir(dest):
            shutil.rmtree(dest)
        if os.path.isdir(tmp_dest):
            shutil.rmtree(tmp_dest)
        subprocess.check_call(['git', 'clone', '-q', mirrordir, tmp_dest])
        subprocess.check_call(['git', 'checkout', '-q', branch], cwd=tmp_dest)
        subprocess.check_call(['git', 'submodule', 'update', '--init'], cwd=tmp_dest)
        os.rename(tmp_dest, dest)
        return dest

    def _parse_src_key(self, srckey):
        idx = srckey.find(':')
        if idx < 0:
            raise ValueError("Invalid SRC uri=%s" % (srckey, ))
        keytype = srckey[:idx]
        if keytype not in ['git']:
            raise ValueError("Unsupported SRC uri=%s" % (srckey, ))
        uri = srckey[idx+1:]
        return (keytype, uri)

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
        return result

    def _build_one_component(self, meta, architecture):
        name = meta['name']

        (keytype, uri) = self._parse_src_key(meta['src'])
        branch = meta.get('branch', 'master')

        buildroot = '%s-%s-devel' % (self.manifest['name'], architecture)
        runtime_branchname = 'artifacts/%s/%s/%s/runtime' % (buildroot, name, branch)
        current_buildroot_version = run_sync_get_output(['ostree', '--repo=' + self.repo,
                                                         'rev-parse', buildroot])
        current_buildroot_version = current_buildroot_version.strip()

        artifact_base = {'buildroot': buildroot,
                         'buildroot-version': current_buildroot_version,
                         'name': name,
                         'branch': branch,
                         }

        component_vcs_mirror = self._ensure_vcs_mirror(name, keytype, uri, branch)
        component_src = self._get_vcs_checkout(name, keytype, component_vcs_mirror, branch)

        current_vcs_version = buildutil.get_git_version_describe(component_src)
        artifact_base['version'] = current_vcs_version

        metadata_dir = os.path.join(self.workdir, 'meta')
        if not os.path.isdir(metadata_dir):
            os.makedirs(metadata_dir)
        metadata_path = os.path.join(metadata_dir, '%s-meta.json' % (name, ))
        f = open(metadata_path, 'w')
        json.dump(artifact_base, f)
        f.close()

        previous_commit_version = run_sync_get_output(['ostree', '--repo=' + self.repo,
                                                       'rev-parse', runtime_branchname],
                                                      stderr=open('/dev/null', 'w'),
                                                      none_on_error=True)
        if previous_commit_version is not None:
            log("Previous build of '%s' is %s" % (runtime_branchname, previous_commit_version))

            previous_artifact_version = run_sync_get_output(['ostree', '--repo=' + self.repo,
                                                             'show', '--print-metadata-key=ostbuild-artifact-version', previous_commit_version])
            previous_artifact_version = previous_artifact_version.strip()
            previous_buildroot_version = run_sync_get_output(['ostree', '--repo=' + self.repo,
                                                              'show', '--print-metadata-key=ostbuild-buildroot-version', previous_commit_version])
            previous_buildroot_version = previous_buildroot_version.strip()

            previous_artifact_base = dict(artifact_base)
            previous_artifact_base['version'] = previous_artifact_version
            previous_artifact_base['buildroot-version'] = previous_buildroot_version

            previous_artifact_runtime = dict(previous_artifact_base)
            previous_artifact_runtime['type'] = 'runtime'
            previous_artifact_devel = dict(previous_artifact_base)
            previous_artifact_devel['type'] = 'devel'
            previous_artifacts = [previous_artifact_runtime,
                                  previous_artifact_devel]
                
            vcs_version_matches = False
            if previous_artifact_version == current_vcs_version:
                vcs_version_matches = True
                log("VCS version is unchanged from '%s'" % (previous_artifact_version, ))
                if self.buildopts.skip_built:
                    return previous_artifacts
            else:
                log("VCS version is now '%s', was '%s'" % (current_vcs_version, previous_artifact_version))
            buildroot_version_matches = False
            if vcs_version_matches:    
                buildroot_version_matches = (current_buildroot_version == previous_buildroot_version)
                if buildroot_version_matches:
                    log("Already have build '%s' of src commit '%s' for '%s' in buildroot '%s'" % (previous_commit_version, previous_artifact_version, runtime_branchname, buildroot))
                    return previous_artifacts
                else:
                    log("Buildroot is now '%s'" % (current_buildroot_version, ))
        else:
            log("No previous build for '%s' found" % (runtime_branchname, ))

        patches = meta.get('patches')
        if patches is not None:
            for patch in patches:
                patch_path = os.path.join(self.manifestdir, patch)
                run_sync(['git', 'am', '--ignore-date', '-3', patch_path], cwd=component_src)
        
        component_resultdir = os.path.join(self.workdir, 'results', name)
        if os.path.isdir(component_resultdir):
            shutil.rmtree(component_resultdir)
        os.makedirs(component_resultdir)

        chroot_args = self._get_ostbuild_chroot_args(architecture)
        chroot_args.extend(['--buildroot=' + buildroot,
                            '--workdir=' + self.workdir,
                            '--resultdir=' + component_resultdir,
                            '--meta=' + metadata_path])
        global_config_opts = self.manifest.get('config-opts')
        if global_config_opts is not None:
            chroot_args.extend(global_config_opts)
        component_config_opts = meta.get('config-opts')
        if component_config_opts is not None:
            chroot_args.extend(component_config_opts)
        if self.buildopts.shell_on_failure:
            ecode = run_sync(chroot_args, cwd=component_src, fatal_on_error=False)
            if ecode != 0:
                self._launch_debug_shell(architecture, buildroot, cwd=component_src)
        else:
            run_sync(chroot_args, cwd=component_src, fatal_on_error=True)

        artifacts = []
        for artifact_type in ['runtime', 'devel']:
            artifact = dict(artifact_base)
            artifacts.append(artifact)
            artifact['type'] = artifact_type

            artifact_branch = buildutil.branch_name_for_artifact(artifact)

            artifact_resultdir = os.path.join(component_resultdir, artifact_branch)
                                              
            run_sync(['ostree', '--repo=' + self.repo,
                      'commit', '-b', artifact_branch, '-s', 'Build ' + artifact_base['version'],
                     '--add-metadata-string=ostbuild-buildroot-version=' + current_buildroot_version,
                     '--add-metadata-string=ostbuild-artifact-version=' + artifact_base['version'],
                     '--owner-uid=0', '--owner-gid=0', '--no-xattrs', 
                     '--skip-if-unchanged'],
                     cwd=artifact_resultdir)
        return artifacts

    def _compose(self, suffix, artifacts):
        child_args = ['ostree', '--repo=' + self.repo, 'compose', '--recompose',
                      '-b', self.manifest['name'] + '-' + suffix, '-s', 'Compose']
        child_args.extend(artifacts)
        run_sync(child_args)
    
    def execute(self, argv):
        parser = argparse.ArgumentParser(description=self.short_description)
        parser.add_argument('--manifest', required=True)
        parser.add_argument('--skip-built', action='store_true')
        parser.add_argument('--start-at')
        parser.add_argument('--shell-on-failure', action='store_true')
        parser.add_argument('--debug-shell', action='store_true')
        parser.add_argument('components', nargs='*')

        args = parser.parse_args(argv)
        
        self.parse_config()

        self.buildopts = BuildOptions()
        self.buildopts.shell_on_failure = args.shell_on_failure
        self.buildopts.skip_built = args.skip_built

        self.manifest = json.load(open(args.manifest))

        if args.debug_shell:
            debug_shell_arch = self.manifest['architectures'][0]
            debug_shell_buildroot = '%s-%s-devel' % (self.manifest['name'], debug_shell_arch)
            self._launch_debug_shell(debug_shell_arch, debug_shell_buildroot)

        self.manifestdir = os.path.dirname(args.manifest)

        self.resolved_components = map(self._resolve_component_meta, self.manifest['components'])

        if len(args.components) == 0:
            build_components = self.resolved_components
        else:
            build_components = []
            for name in args.components:
                found = False
                for child in self.resolved_components:
                    if child['name'] == name:
                        found = True
                        build_components.append(child)
                        break
                if not found:
                    fatal("Unknown component %r" % (name, ))

        start_at_index = -1
        if args.start_at is not None:
            for i,component in enumerate(build_components):
                if component['name'] == args.start_at:
                    start_at_index = i
                    break
            if start_at_index == -1:
                fatal("Unknown component %r specified for --start-at" % (args.start_at, ))
        else:
            start_at_index = 0
            
        for component in build_components[start_at_index:]:
            for architecture in self.manifest['architectures']:
                (runtime_artifact,devel_artifact) = self._build_one_component(component, architecture)
                runtime_branch = buildutil.branch_name_for_artifact(runtime_artifact)
                devel_branch = buildutil.branch_name_for_artifact(devel_artifact)
    
                target_component = component.get('component')
                if target_component != 'devel':
                    self._compose(architecture + '-runtime', [runtime_branch])
                self._compose(architecture + '-devel', [runtime_branch, devel_branch])
        
builtins.register(OstbuildBuild)
