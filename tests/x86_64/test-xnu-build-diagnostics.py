#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Regression tests for the bounded XNU build diagnostic summary."""

from __future__ import annotations

import importlib.util
import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "scripts/x86_64/summarize-xnu-build-diagnostics.py"
SPEC = importlib.util.spec_from_file_location("xnu_build_diagnostics", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def test_symbol_context_does_not_hide_terminal_errors() -> None:
    lines = ["Undefined symbols for architecture x86_64:"]
    lines.extend(f"    _symbol_{index}, referenced from" for index in range(30))
    lines.extend([
        "ld: symbol(s) not found for architecture x86_64",
        "clang: error: linker command failed with exit code 1",
    ])

    output = MODULE.summarize(lines)
    rendered = "\n".join(output)
    assert len(output) == 8, output  # header + five context lines + both terminal diagnostics
    assert "_symbol_0" in rendered and "_symbol_4" in rendered, rendered
    assert "_symbol_5" not in rendered, rendered
    assert "ld: symbol(s) not found" in rendered, rendered
    assert "clang: error: linker command failed" in rendered, rendered


def test_only_main_diagnostics_consume_the_limit() -> None:
    lines = [f"/src/file{index}.c:1:2: error: synthetic failure" for index in range(25)]
    output = MODULE.summarize(lines)
    assert len(output) == MODULE.MAX_DIAGNOSTICS, output
    assert "file19.c" in output[-1], output
    assert "file20.c" not in "\n".join(output), output


def test_empty_and_non_error_logs_are_explained() -> None:
    expected = ["No compiler or linker errors recorded yet."]
    assert MODULE.summarize([]) == expected
    assert MODULE.summarize(["build started", "warning: still compiling"]) == expected


if __name__ == "__main__":
    test_symbol_context_does_not_hide_terminal_errors()
    test_only_main_diagnostics_consume_the_limit()
    test_empty_and_non_error_logs_are_explained()
    print("XNU diagnostic summary tests passed.")
