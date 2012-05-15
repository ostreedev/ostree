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

import os,sys,subprocess,tempfile,re,shutil
import argparse
import time
import urlparse
import json
from StringIO import StringIO

from . import builtins
from .ostbuildlog import log, fatal
from .subprocess_helpers import run_sync
from . import ostbuildrc
from . import fileutil

class OstbuildPrivhelperRunQemu(builtins.Builtin):
    name = "privhelper-run-qemu"
    short_description = "Helper for run-qemu"

    def __init__(self):
        builtins.Builtin.__init__(self)

    def execute(self, argv):
        parser = argparse.ArgumentParser(description=self.short_description)
        parser.add_argument('target')

        args = parser.parse_args(argv)

        if os.geteuid() != 0:
            fatal("This helper can only be run as root")

        self.ostree_dir = self.find_ostree_dir()
        self.qemu_path = os.path.join(self.ostree_dir, "ostree-qemu.img")

        release = os.uname()[2]

        qemu = 'qemu-kvm'
        kernel = '/boot/vmlinuz-%s' % (release, )
        initramfs = '/boot/initramfs-ostree-%s.img' % (release, )
        memory = '512M'
        extra_args = 'root=/dev/sda rd.pymouth=0 ostree=%s' % (args.target, )

        args = [qemu, '-kernel', kernel, '-initrd', initramfs,
                '-hda', self.qemu_path, '-m', memory, '-append', extra_args]
        log("Running: %s" % (subprocess.list2cmdline(args), ))
        os.execvp(qemu, args)
        
builtins.register(OstbuildPrivhelperRunQemu)
