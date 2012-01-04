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

def _get_env_for_cwd(cwd=None, env=None):
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
    return env_copy

def run_sync_get_output(args, cwd=None, env=None, stderr=None, none_on_error=False):
    log("running: %s" % (subprocess.list2cmdline(args),))
    env_copy = _get_env_for_cwd(cwd, env)
    f = open('/dev/null', 'r')
    if stderr is None:
        stderr_target = sys.stderr
    else:
        stderr_target = stderr
    proc = subprocess.Popen(args, stdin=f, stdout=subprocess.PIPE, stderr=stderr_target,
                            close_fds=True, cwd=cwd, env=env_copy)
    f.close()
    output = proc.communicate()[0].strip()
    if proc.returncode != 0 and not none_on_error:
        logfn = fatal
    else:
        logfn = log
    logfn("pid %d exited with code %d, %d bytes of output" % (proc.pid, proc.returncode, len(output)))
    if proc.returncode == 0:
        return output
    return None

def run_sync(args, cwd=None, env=None, fatal_on_error=True, keep_stdin=False):
    log("running: %s" % (subprocess.list2cmdline(args),))
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
    if keep_stdin:
        target_stdin = sys.stdin
    else:
        target_stdin = open('/dev/null', 'r')
    proc = subprocess.Popen(args, stdin=target_stdin, stdout=sys.stdout, stderr=sys.stderr,
                            close_fds=True, cwd=cwd, env=env_copy)
    if not keep_stdin:
        target_stdin.close()
    returncode = proc.wait()
    if fatal_on_error and returncode != 0:
        logfn = fatal
    else:
        logfn = log
    logfn("pid %d exited with code %d" % (proc.pid, returncode))
    return returncode
