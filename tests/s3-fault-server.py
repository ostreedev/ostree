#!/usr/bin/env python3
#
# SPDX-License-Identifier: LGPL-2.0+
#
# A minimal S3-like HTTP server that misbehaves on request, for
# tests/test-pull-s3-faults.sh. It serves files under ROOT at /<path>
# (path-style, so "s3://bkt/repo?endpoint=..." maps to ROOT/bkt/repo), ignores
# authentication, and supports single-range GETs.
#
# The current fault is read from the file MODE on every request:
#
#   ok           behave
#   child500     answer 500 to every ranged request that does not start at 0
#   childshort   promise the full length for such requests, send half, hang up
#   child200     ignore the Range header on such requests (send it all, 200)
#   childgarbage send the right number of wrong bytes for such requests
#   noetag       send no ETag header
#   weaketag     send a weak ETag (W/"...")
#   etagchange   answer 412 to requests whose If-Match is not the "new" ETag,
#                and serve such requests anyway if they carry no If-Match
#   firstnorange answer the first range of an object with a 206 but no
#                Content-Range header
#   firstshort   for the first range of an object larger than that range, send
#                a correct Content-Range but only 1000 bytes of body
#   childshifted answer ranged requests that do not start at 0 with the range
#                one byte earlier, correctly labelled in Content-Range
#   firstempty   answer the first range of a larger object with an empty 206
#                and no Content-Range
#   firstdrop    for objects over 4 MiB: send 1 MiB of the first range, then
#                hang up; answer the other ranges only after 5 seconds
#   childslow    answer ranged requests that do not start at 0 after 1 second
#
# Every request is appended to LOG as "<start>-<end|*> <if-match|-> <path>".

import http.server
import os
import sys
import time

root, mode_file, log_file, port = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
root = os.path.realpath(root)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_GET(self):
        with open(mode_file) as f:
            mode = f.read().strip()
        path = self.path.split("?", 1)[0]
        rel = path.lstrip("/").replace("%2B", "+").replace("%2b", "+")
        full = os.path.normpath(os.path.join(root, rel))
        rng = self.headers.get("Range")
        if_match = self.headers.get("If-Match")

        if not full.startswith(root) or not os.path.isfile(full):
            body = b"<Error><Code>NoSuchKey</Code><Message>not found</Message></Error>"
            self.send_response(404)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        size = os.path.getsize(full)
        start, end = 0, size - 1
        ranged = False
        if rng and rng.startswith("bytes=") and size > 0:
            a, b = rng[6:].split("-", 1)
            start = int(a)
            end = min(int(b), size - 1) if b else size - 1
            ranged = True
        with open(log_file, "a") as f:
            f.write("%d-%s %s %s\n" % (start, end if ranged else "*", if_match or "-", rel))
        child = ranged and start > 0
        first = ranged and start == 0

        etag = '"v1-%d"' % size
        if mode == "weaketag":
            etag = 'W/"v1-%d"' % size
        if mode == "etagchange":
            etag = '"v1-%d"' % size if not child else '"v2-%d"' % size
            if child and if_match is not None and if_match != etag:
                self.send_response(412)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return

        if child and mode == "child500":
            body = b"<Error><Code>InternalError</Code><Message>injected</Message></Error>"
            self.send_response(500)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        if child and mode == "child200":
            ranged = False
            start, end = 0, size - 1

        if child and mode == "childshifted":
            start, end = start - 1, end - 1

        if child and mode == "childslow":
            time.sleep(1)

        if mode == "firstdrop" and ranged and size > 4194304:
            if child:
                time.sleep(5)
            else:
                self.send_response(206)
                self.send_header("Content-Length", str(end - start + 1))
                self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
                self.send_header("ETag", etag)
                self.end_headers()
                with open(full, "rb") as f:
                    self.wfile.write(f.read(1048576))
                self.wfile.flush()
                time.sleep(1)
                self.close_connection = True
                self.connection.close()
                return

        if first and mode == "firstempty" and end + 1 < size:
            self.send_response(206)
            self.send_header("Content-Length", "0")
            self.send_header("ETag", etag)
            self.end_headers()
            return

        if first and mode == "firstshort" and end + 1 < size:
            body = open(full, "rb").read(1000)
            self.send_response(206)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
            self.send_header("ETag", etag)
            self.end_headers()
            self.wfile.write(body)
            return

        length = end - start + 1
        self.send_response(206 if ranged else 200)
        self.send_header("Content-Length", str(length))
        if ranged and not (first and mode == "firstnorange"):
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        if mode != "noetag":
            self.send_header("ETag", etag)
        self.end_headers()
        with open(full, "rb") as f:
            f.seek(start)
            data = f.read(length)
        if child and mode == "childgarbage":
            data = b"\xa5" * length
        if child and mode == "childshort":
            self.wfile.write(data[: length // 2])
            self.wfile.flush()
            self.close_connection = True
            self.connection.close()
            return
        self.wfile.write(data)


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 256


Server(("127.0.0.1", port), Handler).serve_forever()
