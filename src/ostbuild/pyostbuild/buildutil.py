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

import re

from .subprocess_helpers import run_sync_get_output

ARTIFACT_RE = re.compile(r'^artifact-([^,]+),([^,]+),([^,]+),([^,]+),(.+)-((?:runtime)|(?:devel))\.tar$')

def branch_name_for_artifact(a):
    return 'artifacts/%s/%s/%s' % (a['buildroot'],
                                   a['name'],
                                   a['branch'])

def get_git_version_describe(dirpath):
    version = run_sync_get_output(['git', 'describe', '--long', '--abbrev=42', '--always'],
                                  cwd=dirpath)
    return version.strip()
