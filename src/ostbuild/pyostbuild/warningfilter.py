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
import re
import stat
import fcntl
import subprocess

from . import filemonitor
from . import mainloop

warning_re = re.compile(r'(: ((warning)|(error)|(fatal error)): )|(make(\[[0-9]+\])?: \*\*\*)')
output_whitelist_re = re.compile(r'^(make(\[[0-9]+\])?: Entering directory)|(ostbuild:)')

_bold_sequence = None
_normal_sequence = None
if os.isatty(1):
    _bold_sequence = subprocess.Popen(['tput', 'bold'], stdout=subprocess.PIPE, stderr=open('/dev/null', 'w')).communicate()[0]
    _normal_sequence = subprocess.Popen(['tput', 'sgr0'], stdout=subprocess.PIPE, stderr=open('/dev/null', 'w')).communicate()[0]
def _bold(text):
    if _bold_sequence is not None:
        return '%s%s%s' % (_bold_sequence, text, _normal_sequence)
    else:
        return text

class WarningFilter(object):
    def __init__(self, filename, output):
        self.filename = filename
        self.output = output

        # inherit globals
        self._warning_re = warning_re
        self._nonfilter_re = output_whitelist_re

        self._buf = ''
        self._warning_count = 0
        self._filtered_line_count = 0
        filemon = filemonitor.FileMonitor.get()
        filemon.add(filename, self._on_changed)
        self._fd = os.open(filename, os.O_RDONLY)
        fcntl.fcntl(self._fd, fcntl.F_SETFL, os.O_NONBLOCK)

    def _do_read(self):
        while True:
            buf = os.read(self._fd, 4096)
            if buf == '':
                break
            self._buf += buf
        self._flush()

    def _write_last_log_lines(self):
        _last_line_limit = 100
        f = open(self.filename)
        lines = []
        for line in f:
            if line.startswith('ostbuild '):
                continue
            lines.append(line)
            if len(lines) > _last_line_limit:
                lines.pop(0)
        f.close()
        for line in lines:
            self.output.write('| ')
            self.output.write(line)

    def _flush(self):
        while True:
            p = self._buf.find('\n')
            if p < 0:
                break
            line = self._buf[0:p]
            self._buf = self._buf[p+1:]
            match = self._warning_re.search(line)
            if match:
                self._warning_count += 1
                self.output.write(line + '\n')
            else:    
                match = self._nonfilter_re.search(line)
                if match:
                    self.output.write(line + '\n')
                else:
                    self._filtered_line_count += 1

    def _on_changed(self):
        self._do_read()

    def start(self):
        self._do_read()

    def finish(self, successful):
        self._do_read()
        if not successful:
            self._write_last_log_lines()
            pass
        self.output.write("ostbuild %s: %d warnings\n" % ('success' if successful else _bold('failed'),
                                                            self._warning_count, ))
        self.output.write("ostbuild: full log path: %s\n" % (self.filename, ))
