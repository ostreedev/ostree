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

def log(msg, prefix=None):
    if prefix is None:
        prefix_target = ''
    else:
        prefix_target = prefix
    fullmsg = '%s: %s%s\n' % (os.path.basename(sys.argv[0]), prefix_target, msg)
    sys.stdout.write(fullmsg)
    sys.stdout.flush()

def fatal(msg):
    log(msg, prefix="FATAL: ")
    sys.exit(1)

