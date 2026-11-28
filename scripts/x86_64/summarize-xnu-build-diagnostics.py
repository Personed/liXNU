#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Print a bounded summary of compiler and linker errors from an XNU build log."""

from __future__ import annotations

import pathlib
import re
import sys
from collections import deque
from collections.abc import Iterable


MAX_DIAGNOSTICS = 20
MAX_UNDEFINED_SYMBOL_CONTEXT = 5
MAX_LINKER_CONTEXT = 5
LINKER_CONTEXT_LINES = 40
ERROR_LINE = re.compile(r": (?:fatal )?error:|^Undefined symbols")
LINKER_LINE = re.compile(r"^\s*ld: ")
LINKER_NONERROR = re.compile(r"^\s*ld: (?:warning|note):")
LINKER_ERROR = re.compile(
    r"^\s*ld: (?:(?:error|fatal error|fatal):|"
    r"file too small \(length=[0-9]+\) for architecture [A-Za-z0-9_]+(?: in .+)?$)"
)
INTERLEAVED_COMPILER_ERROR = re.compile(
    r"^\s*ld:\s+(?P<source>[^:\s][^:]*:[0-9]+:[0-9]+: (?:fatal )?error:)"
)
LINKER_FAILURE = re.compile(
    r"^\s*clang(?:\+\+)?(?:-\d+)?:(?: fatal)? error: linker command failed\b"
)


def clean(line: str) -> str:
    return "".join(" " if ord(char) < 32 and char not in "\t" else char for char in line)[:500]


def summarize(lines: Iterable[str]) -> list[str]:
    output: list[str] = []
    diagnostics = 0
    iterator = enumerate(lines)
    pending: tuple[int, str] | None = None
    linker_context: deque[tuple[int, str]] = deque(maxlen=MAX_LINKER_CONTEXT)

    while diagnostics < MAX_DIAGNOSTICS:
        if pending is None:
            try:
                index, raw_line = next(iterator)
            except StopIteration:
                break
        else:
            index, raw_line = pending
            pending = None
        line = raw_line.rstrip("\r\n")
        if interleaved := INTERLEAVED_COMPILER_ERROR.match(line):
            line = line[interleaved.start("source"):]
        if LINKER_LINE.match(line):
            if not LINKER_ERROR.match(line):
                # Parallel compiler output can leave fragments that start with `ld:`.
                if not LINKER_NONERROR.match(line):
                    linker_context.append((index, clean(line)))
                continue
        elif not ERROR_LINE.search(line):
            continue

        if LINKER_FAILURE.match(line):
            output.extend(f"    {detail}" for number, detail in linker_context
                          if index - number <= LINKER_CONTEXT_LINES)
            linker_context.clear()
        output.append(f"  {clean(line)}")
        diagnostics += 1

        if line.startswith("Undefined symbols"):
            context_count = 0
            context_index = index
            while True:
                if diagnostics >= MAX_DIAGNOSTICS and (
                    context_count >= MAX_UNDEFINED_SYMBOL_CONTEXT
                    or context_index - index >= LINKER_CONTEXT_LINES
                ):
                    break
                try:
                    context_index, raw_context = next(iterator)
                except StopIteration:
                    break
                context = raw_context.rstrip("\r\n")
                if (
                    context[:1].isspace()
                    and LINKER_LINE.match(context)
                    and not LINKER_ERROR.match(context)
                    and not INTERLEAVED_COMPILER_ERROR.match(context)
                ):
                    # Parallel linker fragments can appear between symbol lines.
                    if not LINKER_NONERROR.match(context):
                        linker_context.append((context_index, clean(context.lstrip())))
                    continue
                if (
                    not context.strip()
                    or not context[:1].isspace()
                    or ERROR_LINE.search(context)
                    or LINKER_LINE.match(context)
                ):
                    pending = (context_index, raw_context)
                    break
                if context_count < MAX_UNDEFINED_SYMBOL_CONTEXT:
                    output.append(f"  {clean(context)}")
                    context_count += 1

    if diagnostics == 0:
        output.append("No confirmed compiler or linker errors recorded yet.")
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
