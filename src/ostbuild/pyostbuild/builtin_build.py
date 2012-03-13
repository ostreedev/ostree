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
from . import fileutil
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

    def _build_one_component(self, name, component):
        branch = component['branch']
        architecture = component['architecture']

        buildname = 'components/%s' % (name, )

        current_vcs_version = component['revision']

        previous_build_version = run_sync_get_output(['ostree', '--repo=' + self.repo,
                                                      'rev-parse', buildname],
                                                     stderr=open('/dev/null', 'w'),
                                                     none_on_error=True)
        if previous_build_version is not None:
            log("Previous build of '%s' is %s" % (buildname, previous_build_version))

            previous_metadata_text = run_sync_get_output(['ostree', '--repo=' + self.repo,
                                                          'cat', previous_build_version,
                                                          '/_ostbuild-meta.json'],
                                                         log_initiation=True)
            previous_meta = json.loads(previous_metadata_text)

            previous_vcs_version = previous_meta['revision']

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

        checkoutdir = os.path.join(self.workdir, 'src')
        component_src = os.path.join(checkoutdir, name)
        run_sync(['ostbuild', 'checkout', '--clean', '--overwrite', name], cwd=checkoutdir)

        artifact_meta = dict(component)

        metadata_path = os.path.join(component_src, '_ostbuild-meta.json')
        f = open(metadata_path, 'w')
        json.dump(artifact_meta, f, indent=4, sort_keys=True)
        f.close()

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
        chroot_args.extend(['--pristine', '--name=' + name])
        if self.buildopts.shell_on_failure:
            ecode = run_sync_monitor_log_file(chroot_args, log_path, cwd=component_src, fatal_on_error=False)
            if ecode != 0:
                self._launch_debug_shell(architecture, buildroot_name, cwd=component_src)
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

    def _compose(self, target):
        base_name = 'bases/%s' % (target['base']['name'], )
        branch_to_rev = {}
        branch_to_subtrees = {}

        contents = [base_name]
        branch_to_subtrees[base_name] = ['/']
        base_revision = run_sync_get_output(['ostree', '--repo=' + self.repo,
                                             'rev-parse', base_name])

        branch_to_rev[base_name] = base_revision

        args = ['ostree', '--repo=' + self.repo, 'rev-parse']
        for component in target['contents']:
            name = component['name']
            contents.append(name)
            args.append('components/%s' % (name, ))
            branch_to_subtrees[name] = component['trees']
        branch_revs_text = run_sync_get_output(args)
        branch_revs = branch_revs_text.split('\n')

        for (content, rev) in zip(target['contents'], branch_revs):
            name = content['name']
            branch_to_rev[name] = rev
        
        compose_rootdir = os.path.join(self.workdir, 'roots', target['name'])
        if os.path.isdir(compose_rootdir):
            shutil.rmtree(compose_rootdir)
        os.mkdir(compose_rootdir)

        resolved_base = dict(target['base'])
        resolved_base['ostree-revision'] = base_revision
        resolved_contents = list(target['contents'])
        for component in resolved_contents:
            component['ostree-revision'] = branch_to_rev[component['name']]
        metadata = {'source': 'ostbuild compose v0',
                    'base': resolved_base, 
                    'contents': resolved_contents}

        for branch in contents:
            branch_rev = branch_to_rev[branch]
            subtrees = branch_to_subtrees[branch]
            for subtree in subtrees:
                run_sync(['ostree', '--repo=' + self.repo,
                          'checkout', '--user-mode',
                          '--union', '--subpath=' + subtree,
                          branch_rev, compose_rootdir])

        contents_path = os.path.join(compose_rootdir, 'contents.json')
        f = open(contents_path, 'w')
        json.dump(metadata, f, indent=4, sort_keys=True)
        f.close()

        run_sync(['ostree', '--repo=' + self.repo,
                  'commit', '-b', target['name'], '-s', 'Compose',
                  '--owner-uid=0', '--owner-gid=0', '--no-xattrs', 
                  '--skip-if-unchanged'], cwd=compose_rootdir)

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
        self.parse_snapshot()

        self.buildopts = BuildOptions()
        self.buildopts.shell_on_failure = args.shell_on_failure
        self.buildopts.skip_built = args.skip_built

        build_component_order = []
        if args.recompose:
            pass
        elif len(args.components) == 0:
            tsorted = buildutil.tsort_components(self.snapshot['components'], 'build-depends')
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
            component = self.snapshot['components'].get(component_name)
            self._build_one_component(component_name, component)

        for target in self.snapshot['targets']:
            self._compose(target)
        
builtins.register(OstbuildBuild)
