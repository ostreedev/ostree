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

ARTIFACT_RE = re.compile(r'^artifact-([^,]+),([^,]+),([^,]+),([^,]+),(.+)-((?:runtime)|(?:devel))\.tar\.gz$')

def parse_artifact_name(artifact_basename):
    match = ARTIFACT_RE.match(artifact_basename)
    if match is None:
        raise ValueError("Invalid artifact basename %s" % (artifact_basename))
    return {'buildroot': match.group(1),
            'buildroot_version': match.group(2),
            'name': match.group(3),
            'branch': match.group(4),
            'version': match.group(5),
            'type': match.group(6)}

def branch_name_for_artifact(a):
    return 'artifacts/%s/%s/%s/%s' % (a['buildroot'],
                                      a['name'],
                                      a['branch'],
                                      a['type'])

