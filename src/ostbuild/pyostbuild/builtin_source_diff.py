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
import argparse
from StringIO import StringIO
import json

from . import builtins
from .ostbuildlog import log, fatal
from . import vcs
from .subprocess_helpers import run_sync, run_sync_get_output
from . import buildutil

class OstbuildSourceDiff(builtins.Builtin):
    name = "source-diff"
    short_description = "Show differences in source code between builds"

    def __init__(self):
        builtins.Builtin.__init__(self)

    def _diff(self, name, mirrordir, from_revision, to_revision,
              diffstat=False):
        diff_replace_re = re.compile(' [ab]')

        env = dict(os.environ)
        env['LANG'] = 'C'
                
        spacename = ' ' + name

        sys.stdout.write('diff of %s revision range %s..%s\n' % (name, from_revision, to_revision))
        sys.stdout.flush()

        diff_proc = subprocess.Popen(['git', 'diff', from_revision, to_revision],
                                     env=env, cwd=mirrordir, stdout=subprocess.PIPE)
        if diffstat:
            diffstat_proc = subprocess.Popen(['diffstat', '-p0'],
                                             stdin=subprocess.PIPE,
                                             stdout=sys.stdout)
            diff_pipe = diffstat_proc.stdin
        else:
            diffstat_proc = None
            diff_pipe = sys.stdout
        for line in diff_proc.stdout:
            if (line.startswith('diff --git ')
                or line.startswith('--- a/')
                or line.startswith('+++ b/')
                or line.startswith('Binary files /dev/null and b/')):
                line = diff_replace_re.sub(spacename, line)
                diff_pipe.write(line)
            else:
                diff_pipe.write(line)
        diff_proc.wait()
        if diffstat_proc is not None:
            diffstat_proc.stdin.close()
            diffstat_proc.wait()

    def _log(self, opts, name, mirrordir, from_revision, to_revision):
        env = dict(os.environ)
        env['LANG'] = 'C'
                
        spacename = ' ' + name

        args = ['git', 'log']
        args.extend(opts)
        args.append(from_revision + '...' + to_revision)
        proc = subprocess.Popen(args, env=env, cwd=mirrordir, stdout=subprocess.PIPE)
        for line in proc.stdout:
            sys.stdout.write(line)
        proc.wait()

    def _snapshot_from_rev(self, rev):
        self.init_repo()
        text = run_sync_get_output(['ostree', '--repo=' + self.repo,
                                    'cat', rev, '/contents.json'],
                                   log_initiation=False)
        return json.loads(text)

    def execute(self, argv):
        parser = argparse.ArgumentParser(description=self.short_description)
        parser.add_argument('--log', action='store_true')
        parser.add_argument('--logp', action='store_true')
        parser.add_argument('--diffstat', action='store_true')
        parser.add_argument('--rev-from')
        parser.add_argument('--rev-to')
        parser.add_argument('--snapshot-from')
        parser.add_argument('--snapshot-to')

        args = parser.parse_args(argv)
        self.parse_config()

        to_snap = None
        from_snap = None

        if args.rev_to:
            to_snap = self._snapshot_from_rev(args.rev_to)
        if args.rev_from:
            from_snap = self._snapshot_from_rev(args.rev_from)
        if args.snapshot_from:
            from_snap = json.load(open(args.snapshot_from))
        if args.snapshot_to:
            to_snap = json.load(open(args.snapshot_to))

        if to_snap is None:
            fatal("One of --rev-to/--snapshot-to must be given")
        if from_snap is None:
            if args.rev_to:
                from_snap = self._snapshot_from_rev(args.rev_to + '^')
            else:
                fatal("One of --rev-from/--snapshot-from must be given")

        for from_component in from_snap['components']:
            name = from_component['name']
            src = from_component['src']
            (keytype, uri) = vcs.parse_src_key(src)
            if keytype == 'local':
                log("Component %r has local URI" % (name, ))
                continue
            branch_or_tag = from_component.get('branch') or from_component.get('tag')
            mirrordir = vcs.ensure_vcs_mirror(self.mirrordir, keytype, uri, branch_or_tag)

            to_component = self.find_component_in_snapshot(name, to_snap)
            if to_component is None:
                log("DELETED COMPONENT: %s" % (name, ))
                continue

            from_revision = from_component.get('revision')
            to_revision = to_component.get('revision')
            if from_revision is None:
                log("From component %s missing revision" % (name, ))
                continue
            if to_revision is None:
                log("From component %s missing revision" % (name, ))
                continue

            if from_revision != to_revision:
                if args.log:
                    self._log([], name, mirrordir, from_revision, to_revision)
                elif args.logp:
                    self._log(['-p'], name, mirrordir, from_revision, to_revision)
                else:
                    self._diff(name, mirrordir, from_revision, to_revision,
                               diffstat=args.diffstat)

builtins.register(OstbuildSourceDiff)
