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
import hashlib
import json
from StringIO import StringIO

from . import builtins
from .ostbuildlog import log, fatal
from .subprocess_helpers import run_sync, run_sync_get_output
from .subprocess_helpers import run_sync_monitor_log_file
from . import ostbuildrc
from . import buildutil
from . import fileutil
from . import kvfile
from . import odict
from . import vcs

class BuildOptions(object):
    pass

class OstbuildBuild(builtins.Builtin):
    name = "build"
    short_description = "Build multiple components and generate trees"

    def __init__(self):
        builtins.Builtin.__init__(self)

    def _get_ostbuild_chroot_args(self, architecture, component, component_resultdir):
        basename = component['name']
        current_machine = os.uname()[4]
        if current_machine != architecture:
            args = ['setarch', architecture]
        else:
            args = []
        args.extend(['ostbuild', 'chroot-compile-one',
                     '--snapshot=' + self.snapshot_path,
                     '--name=' + basename, '--arch=' + architecture,
                     '--resultdir=' + component_resultdir])
        return args

    def _launch_debug_shell(self, architecture, component, component_resultdir, cwd=None):
        args = self._get_ostbuild_chroot_args(architecture, component, component_resultdir)
        args.append('--debug-shell')
        run_sync(args, cwd=cwd, fatal_on_error=False, keep_stdin=True)
        fatal("Exiting after debug shell")

    def _build_one_component(self, component, architecture):
        basename = component['name']

        buildname = '%s/%s/%s' % (self.snapshot['prefix'], basename, architecture)
        build_ref = 'components/%s' % (buildname, )

        current_vcs_version = component.get('revision')

        expanded_component = self.expand_component(component)

        # TODO - deduplicate this with chroot_compile_one
        current_meta_io = StringIO()
        json.dump(expanded_component, current_meta_io, indent=4, sort_keys=True)
        current_metadata_text = current_meta_io.getvalue()
        sha = hashlib.sha256()
        sha.update(current_metadata_text)
        current_meta_digest = sha.hexdigest()

        if (self.buildopts.force_rebuild or
            basename in self.force_build_components):
            previous_build_version = None
        else:
            previous_build_version = run_sync_get_output(['ostree', '--repo=' + self.repo,
                                                          'rev-parse', build_ref],
                                                         stderr=open('/dev/null', 'w'),
                                                         none_on_error=True)
        if (current_vcs_version is not None
            and previous_build_version is not None):
            log("Previous build of '%s' is %s" % (buildname, previous_build_version))

            previous_metadata_text = run_sync_get_output(['ostree', '--repo=' + self.repo,
                                                          'cat', previous_build_version,
                                                          '/_ostbuild-meta.json'],
                                                         log_initiation=True)
            sha = hashlib.sha256()
            sha.update(previous_metadata_text)
            previous_meta_digest = sha.hexdigest()

            if current_meta_digest == previous_meta_digest:
                log("Metadata is unchanged from previous")
                return previous_build_version
            else:
                previous_metadata = json.loads(previous_metadata_text)
                previous_vcs_version = previous_metadata.get('revision')
                if current_vcs_version == previous_vcs_version:
                    log("Metadata differs; VCS version unchanged")
                    if self.buildopts.skip_vcs_matches:
                        return previous_build_version
                    for k,v in expanded_component.iteritems():
                        previous_v = previous_metadata.get(k)
                        if v != previous_v:
                            log("Key %r differs: old: %r new: %r" % (k, previous_v, v))
                else:
                    log("Metadata differs; note vcs version is now '%s', was '%s'" % (current_vcs_version, previous_vcs_version))
        else:
            log("No previous build for '%s' found" % (buildname, ))

        checkoutdir = os.path.join(self.workdir, 'checkouts')
        component_src = os.path.join(checkoutdir, buildname)
        fileutil.ensure_parent_dir(component_src)
        child_args = ['ostbuild', 'checkout', '--snapshot=' + self.snapshot_path,
                      '--checkoutdir=' + component_src,
                      '--clean', '--overwrite', basename]
        if self.args.patches_path:
            child_args.append('--patches-path=' + self.args.patches_path)
        run_sync(child_args)

        artifact_meta = dict(component)

        logdir = os.path.join(self.workdir, 'logs', buildname)
        fileutil.ensure_dir(logdir)
        log_path = os.path.join(logdir, 'compile.log')
        if os.path.isfile(log_path):
            curtime = int(time.time())
            saved_name = os.path.join(logdir, 'compile-prev.log')
            os.rename(log_path, saved_name)

        component_resultdir = os.path.join(self.workdir, 'results', buildname)
        if os.path.isdir(component_resultdir):
            shutil.rmtree(component_resultdir)
        fileutil.ensure_dir(component_resultdir)

        log("Logging to %s" % (log_path, ))
        f = open(log_path, 'w')
        chroot_args = self._get_ostbuild_chroot_args(architecture, component, component_resultdir)
        if self.buildopts.shell_on_failure:
            ecode = run_sync_monitor_log_file(chroot_args, log_path, cwd=component_src, fatal_on_error=False)
            if ecode != 0:
                self._launch_debug_shell(architecture, component, component_resultdir, cwd=component_src)
        else:
            run_sync_monitor_log_file(chroot_args, log_path, cwd=component_src)

        args = ['ostree', '--repo=' + self.repo,
                'commit', '-b', build_ref, '-s', 'Build',
                '--owner-uid=0', '--owner-gid=0', '--no-xattrs', 
                '--skip-if-unchanged']

        setuid_files = artifact_meta.get('setuid', [])
        statoverride_path = None
        if len(setuid_files) > 0:
            (fd, statoverride_path) = tempfile.mkstemp(suffix='.txt', prefix='ostbuild-statoverride-')
            f = os.fdopen(fd, 'w')
            for path in setuid_files:
                f.write('+2048 ' + path)
                f.write('\n')
            f.close()
            args.append('--statoverride=' + statoverride_path)

        run_sync(args, cwd=component_resultdir)
        if statoverride_path is not None:
            os.unlink(statoverride_path)

        if os.path.islink(component_src):
            os.unlink(component_src)
        else:
            shutil.rmtree(component_src)
        shutil.rmtree(component_resultdir)

        return run_sync_get_output(['ostree', '--repo=' + self.repo,
                                    'rev-parse', build_ref])

    def _compose_one_target(self, target, component_build_revs):
        base = target['base']
        base_name = 'bases/%s' % (base['name'], )
        runtime_name = 'bases/%s' % (base['runtime'], )
        devel_name = 'bases/%s' % (base['devel'], )

        compose_rootdir = os.path.join(self.workdir, 'roots', target['name'])
        fileutil.ensure_parent_dir(compose_rootdir)
        if os.path.isdir(compose_rootdir):
            shutil.rmtree(compose_rootdir)
        os.mkdir(compose_rootdir)

        related_refs = {}

        base_revision = run_sync_get_output(['ostree', '--repo=' + self.repo,
                                             'rev-parse', base_name])

        runtime_revision = run_sync_get_output(['ostree', '--repo=' + self.repo,
                                                'rev-parse', runtime_name])
        related_refs[runtime_name] = runtime_revision
        devel_revision = run_sync_get_output(['ostree', '--repo=' + self.repo,
                                              'rev-parse', devel_name])
        related_refs[devel_name] = devel_revision

        for name,rev in component_build_revs.iteritems():
            build_ref = 'components/%s/%s' % (self.snapshot['prefix'], name)
            related_refs[build_ref] = rev

        (related_fd, related_tmppath) = tempfile.mkstemp(suffix='.txt', prefix='ostbuild-compose-')
        related_f = os.fdopen(related_fd, 'w')
        for (name, rev) in related_refs.iteritems():
            related_f.write(name) 
            related_f.write(' ') 
            related_f.write(rev) 
            related_f.write('\n') 
        related_f.close()

        compose_contents = [(base_revision, '/')]
        for tree_content in target['contents']:
            name = tree_content['name']
            rev = component_build_revs[name]
            subtrees = tree_content['trees']
            for subpath in subtrees:
                compose_contents.append((rev, subpath))

        (contents_fd, contents_tmppath) = tempfile.mkstemp(suffix='.txt', prefix='ostbuild-compose-')
        contents_f = os.fdopen(contents_fd, 'w')
        for (branch, subpath) in compose_contents:
            contents_f.write(branch)
            contents_f.write('\0')
            contents_f.write(subpath)
            contents_f.write('\0')
        contents_f.close()

        run_sync(['ostree', '--repo=' + self.repo,
                  'checkout', '--user-mode', '--no-triggers', '--union', 
                  '--from-file=' + contents_tmppath, compose_rootdir])
        os.unlink(contents_tmppath)

        contents_path = os.path.join(compose_rootdir, 'contents.json')
        f = open(contents_path, 'w')
        json.dump(self.snapshot, f, indent=4, sort_keys=True)
        f.close()

        treename = 'trees/%s' % (target['name'], )
        
        child_args = ['ostree', '--repo=' + self.repo,
                      'commit', '-b', treename, '-s', 'Compose',
                      '--owner-uid=0', '--owner-gid=0', '--no-xattrs', 
                      '--related-objects-file=' + related_tmppath,
                      ]
        if not self.buildopts.no_skip_if_unchanged:
            child_args.append('--skip-if-unchanged')
        run_sync(child_args, cwd=compose_rootdir)
        os.unlink(related_tmppath)
        shutil.rmtree(compose_rootdir)

    def execute(self, argv):
        parser = argparse.ArgumentParser(description=self.short_description)
        parser.add_argument('--prefix')
        parser.add_argument('--src-snapshot')
        parser.add_argument('--patches-path')
        parser.add_argument('--force-rebuild', action='store_true')
        parser.add_argument('--skip-vcs-matches', action='store_true')
        parser.add_argument('--no-compose', action='store_true')
        parser.add_argument('--no-skip-if-unchanged', action='store_true')
        parser.add_argument('--compose-only', action='store_true')
        parser.add_argument('--shell-on-failure', action='store_true')
        parser.add_argument('--debug-shell', action='store_true')
        parser.add_argument('components', nargs='*')
        
        args = parser.parse_args(argv)
        self.args = args
        
        self.parse_config()
        self.parse_snapshot(args.prefix, args.src_snapshot)

        log("Using source snapshot: %s" % (os.path.basename(self.snapshot_path), ))

        self.buildopts = BuildOptions()
        self.buildopts.shell_on_failure = args.shell_on_failure
        self.buildopts.force_rebuild = args.force_rebuild
        self.buildopts.skip_vcs_matches = args.skip_vcs_matches
        self.buildopts.no_skip_if_unchanged = args.no_skip_if_unchanged

        self.force_build_components = set()

        components = self.snapshot['components']

        prefix = self.snapshot['prefix']
        base_prefix = '%s/%s' % (self.snapshot['base']['name'], prefix)

        architectures = self.snapshot['architectures']

        component_to_arches = {}

        runtime_components = []
        devel_components = []

        for component in components:
            name = component['name']

            is_runtime = component.get('component', 'runtime') == 'runtime'

            if is_runtime:
                runtime_components.append(component)
            devel_components.append(component)

            is_noarch = component.get('noarch', False)
            if is_noarch:
                # Just use the first specified architecture
                component_arches = [architectures[0]]
            else:
                component_arches = component.get('architectures', architectures)
            component_to_arches[name] = component_arches

        for name in args.components:
            component = self.get_component(name)
            self.force_build_components.add(component['name'])

        components_to_build = []
        component_skipped_count = 0

        component_build_revs = {}

        if not args.compose_only:
            for component in components:
                for architecture in architectures:
                    components_to_build.append((component, architecture))

            log("%d components to build" % (len(components_to_build), ))
            for (component, architecture) in components_to_build:
                archname = '%s/%s' % (component['name'], architecture)
                build_rev = self._build_one_component(component, architecture)
                component_build_revs[archname] = build_rev

        targets_list = []
        for target_component_type in ['runtime', 'devel']:
            for architecture in architectures:
                target = {}
                targets_list.append(target)
                target['name'] = '%s-%s-%s' % (prefix, architecture, target_component_type)

                runtime_ref = '%s-%s-runtime' % (base_prefix, architecture)
                buildroot_ref = '%s-%s-devel' % (base_prefix, architecture)
                if target_component_type == 'runtime':
                    base_ref = runtime_ref
                else:
                    base_ref = buildroot_ref
                target['base'] = {'name': base_ref,
                                  'runtime': runtime_ref,
                                  'devel': buildroot_ref}

                if target_component_type == 'runtime':
                    target_components = runtime_components
                else:
                    target_components = devel_components
                    
                contents = []
                for component in target_components:
                    builds_for_component = component_to_arches[component['name']]
                    if architecture not in builds_for_component:
                        continue
                    binary_name = '%s/%s' % (component['name'], architecture)
                    component_ref = {'name': binary_name}
                    if target_component_type == 'runtime':
                        component_ref['trees'] = ['/runtime']
                    else:
                        component_ref['trees'] = ['/runtime', '/devel', '/doc']
                    contents.append(component_ref)
                target['contents'] = contents

        for target in targets_list:
            self._compose_one_target(target, component_build_revs)

builtins.register(OstbuildBuild)
