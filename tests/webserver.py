#!/usr/bin/env python3
# Run a webserver on a random port, serving the current working directory.
# The allocated port is written to the provided path
import http.server
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import socket
import sys
import os
import argparse
import threading
import time
import contextlib

def _get_best_family(*address):
    infos = socket.getaddrinfo(
        *address,
        type=socket.SOCK_STREAM,
        flags=socket.AI_PASSIVE,
    )
    family, ty, proto, canonname, sockaddr = next(iter(infos))
    return family, sockaddr

def run(port_path, HandlerClass=SimpleHTTPRequestHandler,
        ServerClass=ThreadingHTTPServer,
        protocol="HTTP/1.1", port=0, bind=None):
    ServerClass.address_family, addr = _get_best_family(bind, port)
    HandlerClass.protocol_version = protocol

    server = ThreadingHTTPServer(addr, HandlerClass)

    with server as httpd:
        host, port = httpd.socket.getsockname()[:2]
        with open(port_path + '.tmp', 'w') as f:
            f.write(f'{port}\n')
        os.rename(port_path + '.tmp', port_path)
        print(f"Running on port {port}")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nKeyboard interrupt received, exiting.")
            sys.exit(0)

def main():
    # We automatically daemonize
    pid = os.fork()
    if pid > 0:
        sys.exit(0)
    os.setsid()
    pid = os.fork()
    if pid > 0:
        sys.exit(0)
    # code to spawn a thread and detect when the current working directory no longer exists, then exit
    def watch_cwd(original_cwd):
        while True:
            try:
                os.stat(original_cwd)
            except FileNotFoundError:
                sys.exit(0)
            time.sleep(1)

    original_cwd = os.getcwd()
    watcher_thread = threading.Thread(target=watch_cwd, args=(original_cwd,))
    watcher_thread.daemon = True
    watcher_thread.start()

    parser = argparse.ArgumentParser()
    parser.add_argument('port_path', metavar='PATH',
                        help='Write port used to this path ')
    args = parser.parse_args()
    run(args.port_path)

if __name__ == '__main__':
    main()
