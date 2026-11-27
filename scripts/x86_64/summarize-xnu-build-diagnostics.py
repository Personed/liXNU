#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Print a bounded summary of compiler and linker errors from an XNU build log."""

from __future__ import annotations

import pathlib
import re
import sys


MAX_DIAGNOSTICS = 20
MAX_UNDEFINED_SYMBOL_CONTEXT = 5
ERROR_LINE = re.compile(r": (?:fatal )?error:|^Undefined symbols|^ld: ")


def clean(line: str) -> str:
    return "".join(" " if ord(char) < 32 and char not in "\t" else char for char in line)[:500]


def summarize(lines: list[str]) -> list[str]:
    output: list[str] = []
    diagnostics = 0
    index = 0

    while index < len(lines) and diagnostics < MAX_DIAGNOSTICS:
        line = lines[index].rstrip("\r\n")
        if not ERROR_LINE.search(line):
            index += 1
            continue

        output.append(f"  {clean(line)}")
        diagnostics += 1
        index += 1

        if line.startswith("Undefined symbols"):
            context_count = 0
            while index < len(lines):
                context = lines[index].rstrip("\r\n")
                if not context.strip() or not context[:1].isspace():
                    break
                if context_count < MAX_UNDEFINED_SYMBOL_CONTEXT:
                    output.append(f"  {clean(context)}")
                    context_count += 1
                index += 1

    if diagnostics == 0:
        output.append("No compiler or linker errors recorded yet.")
    return output


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {pathlib.Path(argv[0]).name} BUILD_LOG", file=sys.stderr)
        return 2

    try:
        lines = pathlib.Path(argv[1]).read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as error:
        print(f"cannot read XNU build log: {error}", file=sys.stderr)
        return 2

    print("\n".join(summarize(lines)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
