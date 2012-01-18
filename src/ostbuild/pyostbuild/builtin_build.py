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

class BuildOptions(object):
    pass

class OstbuildBuild(builtins.Builtin):
    name = "build"
    short_description = "Rebuild all artifacts from the given manifest"

    def __init__(self):
        builtins.Builtin.__init__(self)

    def _mirror_for_url(self, url):
        parsed = urlparse.urlsplit(url)
        return os.path.join(self.mirrordir, 'git', parsed.scheme, parsed.netloc, parsed.path[1:])

    def _fixup_submodule_references(self, cwd):
        submodules_status_text = run_sync_get_output(['git', 'submodule', 'status'], cwd=cwd)
        submodule_status_lines = submodules_status_text.split('\n')
        have_submodules = False
        for line in submodule_status_lines:
            if line == '': continue
            have_submodules = True
            line = line[1:]
            (sub_checksum, sub_name) = line.split(' ', 1)
            sub_url = run_sync_get_output(['git', 'config', '-f', '.gitmodules',
                                           'submodule.%s.url' % (sub_name, )], cwd=cwd)
            mirrordir = self._mirror_for_url(sub_url)
            run_sync(['git', 'config', 'submodule.%s.url' % (sub_name, ), 'file://' + mirrordir], cwd=cwd)
        return have_submodules

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
        git_mirrors_path = os.path.join(self.mirrordir, 'gitconfig')
        f = open(git_mirrors_path)
        git_mirrors = f.read()
        f.close()
        run_sync(['git', 'clone', '-q',
                  '--no-checkout', mirrordir, tmp_dest])
        run_sync(['git', 'checkout', '-q', branch], cwd=tmp_dest)
        run_sync(['git', 'submodule', 'init'], cwd=tmp_dest)
        have_submodules = self._fixup_submodule_references(tmp_dest)
        if have_submodules:
            run_sync(['linux-user-chroot',
                      '--unshare-net', '--chdir', tmp_dest, '/',
                      '/usr/bin/git', 'submodule', 'update'])
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

        if 'branch' not in result:
            result['branch'] = 'master'

        return result

    def _get_target(self, architecture):
        return '%s-%s-devel' % (self.manifest['name'], architecture)

    def _get_base(self, roottype, architecture):
        return 'bases/%s-%s-%s' % (self.manifest['base'],
                                   architecture, roottype)

    def _get_buildname(self, component, architecture):
        return 'artifacts/%s/%s/%s' % (self._get_target (architecture),
                                       component['name'],
                                       component['branch'])

    def _get_buildroot_name(self, component, architecture):
        return 'buildroots/%s/%s/%s' % (self._get_target (architecture),
                                        component['name'],
                                        component['branch'])

    def _compose_buildroot(self, buildroot_name, component, dependencies, architecture):
        base = self._get_base('devel', architecture)
        buildroot_contents = [base + ':/']
        for dep in dependencies:
            dep_buildname = self._get_buildname(dep, architecture)
            buildroot_contents.append(dep_buildname + ':/runtime')
            buildroot_contents.append(dep_buildname + ':/devel')

        return self._compose(buildroot_name, buildroot_contents)

    def _build_one_component(self, meta, dependencies, architecture):
        name = meta['name']
        branch = meta['branch']

        target = self._get_target(architecture)
        buildname = self._get_buildname(meta, architecture)
        buildroot_name = self._get_buildroot_name(meta, architecture)

        (keytype, uri) = self._parse_src_key(meta['src'])

        current_vcs_version = meta['revision']

        previous_build_version = run_sync_get_output(['ostree', '--repo=' + self.repo,
                                                      'rev-parse', buildname],
                                                     stderr=open('/dev/null', 'w'),
                                                     none_on_error=True)
        if previous_build_version is not None:
            log("Previous build of '%s' is %s" % (buildname, previous_build_version))

            previous_vcs_version = run_sync_get_output(['ostree', '--repo=' + self.repo,
                                                        'show', '--print-metadata-key=ostbuild-artifact-version',
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

            
        mirror = self._mirror_for_url(uri)
        component_src = self._get_vcs_checkout(name, keytype, mirror, branch)

        if meta.get('rm-configure', False):
            configure_path = os.path.join(component_src, 'configure')
            if os.path.exists(configure_path):
                os.unlink(configure_path)

        buildroot_version = self._compose_buildroot(buildroot_name, meta, dependencies, architecture)

        artifact_meta = {'buildroot': buildroot_name,
                         'buildroot-version': buildroot_version,
                         'name': name,
                         'branch': branch,
                         'version': current_vcs_version
                         }

        metadata_dir = os.path.join(self.workdir, 'meta')
        if not os.path.isdir(metadata_dir):
            os.makedirs(metadata_dir)
        metadata_path = os.path.join(metadata_dir, '%s-meta.json' % (name, ))
        f = open(metadata_path, 'w')
        json.dump(artifact_meta, f)
        f.close()

        patches = meta.get('patches')
        if patches is not None:
            for patch in patches:
                patch_path = os.path.join(self.patchdir, patch)
                run_sync(['git', 'am', '--ignore-date', '-3', patch_path], cwd=component_src)
        
        component_resultdir = os.path.join(self.workdir, 'results', name)
        if os.path.isdir(component_resultdir):
            shutil.rmtree(component_resultdir)
        os.makedirs(component_resultdir)

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

        if self.args.debug_shell:
            self._launch_debug_shell(architecture, buildroot_name, cwd=component_src)
        else:
            log("Logging to %s" % (log_path, ))
            f = open(log_path, 'w')
            chroot_args = self._get_ostbuild_chroot_args(architecture)
            chroot_args.extend(['--buildroot=' + buildroot_name,
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
                ecode = run_sync_monitor_log_file(chroot_args, log_path, cwd=component_src, fatal_on_error=False)
                if ecode != 0:
                    self._launch_debug_shell(architecture, buildroot_name, cwd=component_src)
            else:
                run_sync_monitor_log_file(chroot_args, log_path, cwd=component_src)

        run_sync(['ostree', '--repo=' + self.repo,
                  'commit', '-b', buildname, '-s', 'Build ' + artifact_meta['version'],
                  '--add-metadata-string=ostbuild-buildroot-version=' + buildroot_version,
                  '--add-metadata-string=ostbuild-artifact-version=' + artifact_meta['version'],
                  '--owner-uid=0', '--owner-gid=0', '--no-xattrs', 
                  '--skip-if-unchanged'],
                 cwd=component_resultdir)
        return True

    def _compose(self, target, artifacts):
        child_args = ['ostree', '--repo=' + self.repo, 'compose',
                      '-b', target, '-s', 'Compose']
        (fd, path) = tempfile.mkstemp(suffix='.txt', prefix='ostbuild-compose-')
        f = os.fdopen(fd, 'w')
        for artifact in artifacts:
            f.write(artifact)
            f.write('\n')
        f.close()
        child_args.extend(['-F', path])
        revision = run_sync_get_output(child_args, log_initiation=True).strip()
        os.unlink(path)
        return revision

    def _compose_arch(self, architecture, components):
        runtime_base = self._get_base('runtime', architecture)
        devel_base = self._get_base('runtime', architecture)
        runtime_contents = [runtime_base + ':/']
        devel_contents = [devel_base + ':/']

        for component in components:
            branch = self._get_buildname(component, architecture)
            runtime_contents.append(branch + ':/runtime')
            devel_contents.append(branch + ':/runtime')
            # For now just hardcode docs going in devel
            devel_contents.append(branch + ':/doc')
            devel_contents.append(branch + ':/devel')

        self._compose('%s-%s-%s' % (self.manifest['name'], architecture, 'runtime'),
                      runtime_contents)
        self._compose('%s-%s-%s' % (self.manifest['name'], architecture, 'devel'),
                      devel_contents)
    
    def execute(self, argv):
        parser = argparse.ArgumentParser(description=self.short_description)
        parser.add_argument('--skip-built', action='store_true')
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

        build_manifest_path = os.path.join(self.workdir, 'manifest.json')
        self.manifest = json.load(open(build_manifest_path))

        self.patchdir = os.path.join(self.workdir, 'patches')

        components = self.manifest['components']
        if len(args.components) == 0:
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
            dependencies = components[:index]
            for architecture in self.manifest['architectures']:
                self._build_one_component(component, dependencies, architecture)

        for architecture in self.manifest['architectures']:
            self._compose_arch(architecture, components)
        
builtins.register(OstbuildBuild)
