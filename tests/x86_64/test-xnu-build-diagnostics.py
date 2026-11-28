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


def test_indented_error_is_not_discarded_as_linker_context() -> None:
    lines = ["Undefined symbols for architecture x86_64:"]
    lines.extend(f"    _symbol_{index}, referenced from" for index in range(8))
    lines.append("    /src/late.c:8:3: error: indented compiler failure")

    output = MODULE.summarize(lines)
    rendered = "\n".join(output)
    assert "indented compiler failure" in rendered, rendered
    assert "_symbol_5" not in rendered, rendered


def test_summary_stops_reading_after_the_primary_diagnostic_limit() -> None:
    def bounded_input():
        for index in range(MODULE.MAX_DIAGNOSTICS):
            yield f"/src/file{index}.c:1:2: error: synthetic failure"
        raise AssertionError("summary read beyond the primary diagnostic limit")

    assert len(MODULE.summarize(bounded_input())) == MODULE.MAX_DIAGNOSTICS


def test_last_symbol_block_stops_after_its_context_limit() -> None:
    def bounded_input():
        for index in range(MODULE.MAX_DIAGNOSTICS - 1):
            yield f"/src/file{index}.c:1:2: error: synthetic failure"
        yield "Undefined symbols for architecture x86_64:"
        for index in range(MODULE.MAX_UNDEFINED_SYMBOL_CONTEXT):
            yield f"    _symbol_{index}, referenced from"
        raise AssertionError("summary read beyond the final symbol context limit")

    output = MODULE.summarize(bounded_input())
    assert len(output) == MODULE.MAX_DIAGNOSTICS + MODULE.MAX_UNDEFINED_SYMBOL_CONTEXT


def test_linker_warnings_do_not_hide_compiler_errors() -> None:
    lines = [f"ld: warning: synthetic warning {index}" for index in range(20)]
    lines.append("clang: error: linker command failed with exit code 1")
    output = MODULE.summarize(lines)
    assert output == ["  clang: error: linker command failed with exit code 1"], output


def test_empty_and_non_error_logs_are_explained() -> None:
    expected = ["No confirmed compiler or linker errors recorded yet."]
    assert MODULE.summarize([]) == expected
    assert MODULE.summarize(["build started", "warning: still compiling"]) == expected
    assert MODULE.summarize([
        "ld:  odr_union_to_sockaddr",
        "ld: lock 194enIn file included from ...",
        "ld: interleaved fragment: error: from another command",
        "ld: note: repeated text: error: from another command",
    ]) == expected


def test_apple_clangxx_confirms_recent_context() -> None:
    terminal = "clang++: error: linker command failed with exit code 1 (use -v to see invocation)"
    lines = ["ld: stale detail", *("compiling" for _ in range(MODULE.LINKER_CONTEXT_LINES))]
    assert MODULE.summarize([*lines, "ld: file too small", terminal]) == [
        "    ld: file too small", f"  {terminal}",
    ]
    output = MODULE.summarize([
        *(f"ld: detail {i}" for i in range(MODULE.MAX_LINKER_CONTEXT + 1)), terminal, terminal,
    ])
    assert output == [
        *(f"    ld: detail {i}" for i in range(1, MODULE.MAX_LINKER_CONTEXT + 1)),
        f"  {terminal}", f"  {terminal}",
    ], output


def test_explicit_linker_error_is_kept_without_a_driver_marker() -> None:
    line = "ld: error: malformed input file"
    assert MODULE.summarize([line]) == [f"  {line}"]


def test_each_linker_detail_expires_independently() -> None:
    gap = MODULE.LINKER_CONTEXT_LINES
    lines = [
        "ld: stale detail",
        *("ordinary compiler output" for _ in range(gap - 1)),
        "ld: fresh detail",
        "clang: error: linker command failed with exit code 1",
    ]
    output = MODULE.summarize(lines)
    assert output == [
        "    ld: fresh detail",
        "  clang: error: linker command failed with exit code 1",
    ], output


def test_compiler_diagnostic_cannot_confirm_linker_context() -> None:
    compiler_error = "/src/comment.c:8:3: error: linker command failed explanation"
    output = MODULE.summarize(["ld: unrelated fragment", compiler_error])
    assert output == [f"  {compiler_error}"], output


def test_partial_darwin_link_error_without_clang_confirmation() -> None:
    error = "ld: file too small (length=8) for architecture x86_64"
    output = MODULE.summarize(["ld: interleaved fragment", error])
    assert output == [f"  {error}"], output


def test_interleaved_prefix_does_not_hide_compiler_error() -> None:
    error = "/src/example.c:42:9: error: incompatible pointer types"
    output = MODULE.summarize(["ld: unrelated fragment", f"ld:   {error}"])
    assert output == [f"  {error}"], output


def test_interleaved_linker_fragment_does_not_end_symbol_context() -> None:
    header = "Undefined symbols for architecture x86_64:"
    fragment = "ld: interleaved linker detail"
    symbol = '    "_required_symbol", referenced from:'
    reference = "        _callee in real.o"
    terminal = "clang: error: linker command failed with exit code 1"
    output = MODULE.summarize([header, f"    {fragment}", symbol, reference, terminal])
    assert output == [
        f"  {header}", f"  {symbol}", f"  {reference}", f"    {fragment}", f"  {terminal}",
    ], output


if __name__ == "__main__":
    test_symbol_context_does_not_hide_terminal_errors()
    test_only_main_diagnostics_consume_the_limit()
    test_indented_error_is_not_discarded_as_linker_context()
    test_summary_stops_reading_after_the_primary_diagnostic_limit()
    test_last_symbol_block_stops_after_its_context_limit()
    test_linker_warnings_do_not_hide_compiler_errors()
    test_empty_and_non_error_logs_are_explained()
    test_apple_clangxx_confirms_recent_context()
    test_explicit_linker_error_is_kept_without_a_driver_marker()
    test_each_linker_detail_expires_independently()
    test_compiler_diagnostic_cannot_confirm_linker_context()
    test_partial_darwin_link_error_without_clang_confirmation()
    test_interleaved_prefix_does_not_hide_compiler_error()
    test_interleaved_linker_fragment_does_not_end_symbol_context()
    print("XNU diagnostic summary tests passed.")
