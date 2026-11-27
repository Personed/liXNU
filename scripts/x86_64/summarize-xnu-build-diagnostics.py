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
MAX_LINKER_CANDIDATE_GAP = 40
ERROR_LINE = re.compile(r": (?:fatal )?error:|^Undefined symbols")
LINKER_CANDIDATE = re.compile(r"^ld: (?!warning:|note:)")
LINKER_FAILURE = re.compile(r": (?:fatal )?error: linker command failed")


def clean(line: str) -> str:
    return "".join(" " if ord(char) < 32 and char not in "\t" else char for char in line)[:500]


def summarize(lines: Iterable[str]) -> list[str]:
    output: list[str] = []
    diagnostics = 0
    iterator = iter(lines)
    pending: str | None = None
    linker_candidates: deque[str] = deque()
    linker_candidate_gap = 0

    while diagnostics < MAX_DIAGNOSTICS:
        if pending is None:
            try:
                raw_line = next(iterator)
            except StopIteration:
                break
        else:
            raw_line, pending = pending, None
        line = raw_line.rstrip("\r\n")

        if LINKER_CANDIDATE.search(line):
            linker_candidates.append(line)
            linker_candidate_gap = 0
            continue

        linker_candidate_gap += 1
        if linker_candidate_gap > MAX_LINKER_CANDIDATE_GAP:
            linker_candidates.clear()

        if LINKER_FAILURE.search(line):
            # Parallel xcodebuild output can interleave arbitrary fragments at
            # the start of a physical line. Treat ld-prefixed lines as
            # candidates only, and emit the recent candidates once the clang
            # driver confirms that the linker actually failed.
            while linker_candidates and diagnostics < MAX_DIAGNOSTICS - 1:
                output.append(f"  {clean(linker_candidates.popleft())}")
                diagnostics += 1
            linker_candidates.clear()

        if not ERROR_LINE.search(line):
            continue

        output.append(f"  {clean(line)}")
        diagnostics += 1

        if line.startswith("Undefined symbols"):
            context_count = 0
            while True:
                if (
                    diagnostics >= MAX_DIAGNOSTICS
                    and context_count >= MAX_UNDEFINED_SYMBOL_CONTEXT
                ):
                    break
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
