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

class OstbuildBuildComponents(builtins.Builtin):
    name = "build-components"
    short_description = "Build multiple components from given source snapshot"

    def __init__(self):
        builtins.Builtin.__init__(self)

    def _get_ostbuild_chroot_args(self, architecture):
        current_machine = os.uname()[4]
        if current_machine != architecture:
            args = ['setarch', architecture]
        else:
            args = []
        args.extend(['ostbuild', 'chroot-compile-one',
                     '--snapshot=' + self.snapshot_path])
        return args

    def _launch_debug_shell(self, architecture, component, cwd=None):
        args = self._get_ostbuild_chroot_args(architecture)
        args.extend(['--arch=' + architecture,
                     '--name=' + component,
                     '--debug-shell'])
        run_sync(args, cwd=cwd, fatal_on_error=False, keep_stdin=True)
        fatal("Exiting after debug shell")

    def _build_one_component(self, basename, component, architecture):
        branch = component['branch']

        name = '%s/%s' % (basename, architecture)
        buildname = 'components/%s' % (name, )

        current_vcs_version = component['revision']

        # TODO - deduplicate this with chroot_compile_one
        current_meta_io = StringIO()
        meta_copy = dict(component)
        meta_copy['name'] = basename  # Note we have to match the name here
        json.dump(meta_copy, current_meta_io, indent=4, sort_keys=True)
        current_metadata_text = current_meta_io.getvalue()
        sha = hashlib.sha256()
        sha.update(current_metadata_text)
        current_meta_digest = sha.hexdigest()

        previous_build_version = run_sync_get_output(['ostree', '--repo=' + self.repo,
                                                      'rev-parse', buildname],
                                                     stderr=open('/dev/null', 'w'),
                                                     none_on_error=True)
        if previous_build_version is not None:
            log("Previous build of '%s' is %s" % (name, previous_build_version))

            previous_metadata_text = run_sync_get_output(['ostree', '--repo=' + self.repo,
                                                          'cat', previous_build_version,
                                                          '/_ostbuild-meta.json'],
                                                         log_initiation=True)
            sha = hashlib.sha256()
            sha.update(previous_metadata_text)
            previous_meta_digest = sha.hexdigest()

            if current_meta_digest == previous_meta_digest:
                log("Metadata is unchanged from previous")
                if self.buildopts.skip_built:
                    return False
            else:
                current_vcs_version = component['revision']
                previous_metadata = json.loads(previous_metadata_text)
                previous_vcs_version = previous_metadata['revision']
                if current_vcs_version == previous_vcs_version:
                    log("Metadata differs; VCS version unchanged")
                    for k,v in meta_copy.iteritems():
                        previous_v = previous_metadata.get(k)
                        if v != previous_v:
                            log("Key %r differs: old: %r new: %r" % (k, previous_v, v))
                else:
                    log("Metadata differs; note vcs version is now '%s', was '%s'" % (current_vcs_version, previous_vcs_version))
        else:
            log("No previous build for '%s' found" % (name, ))

        checkoutdir = os.path.join(self.workdir, 'checkouts')
        fileutil.ensure_dir(checkoutdir)
        component_src = os.path.join(checkoutdir, basename)
        run_sync(['ostbuild', 'checkout', '--snapshot=' + self.snapshot_path,
                  '--clean', '--overwrite', basename], cwd=checkoutdir)

        artifact_meta = dict(component)

        logdir = os.path.join(self.workdir, 'logs', name)
        fileutil.ensure_dir(logdir)
        log_path = os.path.join(logdir, 'compile.log')
        if os.path.isfile(log_path):
            curtime = int(time.time())
            saved_name = os.path.join(logdir, 'compile-prev.log')
            os.rename(log_path, saved_name)

        log("Logging to %s" % (log_path, ))
        f = open(log_path, 'w')
        chroot_args = self._get_ostbuild_chroot_args(architecture)
        chroot_args.extend(['--pristine', '--name=' + basename, '--arch=' + architecture])
        if self.buildopts.shell_on_failure:
            ecode = run_sync_monitor_log_file(chroot_args, log_path, cwd=component_src, fatal_on_error=False)
            if ecode != 0:
                self._launch_debug_shell(architecture, basename, cwd=component_src)
        else:
            run_sync_monitor_log_file(chroot_args, log_path, cwd=component_src)

        args = ['ostree', '--repo=' + self.repo,
                'commit', '-b', buildname, '-s', 'Build',
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

    def _resolve_refs(self, refs):
        args = ['ostree', '--repo=' + self.repo, 'rev-parse']
        args.extend(refs)
        output = run_sync_get_output(args)
        return output.split('\n')

    def _save_bin_snapshot(self, components, component_architectures):
        bin_snapshot = dict(self.snapshot)

        del bin_snapshot['00ostree-src-snapshot-version']
        bin_snapshot['00ostree-bin-snapshot-version'] = 0

        for target in bin_snapshot['targets']:
            base = target['base']
            base_name = 'bases/%s' % (base['name'], )
            base_revision = run_sync_get_output(['ostree', '--repo=' + self.repo,
                                                 'rev-parse', base_name])
            base['ostree-revision'] = base_revision

        component_refs = []
        for name in components.iterkeys():
            for architecture in component_architectures[name]:
                component_refs.append('components/%s/%s' % (name, architecture))

        new_components = {}
        resolved_refs = self._resolve_refs(component_refs)
        for name,rev in zip(components.iterkeys(), resolved_refs):
            for architecture in component_architectures[name]:
                archname = '%s/%s' % (name, architecture)
                new_components[archname] = rev

        bin_snapshot['components'] = new_components

        path = self.get_bin_snapshot_db().store(bin_snapshot)
        log("Binary snapshot: %s" % (path, ))

    def execute(self, argv):
        parser = argparse.ArgumentParser(description=self.short_description)
        parser.add_argument('--skip-built', action='store_true')
        parser.add_argument('--prefix')
        parser.add_argument('--src-snapshot')
        parser.add_argument('--compose', action='store_true')
        parser.add_argument('--start-at')
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
        self.buildopts.skip_built = args.skip_built

        required_components = {}
        component_architectures = {}
        for target in self.snapshot['targets']:
            for tree_content in target['contents']:
                (name, arch) = tree_content['name'].rsplit('/', 1)
                required_components[name] = self.snapshot['components'][name]
                if name not in component_architectures:
                    component_architectures[name] = set([arch])
                else:
                    component_architectures[name].add(arch)

        build_component_order = []
        if len(args.components) == 0:
            tsorted = buildutil.tsort_components(required_components, 'build-depends')
            tsorted.reverse()
            build_component_order = tsorted
        else:
            if args.start_at is not None:
                fatal("Can't specify --start-at with component list")
            for name in args.components:
                found = False
                component = self.snapshot['components'].get(name)
                if component is None:
                    fatal("Unknown component %r" % (name, ))
                build_component_order.append(name)

        start_at_index = -1
        if args.start_at is not None:
            for i,component_name in enumerate(build_component_order):
                if component_name == args.start_at:
                    start_at_index = i
                    break
            if start_at_index == -1:
                fatal("Unknown component %r specified for --start-at" % (args.start_at, ))
        else:
            start_at_index = 0

        for component_name in build_component_order[start_at_index:]:
            component = required_components[component_name]
            architectures = component_architectures[component_name]
            for architecture in architectures:
                self._build_one_component(component_name, component, architecture)

        self._save_bin_snapshot(required_components, component_architectures)   

        if args.compose:
            run_sync(['ostbuild', 'compose', '--prefix=' + self.prefix])
        
builtins.register(OstbuildBuildComponents)
