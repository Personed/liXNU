#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Print a bounded summary of compiler and linker errors from an XNU build log."""

from __future__ import annotations

import pathlib
import re
import sys
from collections.abc import Iterable


MAX_DIAGNOSTICS = 20
MAX_UNDEFINED_SYMBOL_CONTEXT = 5
ERROR_LINE = re.compile(r": (?:fatal )?error:|^Undefined symbols|^ld: ")


def clean(line: str) -> str:
    return "".join(" " if ord(char) < 32 and char not in "\t" else char for char in line)[:500]


def summarize(lines: Iterable[str]) -> list[str]:
    output: list[str] = []
    diagnostics = 0
    iterator = iter(lines)
    pending: str | None = None

    while diagnostics < MAX_DIAGNOSTICS:
        if pending is None:
            try:
                raw_line = next(iterator)
            except StopIteration:
                break
        else:
            raw_line, pending = pending, None
        line = raw_line.rstrip("\r\n")
        if not ERROR_LINE.search(line):
            continue

        output.append(f"  {clean(line)}")
        diagnostics += 1

        if line.startswith("Undefined symbols"):
            context_count = 0
            while True:
                try:
                    raw_context = next(iterator)
                except StopIteration:
                    break
                context = raw_context.rstrip("\r\n")
                if (
                    not context.strip()
                    or not context[:1].isspace()
                    or ERROR_LINE.search(context)
                ):
                    pending = raw_context
                    break
                if context_count < MAX_UNDEFINED_SYMBOL_CONTEXT:
                    output.append(f"  {clean(context)}")
                    context_count += 1

    if diagnostics == 0:
        output.append("No compiler or linker errors recorded yet.")
    return output


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {pathlib.Path(argv[0]).name} BUILD_LOG", file=sys.stderr)
        return 2

    try:
        with pathlib.Path(argv[1]).open(encoding="utf-8", errors="replace") as log:
            output = summarize(log)
    except OSError as error:
        print(f"cannot read XNU build log: {error}", file=sys.stderr)
        return 2

    print("\n".join(output))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
