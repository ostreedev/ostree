#!/usr/bin/python
#
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

import os
import sys
import subprocess

from .ostbuildlog import log, fatal

def run_sync(args, cwd=None, env=None):
    log("running: %r" % (args,))
    f = open('/dev/null', 'r')
    # This dance is necessary because we want to keep the PWD
    # environment variable up to date.  Not doing so is a recipie
    # for triggering edge conditions in pwd lookup.
    if (cwd is not None) and (env is None or ('PWD' in env)):
        if env is None:
            env_copy = os.environ.copy()
        else:
            env_copy = env.copy()
        if ('PWD' in env_copy) and (not cwd.startswith('/')):
            env_copy['PWD'] = os.path.join(env_copy['PWD'], cwd)
        else:
            env_copy['PWD'] = cwd
    else:
        env_copy = env
    proc = subprocess.Popen(args, stdin=f, stdout=sys.stdout, stderr=sys.stderr,
                            close_fds=True, cwd=cwd, env=env_copy)
    f.close()
    returncode = proc.wait()
    if returncode != 0:
        logfn = fatal
    else:
        logfn = log
    logfn("pid %d exited with code %d" % (proc.pid, returncode))
