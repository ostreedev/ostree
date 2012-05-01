#
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

import os
import stat
import time
import tempfile
import re
import shutil
import hashlib
import json

class JsonDB(object):
    def __init__(self, dirpath, prefix):
        self._dirpath = dirpath
        self._prefix = prefix
        self._version_csum_re = re.compile(r'-(\d+)\.(\d+)-([0-9a-f]+).json$')

    def _cmp_match_by_version(self, a, b):
        # Note this is a reversed comparison; bigger is earlier
        a_major = a[0]
        a_minor = a[1]
        b_major = b[0]
        b_minor = b[1]

        c = cmp(b_major, a_major)
        if c == 0:
            return cmp(b_minor, a_minor)
        return 0

    def _get_all(self):
        result = []
        for fname in os.listdir(self._dirpath):
            if not (fname.startswith(self._prefix) and fname.endswith('.json')):
                continue

            path = os.path.join(self._dirpath, fname)
            match = self._version_csum_re.search(fname)
            if not match:
                raise Exception("Invalid file '%s' in JsonDB; doesn't contain version+checksum",
                                path)
            result.append((int(match.group(1)), int(match.group(2)), match.group(3), fname))
        result.sort(self._cmp_match_by_version)
        return result

    def get_latest(self):
        path = self.get_latest_path()
        if path is None:
            return None
        return json.load(open(path))

    def get_latest_path(self):
        files = self._get_all()
        if len(files) == 0:
            return None
        return os.path.join(self._dirpath, files[0][3])

    def store(self, obj):
        files = self._get_all()
        if len(files) == 0:
            latest = None
        else:
            latest = files[0]

        current_time = time.gmtime()

        (fd, tmppath) = tempfile.mkstemp(suffix='.tmp',
                prefix='tmp-jsondb-', dir=self._dirpath)
        os.close(fd)
        f = open(tmppath, 'w')
        json.dump(obj, f, indent=4, sort_keys=True)
        f.close()

        csum = hashlib.sha256()
        f = open(tmppath)
        buf = f.read(8192)
        while buf != '':
            csum.update(buf)
            buf = f.read(8192)
        f.close()
        digest = csum.hexdigest()
        
        if latest is not None:
            if digest == latest[2]:
                os.unlink(tmppath)
                return latest[3]
            latest_version = (latest[0], latest[1])
        else:
            latest_version = (current_time.tm_year, 0)
        target_name = '%s-%d.%d-%s.json' % (self._prefix, current_time.tm_year,
                                            latest_version[1] + 1, digest)
        target_path = os.path.join(self._dirpath, target_name)
        os.rename(tmppath, target_path)
        return target_path
                
                
        
