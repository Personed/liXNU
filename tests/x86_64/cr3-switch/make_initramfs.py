#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Write the small newc archive without creating devices on the host."""

import gzip
from pathlib import Path
import stat
import sys


def write_archive(output, files, release):
    with open(output, "wb") as archive_file:
        with gzip.GzipFile(filename="", mode="wb", fileobj=archive_file, mtime=0) as archive:
            inode = 0

            def entry(name, mode, data=b"", major=0, minor=0):
                nonlocal inode
                inode += 1
                path = name.encode("ascii") + b"\0"
                fields = (inode, mode, 0, 0, 1, 0, len(data), 0, 0,
                          major, minor, len(path), 0)
                header = b"070701" + "".join(f"{field:08x}" for field in fields).encode("ascii")
                archive.write(header + path)
                archive.write(b"\0" * (-(len(header) + len(path)) % 4))
                archive.write(data)
                archive.write(b"\0" * (-len(data) % 4))

            for directory in ("bin", "dev", "proc", "sys"):
                entry(directory, stat.S_IFDIR | 0o755)
            entry("dev/console", stat.S_IFCHR | 0o600, major=5, minor=1)
            entry("dev/null", stat.S_IFCHR | 0o666, major=1, minor=3)
            for name, path, mode in files:
                entry(name, stat.S_IFREG | mode, Path(path).read_bytes())
            entry("expected-release", stat.S_IFREG | 0o644, (release + "\n").encode("ascii"))
            entry("TRAILER!!!", 0)


if __name__ == "__main__":
    output, init, busybox, module, release = sys.argv[1:]
    write_archive(output, (("init", init, 0o755),
                           ("bin/busybox", busybox, 0o755),
                           ("x86-cr3-switch.ko", module, 0o644)), release)
