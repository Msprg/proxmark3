#!/usr/bin/env python3
#-----------------------------------------------------------------------------
# Copyright (C) Proxmark3 contributors. See AUTHORS.md for details.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# See LICENSE.txt for the text of the license.
#-----------------------------------------------------------------------------
"""Offline GNU readline regression tests; run with a built client as argv[1]."""

import errno
import os
from pathlib import Path
import pty
import re
import select
import signal
import sys
import tempfile
import time


def read_until(fd, marker):
    output = b""
    deadline = time.monotonic() + 10
    while marker not in output:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([fd], [], [], remaining)[0]:
            raise AssertionError(f"Timed out waiting for {marker!r}: {output!r}")
        try:
            chunk = os.read(fd, 65536)
        except OSError as error:
            if error.errno != errno.EIO:
                raise
            chunk = b""
        if not chunk:
            raise AssertionError(f"Client exited: {output!r}")
        output += chunk
    return output


def main():
    client = Path(sys.argv[1] if len(sys.argv) > 1 else "client/proxmark3").resolve()
    cases = [
        ('data load --unknown "abc', 'data load --unknown "abc'),
        ('data load -f "space name.pm3" --bi', 'data load -f "space name.pm3" --bin '),
        ("data load --fi", "data load --file "),
        ("DATA LOAD --fi", "DATA LOAD --file "),
        ("  data  lo --fi", "  data  lo --file "),
        ("da lo --fi", "da lo --file "),
        ("da lo", "da load "),
        ("data load -f --bin --bi", "data load -f --bin --bin "),
        ("data load -bn --bi", "data load -bn --bi"),
        ("data load -bf sample --bi", "data load -bf sample --bi"),
        ("data load -fsample --fi", "data load -fsample --fi"),
        ("data load -- --fi", "data load -- --fi"),
        ("data load --file=sample --fi", "data load --file=sample --fi"),
        ("data load -f sam", "data load -f sample.pm3 "),
        ("data load --file=sam", "data load --file=sample.pm3 "),
        ('data load -f "space n', 'data load -f "space name.pm3" '),
        ("data load --unknown", "data load --unknown"),
        ("data load --unknown\t", "data load --unknown"),
        ("data load --fi tail\x1b[D\x1b[D\x1b[D\x1b[D\x1b[D", "data load --file tail"),
    ]
    with tempfile.TemporaryDirectory(prefix="pm3line-") as directory:
        for name in ("sample.pm3", "space name.pm3"):
            Path(directory, name).touch()
        pid, fd = pty.fork()
        if pid == 0:
            os.chdir(directory)
            os.environ.update(TERM="xterm", INPUTRC="/dev/null")
            os.execv(str(client), [str(client), "--incognito"])
        try:
            read_until(fd, b"--> ")
            for typed, expected in cases:
                # Redraw exposes the entire edited buffer without executing it.
                os.write(fd, b"\x01\x0b" + typed.encode() + b"\t\x0c")
                output = read_until(fd, b"\x1b[2J")
                if b"--> " not in output.split(b"\x1b[2J")[-1]:
                    output += read_until(fd, b"--> ")
                while select.select([fd], [], [], 0.1)[0]:
                    output += os.read(fd, 65536)
                line = re.sub(rb"\x1b\[[0-9;?]*[A-Za-z]", b"", output.split(b"\x1b[2J")[-1])
                actual = line.split(b"--> ", 1)[-1].decode().rstrip("\b")
                assert actual == expected, (typed, actual, expected)
                print(f"PASS {typed!r}")
        finally:
            os.kill(pid, signal.SIGTERM)
            os.waitpid(pid, 0)
            os.close(fd)


if __name__ == "__main__":
    main()
