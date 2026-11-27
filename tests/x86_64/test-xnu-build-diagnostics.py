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


def test_interleaved_parallel_log_fragments_are_not_linker_errors() -> None:
    expected = ["No compiler or linker errors recorded yet."]
    lines = [
        "ld:  odr_union_to_sockaddr",
        "ld: lock 194enIn file included from /src/bsd/netinet/in_var.h:595:",
        "ld:41:",
    ]
    assert MODULE.summarize(lines) == expected


def test_linker_candidates_are_emitted_after_confirmed_linker_failure() -> None:
    lines = [
        "ld: file too small (length=8) for architecture x86_64",
        "ld: unknown option: -bad",
        "clang: error: linker command failed with exit code 1",
    ]
    assert MODULE.summarize(lines) == [
        "  ld: file too small (length=8) for architecture x86_64",
        "  ld: unknown option: -bad",
        "  clang: error: linker command failed with exit code 1",
    ]


def test_stale_interleaved_fragments_are_not_linker_failure_details() -> None:
    lines = [
        "ld:  odr_union_to_sockaddr",
        *(f"ordinary parallel output {index}" for index in range(MODULE.MAX_LINKER_CANDIDATE_GAP + 1)),
        "ld: file too small (length=8) for architecture x86_64",
        "clang: error: linker command failed with exit code 1",
    ]
    rendered = "\n".join(MODULE.summarize(lines))
    assert "odr_union_to_sockaddr" not in rendered, rendered
    assert "file too small" in rendered, rendered


def test_empty_and_non_error_logs_are_explained() -> None:
    expected = ["No compiler or linker errors recorded yet."]
    assert MODULE.summarize([]) == expected
    assert MODULE.summarize(["build started", "warning: still compiling"]) == expected


if __name__ == "__main__":
    test_symbol_context_does_not_hide_terminal_errors()
    test_only_main_diagnostics_consume_the_limit()
    test_indented_error_is_not_discarded_as_linker_context()
    test_summary_stops_reading_after_the_primary_diagnostic_limit()
    test_last_symbol_block_stops_after_its_context_limit()
    test_linker_warnings_do_not_hide_compiler_errors()
    test_interleaved_parallel_log_fragments_are_not_linker_errors()
    test_linker_candidates_are_emitted_after_confirmed_linker_failure()
    test_stale_interleaved_fragments_are_not_linker_failure_details()
    test_empty_and_non_error_logs_are_explained()
    print("XNU diagnostic summary tests passed.")
