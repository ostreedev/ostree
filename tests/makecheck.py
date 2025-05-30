#!/usr/bin/env python3
import subprocess
import sys
import os
import argparse
import shutil
import re

HEADERS = ["PASS", "SKIP", "XFAIL", "FAIL", "XPASS", "ERROR"]

def is_header(line) -> bool:
    return line.startswith("========")

def run_make_check():
    """
    Runs 'make check' with optional additional arguments.
    Returns True if 'make check' succeeds, False otherwise.
    """
    command = ['make', 'check', '-j', '6'] + sys.argv[1:]
    print(f"Running '{' '.join(command)}'...")
    try:
        result = subprocess.run(command, check=False) # check=False to handle return code manually
    except FileNotFoundError:
        print(f"Error: 'make' command not found. Is it in your PATH?", file=sys.stderr)
        return False # Indicate failure

    if result.returncode == 0:
        return True
    return False
    
def print_truncated(lines):
    if len(lines) == 0:
        return
    print()
    print(lines[0])
    print("(skipped %d lines)" % max(len(lines) - 20, 0))
    print(os.linesep.join(lines[-20:]))
    print("-" * 20)

def get_failed_test_output(lines):
    """
    Parses test-suite.log to find failed tests and print the last 20 lines
    of their output.
    """
    in_error_section = None
    prevline = None
    errlines = []
    for line in lines:
        line = line.strip()
        if is_header(line) and prevline != None:
            (k, v) = prevline.split(':')
            print("%s %s" % (k, v))
            if k in ('ERROR', 'FAIL'):
                if in_error_section:
                    print_truncated(errlines)
                    in_error_section = None
                else:
                    in_error_section = v
                errlines = []
        prevline = line
        if in_error_section:
            errlines.append(line)
    print_truncated(errlines)

if __name__== "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "analyze":
        get_failed_test_output(open(sys.argv[2]).readlines())
        sys.exit(0)

    if run_make_check():
        print("make check passed successfully.")
        sys.exit(0)
    else:
        print("make check failed. Attempting to extract failed test output.")
        get_failed_test_output(open('test-suite.log').readlines())
        artifacts = os.environ.get('ARTIFACTS')
        if artifacts is not None:
            shutil.move('test-suite.log', os.path.join(artifacts, 'test-suite.log'))
            print("Saved test-suite.log to artifacts directory.")
        sys.exit(1)
