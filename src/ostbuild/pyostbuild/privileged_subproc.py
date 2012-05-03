# Copyright (C) 2012 Colin Walters <walters@verbum.org>
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

import os,sys,subprocess

from .ostbuildlog import log, fatal
from . import ostbuildrc
from .subprocess_helpers import run_sync

class PrivilegedSubprocess(object):

    def spawn_sync(self, argv):
        helper = ostbuildrc.get_key('privileged_exec', default='pkexec')

        handlers = {'pkexec': self._pkexec_spawn_sync}

        handler = handlers.get(helper)
        if handler is None:
            fatal("Unrecognized privileged_exec; valid values=%r" % (handlers.keys(),))
        else:
            handler(argv)

    def _pkexec_spawn_sync(self, argv):
        pkexec_argv = ['pkexec'] + argv
        run_sync(pkexec_argv)
