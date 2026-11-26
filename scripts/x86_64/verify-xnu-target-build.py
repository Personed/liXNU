#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Validate the effective SDK and inputs used by pinned XNU target compiles."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import shlex
import sys
import tempfile


ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
SOURCE_SUFFIXES = {
    ".c": "c",
    ".m": "c",
    ".cc": "cxx",
    ".cpp": "cxx",
    ".cxx": "cxx",
    ".mm": "cxx",
    ".s": "assembly",
    ".asm": "assembly",
}
EXCLUDED_SUFFIXES = {
    "kernel_usr_local": "usr/local/include/kernel",
    "iBoot_usr_local": "usr/local/include/iBoot",
    "Kernel_framework_AppleFeatures": "System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders/AppleFeatures",
    "Kernel_framework_platform": "System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders/platform",
}
WARNING_EXCEPTIONS = (
    "-Wno-error=reserved-identifier",
    "-Wno-error=unused-but-set-variable",
    "-Wno-error=void-pointer-to-enum-cast",
)


class ValidationError(Exception):
    pass


def canonical(path: str | pathlib.Path) -> pathlib.Path:
    return pathlib.Path(os.path.realpath(path))


def classify_source(tokens: list[str], source: str) -> tuple[str, str]:
    """Return (language class, source token) for a compiler invocation."""
    explicit_language = None
    for index, token in enumerate(tokens[:-1]):
        if token == "-x":
            explicit_language = tokens[index + 1].lower()
    if explicit_language:
        if "assembler" in explicit_language or explicit_language in {"asm", "s"}:
            return "assembly", source
        if explicit_language in {"c", "objective-c"}:
            return "c", source
        if explicit_language in {"c++", "objective-c++", "cpp-output", "c++-cpp-output"}:
            return "cxx", source
    language = SOURCE_SUFFIXES.get(pathlib.Path(source).suffix.lower())
    if language is None:
        raise ValidationError(f"could not classify XNU target compile source: {source!r}")
    return language, source


def compiler_path(token: str, expected_compilers: set[pathlib.Path]) -> bool:
    try:
        return canonical(token) in expected_compilers
    except (OSError, RuntimeError):
        return False


def parse_commands(objroot: pathlib.Path, expected_compilers: set[pathlib.Path]) -> list[dict[str, object]]:
    commands = []
    fragments = sorted(objroot.rglob("*.o.json"))
    if not fragments:
        raise ValidationError(f"XNU build produced no Clang -MJ compilation database fragments under {objroot}")
    for fragment in fragments:
        raw_text = fragment.read_text(errors="replace").strip()
        if not raw_text:
            raise ValidationError(f"empty XNU Clang -MJ compilation database fragment: {fragment}")
        try:
            entry = json.loads(raw_text.rstrip(","))
        except json.JSONDecodeError:
            try:
                entry = json.loads("[" + raw_text.rstrip(",") + "]")
            except json.JSONDecodeError as error:
                raise ValidationError(f"cannot parse XNU -MJ fragment {fragment}: {error}") from error
        entries = entry if isinstance(entry, list) else [entry]
        for item in entries:
            if not isinstance(item, dict):
                raise ValidationError(f"XNU -MJ fragment has a non-object entry: {fragment}")
            if isinstance(item.get("arguments"), list):
                tokens = [str(token) for token in item["arguments"]]
            elif isinstance(item.get("command"), str):
                try:
                    tokens = shlex.split(item["command"])
                except ValueError as error:
                    raise ValidationError(f"cannot parse command in XNU -MJ fragment {fragment}: {error}") from error
            else:
                raise ValidationError(f"XNU -MJ entry has neither arguments nor command: {fragment}")
            if "-mkernel" not in tokens or "-c" not in tokens:
                continue
            if not tokens or not compiler_path(tokens[0], expected_compilers):
                raise ValidationError(f"unexpected compiler for XNU target command in {fragment}: {tokens[:1]}")
            directory = canonical(item.get("directory", fragment.parent))
            source = str(item.get("file", ""))
            if not source:
                source_candidates = [token for token in tokens if pathlib.Path(token).suffix.lower() in SOURCE_SUFFIXES]
                if not source_candidates:
                    raise ValidationError(f"XNU -MJ target entry has no source file: {fragment}")
                source = source_candidates[-1]
            source_path = pathlib.Path(source)
            if not source_path.is_absolute():
                source_path = directory / source_path
            source = str(canonical(source_path))
            argument_sources = [token for token in tokens if pathlib.Path(token).suffix.lower() in SOURCE_SUFFIXES]
            if argument_sources:
                argument_source = pathlib.Path(argument_sources[-1])
                if not argument_source.is_absolute():
                    argument_source = directory / argument_source
                if canonical(argument_source) != pathlib.Path(source):
                    raise ValidationError(
                        f"XNU -MJ source field does not match compiler argument in {fragment}: "
                        f"{source} != {canonical(argument_source)}"
                    )
            language, _ = classify_source(tokens, source)
            commands.append({
                "tokens": tokens,
                "language": language,
                "source": source,
                "directory": directory,
                "includes": include_paths(tokens, directory),
                "sysroots": sysroot_paths(tokens),
                "depfiles": depfile_paths(tokens),
                "fragment": fragment,
            })
    return commands


def include_paths(tokens: list[str], directory: pathlib.Path) -> list[pathlib.Path]:
    paths = []
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token == "-I" and index + 1 < len(tokens):
            raw = pathlib.Path(tokens[index + 1])
            paths.append(canonical(raw if raw.is_absolute() else directory / raw))
            index += 2
            continue
        if token.startswith("-I") and len(token) > 2:
            raw = pathlib.Path(token[2:])
            paths.append(canonical(raw if raw.is_absolute() else directory / raw))
        index += 1
    return paths


def sysroot_paths(tokens: list[str]) -> list[pathlib.Path]:
    roots = []
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token == "-isysroot":
            if index + 1 >= len(tokens):
                raise ValidationError("XNU target clang command has -isysroot without a path")
            raw = pathlib.Path(tokens[index + 1])
            if not raw.is_absolute():
                raise ValidationError(f"XNU target clang command has a relative -isysroot: {raw}")
            roots.append(canonical(raw))
            index += 2
            continue
        if token.startswith("-isysroot"):
            raw = pathlib.Path(token[len("-isysroot"):])
            if not raw.is_absolute():
                raise ValidationError(f"XNU target clang command has an unsupported -isysroot form: {token}")
            roots.append(canonical(raw))
        index += 1
    return roots


def depfile_paths(tokens: list[str]) -> list[str]:
    paths = []
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token == "-MF":
            if index + 1 >= len(tokens):
                raise ValidationError("XNU target clang command has -MF without a path")
            paths.append(tokens[index + 1])
            index += 2
            continue
        if token.startswith("-MF") and len(token) > 3:
            paths.append(token[3:])
        index += 1
    return paths


def dependency_entries(path: pathlib.Path) -> list[str]:
    text = path.read_text(errors="replace").replace("\\\r\n", " ").replace("\\\n", " ")
    first_rule = next((line for line in text.splitlines() if line.strip()), "")
    _, separator, dependencies = first_rule.partition(":")
    if not separator:
        raise ValidationError(f"compiler dependency file has no rule separator: {path}")
    try:
        return shlex.split(dependencies)
    except ValueError as error:
        raise ValidationError(f"cannot parse compiler dependency file {path}: {error}") from error


def path_is_within(path: pathlib.Path, root: pathlib.Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def resolve_dependency(token: str, depfile: pathlib.Path, source_root: pathlib.Path, objroot: pathlib.Path,
                       command_directory: pathlib.Path) -> pathlib.Path:
    raw = pathlib.Path(token)
    if raw.is_absolute():
        return canonical(raw)
    candidates = [
        command_directory / raw,
        pathlib.Path.cwd() / raw,
        source_root / raw,
        depfile.parent / raw,
        objroot / raw,
    ]
    for candidate in candidates:
        resolved = canonical(candidate)
        if resolved.is_file():
            return resolved
    return canonical(candidates[0])


def resolve_depfile(token: str, objroot: pathlib.Path, source_root: pathlib.Path, source: str,
                    command_directory: pathlib.Path,
                    depfiles_by_name: dict[str, list[pathlib.Path]]) -> pathlib.Path:
    raw = pathlib.Path(token)
    if raw.is_absolute():
        return canonical(raw)
    direct_candidates = [command_directory / raw, pathlib.Path.cwd() / raw, source_root / raw, objroot / raw]
    for candidate in direct_candidates:
        resolved = canonical(candidate)
        if resolved.is_file():
            return resolved

    # XNU's -MF value is commonly relative to the object directory in which
    # Make invoked clang. Recover it under OBJROOT and prefer the path matching
    # the source's directory suffix if basenames are reused in several trees.
    if raw.name not in depfiles_by_name:
        depfiles_by_name[raw.name] = [canonical(path) for path in objroot.rglob(raw.name) if path.is_file()]
    matches = depfiles_by_name[raw.name]
    if len(matches) == 1:
        return matches[0]
    try:
        source_parent = pathlib.Path(source)
        if not source_parent.is_absolute():
            source_parent = source_root / source_parent
        suffix = source_parent.resolve().parent.relative_to(source_root).parts
    except ValueError:
        suffix = ()
    scored = []
    for match in matches:
        try:
            relative_parts = match.relative_to(objroot).parts
        except ValueError:
            continue
        score = 0
        for length in range(1, len(suffix) + 1):
            needle = suffix[-length:]
            if any(relative_parts[i:i + length] == needle for i in range(len(relative_parts) - length + 1)):
                score = length
        if score:
            scored.append((score, match))
    if scored:
        best = max(score for score, _ in scored)
        best_matches = [path for score, path in scored if score == best]
        if len(best_matches) == 1:
            return best_matches[0]
    if len(matches) > 1:
        raise ValidationError(f"ambiguous XNU dependency file {token!r} under {objroot}: {matches[:5]}")
    return canonical(direct_candidates[0])


def sdkroot_notes(log_text: str) -> list[pathlib.Path]:
    return [canonical(value) for value in re.findall(r"xcrun: note: SDKROOT = '([^']+)'", log_text)]


def validate_sysroot_contract(sysroots: list[pathlib.Path], sdk_root: pathlib.Path, overlay_note_count: int) -> str:
    if overlay_note_count < 1:
        raise ValidationError(f"XNU make log does not show SDKROOT resolving to isolated SDK {sdk_root}")
    wrong = [root for root in sysroots if root != sdk_root]
    if wrong:
        raise ValidationError(f"XNU target clang uses explicit sysroots {wrong}; expected only {sdk_root}")
    if not sysroots:
        return "sdkroot-resolved-no-explicit-isysroot"
    return "explicit-isysroot-overlay"


def validate_command_warning_policy(commands: list[dict[str, object]]) -> None:
    c_commands = [command for command in commands if command["language"] in {"c", "cxx"}]
    missing_werror = [command["source"] for command in c_commands if "-Werror" not in command["tokens"]]
    if missing_werror:
        raise ValidationError(f"XNU C/C++ target clang commands lack global -Werror: {missing_werror[:10]}")
    for exception in WARNING_EXCEPTIONS:
        missing = [command["source"] for command in c_commands if exception not in command["tokens"]]
        if missing:
            raise ValidationError(f"XNU C/C++ target clang commands lack {exception}: {missing[:10]}")
        before_werror = []
        for command in c_commands:
            tokens = command["tokens"]
            error_positions = [index for index, token in enumerate(tokens) if token == "-Werror"]
            exception_positions = [index for index, token in enumerate(tokens) if token == exception]
            if error_positions and exception_positions and max(exception_positions) < max(error_positions):
                before_werror.append(command["source"])
        if before_werror:
            raise ValidationError(f"XNU exception {exception} precedes global -Werror: {before_werror[:10]}")


def verify(log_path: pathlib.Path, objroot: pathlib.Path, sdk_root: pathlib.Path, base_sdk_root: pathlib.Path,
           source_root: pathlib.Path, diagnostic_path: pathlib.Path, compiler_paths: list[pathlib.Path]) -> dict[str, object]:
    objroot = canonical(objroot)
    sdk_root = canonical(sdk_root)
    base_sdk_root = canonical(base_sdk_root)
    source_root = canonical(source_root)
    log_text = log_path.read_text(errors="replace")
    notes = sdkroot_notes(log_text)
    overlay_note_count = sum(note == sdk_root for note in notes)
    base_note_count = sum(note == base_sdk_root for note in notes)
    expected_compilers = {canonical(path) for path in compiler_paths}
    commands = parse_commands(objroot, expected_compilers)
    if not commands:
        raise ValidationError(f"no XNU target clang compile commands found in {log_path}")

    no_explicit_sysroot_commands = 0
    explicit_sysroot_commands = 0
    sysroot_modes = []
    wrong_sysroots = []
    for command in commands:
        sysroots = command["sysroots"]
        try:
            mode = validate_sysroot_contract(sysroots, sdk_root, overlay_note_count)
        except ValidationError as error:
            wrong_sysroots.append((command["source"], str(error)))
            continue
        sysroot_modes.append(mode)
        if sysroots:
            explicit_sysroot_commands += 1
        else:
            no_explicit_sysroot_commands += 1
    if wrong_sysroots:
        raise ValidationError(f"XNU target compile commands use a wrong sysroot: {wrong_sysroots[:5]}")

    excluded_paths = {name: [sdk_root / suffix, base_sdk_root / suffix] for name, suffix in EXCLUDED_SUFFIXES.items()}
    private_include_root = sdk_root / "usr/local/include"
    commands_without_depfile = []
    dependency_files: dict[pathlib.Path, pathlib.Path] = {}
    depfiles_by_name: dict[str, list[pathlib.Path]] = {}
    excluded_search_paths = {name: [] for name in excluded_paths}
    for index, command in enumerate(commands, 1):
        includes = command["includes"]
        if private_include_root not in includes:
            raise ValidationError(f"XNU target command {index} ({command['source']}) lacks staged private include root {private_include_root}")
        for name, roots in excluded_paths.items():
            if any(include in roots for include in includes):
                excluded_search_paths[name].append((index, command["source"]))
        depfiles = command["depfiles"]
        if not depfiles:
            commands_without_depfile.append(index)
            continue
        for token in depfiles:
            path = resolve_depfile(token, objroot, source_root, command["source"],
                                   command["directory"], depfiles_by_name)
            dependency_files.setdefault(path, command["directory"])

    if commands_without_depfile:
        raise ValidationError(f"XNU target clang commands lack -MF dependency files: {commands_without_depfile[:10]}")
    if any(excluded_search_paths.values()):
        raise ValidationError(f"XNU target commands still search excluded generic SDK roots: {excluded_search_paths}")
    missing_depfiles = [path for path in dependency_files if not path.is_file()]
    if missing_depfiles:
        raise ValidationError(f"XNU target clang dependency files are missing: {missing_depfiles[:10]}")
    if not dependency_files:
        raise ValidationError("no XNU target dependency files were resolved")

    dependency_entries_seen = []
    missing_dependencies = []
    excluded_dependencies = {name: [] for name in excluded_paths}
    for depfile, command_directory in dependency_files.items():
        for token in dependency_entries(depfile):
            resolved = resolve_dependency(token, depfile, source_root, objroot, command_directory)
            dependency_entries_seen.append((depfile, token, resolved))
            if not resolved.is_file():
                missing_dependencies.append((depfile, token, resolved))
            for name, roots in excluded_paths.items():
                if any(path_is_within(resolved, root) for root in roots):
                    excluded_dependencies[name].append((depfile, resolved))
    if not dependency_entries_seen:
        raise ValidationError("XNU dependency files contain no entries")
    if missing_dependencies:
        raise ValidationError(f"XNU compiler dependency files reference missing inputs: {missing_dependencies[:10]}")
    if any(excluded_dependencies.values()):
        details = {name: entries[:3] for name, entries in excluded_dependencies.items() if entries}
        raise ValidationError(f"XNU target dependencies unexpectedly reference removed SDK roots: {details}")

    header_suffixes = {".h", ".hh", ".hpp", ".hxx", ".inc", ".def"}
    header_dependencies = [entry for entry in dependency_entries_seen if pathlib.Path(entry[2]).suffix.lower() in header_suffixes]
    overlay_header_dependencies = [entry for entry in header_dependencies if path_is_within(entry[2], sdk_root)]
    if not header_dependencies:
        raise ValidationError("XNU dependency files contain no header dependencies")

    validate_command_warning_policy(commands)
    c_command_count = sum(command["language"] in {"c", "cxx"} for command in commands)
    assembly_command_count = sum(command["language"] == "assembly" for command in commands)
    if c_command_count == 0:
        raise ValidationError("XNU build log contains no C/C++ target commands")
    if assembly_command_count == 0:
        raise ValidationError("XNU build log contains no assembly target commands")

    private_headers = list(private_include_root.rglob("*.h")) if private_include_root.is_dir() else []
    sdkroot_resolution_mode = sysroot_modes[0] if len(set(sysroot_modes)) == 1 else "mixed-explicit-and-sdkroot"
    diagnostics: list[str] = [
        f"target_xnu_compile_commands={len(commands)}",
        f"target_xnu_c_cxx_compile_commands={c_command_count}",
        f"target_xnu_assembly_compile_commands={assembly_command_count}",
        f"target_commands_without_explicit_isysroot={no_explicit_sysroot_commands}",
        f"target_commands_with_explicit_overlay_isysroot={explicit_sysroot_commands}",
        f"target_commands_with_wrong_isysroot={len(wrong_sysroots)}",
        f"sdkroot_overlay_notes={overlay_note_count}",
        f"sdkroot_base_notes={base_note_count}",
        f"target_sdkroot_resolution_mode={sdkroot_resolution_mode}",
        f"target_commands_without_depfile={len(commands_without_depfile)}",
        f"target_dependency_files={len(dependency_files)}",
        f"missing_target_dependency_files={len(missing_depfiles)}",
        f"target_dependency_file_entries={len(dependency_entries_seen)}",
        f"target_header_dependencies={len(header_dependencies)}",
        f"overlay_header_dependencies={len(overlay_header_dependencies)}",
        f"missing_dependency_files={len(missing_dependencies)}",
        f"excluded_sdk_header_dependencies={sum(map(len, excluded_dependencies.values()))}",
        f"target_commands_with_werror={sum('-Werror' in c['tokens'] for c in commands if c['language'] in {'c','cxx'})}",
        f"target_commands_with_reserved_identifier_policy={sum(WARNING_EXCEPTIONS[0] in c['tokens'] for c in commands if c['language'] in {'c','cxx'})}",
        f"target_commands_with_unused_but_set_policy={sum(WARNING_EXCEPTIONS[1] in c['tokens'] for c in commands if c['language'] in {'c','cxx'})}",
        f"target_commands_with_void_pointer_enum_policy={sum(WARNING_EXCEPTIONS[2] in c['tokens'] for c in commands if c['language'] in {'c','cxx'})}",
        f"private_usr_local_include_root={private_include_root} directory_exists={private_include_root.is_dir()} headers={len(private_headers)}",
    ]
    for name, suffix in EXCLUDED_SUFFIXES.items():
        for label, root in (("overlay", sdk_root / suffix), ("base_sdk", base_sdk_root / suffix)):
            headers = list(root.rglob("*.h")) if root.is_dir() else []
            diagnostics.append(f"{name}_{label}_excluded_include_path={root} directory_exists={root.is_dir()} headers={len(headers)} dependency_references={len(excluded_dependencies[name])}")
    for depfile, token, resolved in missing_dependencies[:20]:
        diagnostics.append(f"missing_dependency_file={depfile} token={token} resolved={resolved}")
    for depfile, token, resolved in header_dependencies:
        diagnostics.append(f"header_dependency={depfile} token={token} resolved={resolved} exists={resolved.is_file()} overlay={path_is_within(resolved, sdk_root)}")
    for index, command in enumerate(commands, 1):
        selected = [str(path) for path in command["includes"] if path == private_include_root or any(path in roots for roots in excluded_paths.values())]
        diagnostics.append(f"target_command_{index}_source={command['source']} language={command['language']} sysroots={' '.join(map(str, command['sysroots']))}")
        diagnostics.append(f"target_command_{index}_sdk_include_paths={' '.join(selected)}")

    diagnostic_path.parent.mkdir(parents=True, exist_ok=True)
    diagnostic_path.write_text("\n".join(diagnostics) + "\n", encoding="utf-8")
    return {
        "mode": sdkroot_resolution_mode,
        "sdkroot_overlay_notes": overlay_note_count,
        "sdkroot_base_notes": base_note_count,
        "target_commands_without_explicit_isysroot": no_explicit_sysroot_commands,
        "target_commands_with_explicit_overlay_isysroot": explicit_sysroot_commands,
        "overlay_header_dependencies": len(overlay_header_dependencies),
        "target_dependency_files": len(dependency_files),
        "target_dependency_file_entries": len(dependency_entries_seen),
        "target_header_dependencies": len(header_dependencies),
        "missing_dependency_files": len(missing_dependencies),
        "c_cxx_compile_commands": c_command_count,
        "assembly_compile_commands": assembly_command_count,
    }


def _expect_failure(callable_, label: str) -> None:
    try:
        callable_()
    except ValidationError:
        return
    raise ValidationError(f"self-test expected failure: {label}")


def self_test() -> None:
    with tempfile.TemporaryDirectory(prefix="xnu-target-verifier-") as temporary:
        root = pathlib.Path(temporary)
        source_root = root / "source"
        objroot = root / "obj"
        sdk_root = root / "overlay"
        base_sdk_root = root / "base-sdk"
        compiler = root / "toolchain" / "clang"
        source_root.mkdir()
        objdir = objroot / "DEVELOPMENT_X86_64" / "bsd" / "DEVELOPMENT"
        objdir.mkdir(parents=True)
        include_root = sdk_root / "usr/local/include"
        private_header = include_root / "os/atomic_private.h"
        private_header.parent.mkdir(parents=True)
        private_header.write_text("/* staged private header */\n")
        compiler.parent.mkdir(parents=True)
        compiler.write_text("#!/bin/sh\nexit 0\n")
        source_c = source_root / "bsd/probe.c"
        source_c.parent.mkdir(parents=True)
        source_c.write_text("int probe;\n")
        source_asm = source_root / "osfmk/start.s"
        source_asm.parent.mkdir(parents=True)
        source_asm.write_text("/* assembly fixture */\n")
        c_dep = objdir / "probe.d"
        asm_dep = objdir / "start.d"
        c_dep.write_text(f"probe.o: {source_c} {private_header}\n")
        asm_dep.write_text(f"start.o: {source_asm} {private_header}\n")
        common = ["-c", "-mkernel", "-I", str(include_root)]
        c_args = [
            str(compiler), *common, "-Werror", *WARNING_EXCEPTIONS,
            "-MF", "probe.d", str(source_c),
        ]
        asm_args = [str(compiler), *common, "-MF", "start.d", str(source_asm)]
        log = root / "build.log"
        diagnostic = root / "diagnostics.log"

        def write_log(*commands: list[str], notes: tuple[pathlib.Path, ...] = (sdk_root, base_sdk_root)) -> None:
            lines = [f"xcrun: note: SDKROOT = '{path}'" for path in notes]
            lines.extend(shlex.join(command) for command in commands)
            log.write_text("\n".join(lines) + "\n")
            for stale in objdir.glob("*.o.json"):
                stale.unlink()
            for command in commands:
                source = pathlib.Path(command[-1])
                fragment = objdir / f"{source.stem}.o.json"
                entry = {
                    "directory": str(objdir),
                    "arguments": command,
                    "file": str(source),
                    "output": str(objdir / f"{source.stem}.o"),
                }
                fragment.write_text(json.dumps(entry) + ",\n")

        write_log(c_args, asm_args)
        result = verify(log, objroot, sdk_root, base_sdk_root, source_root, diagnostic, [compiler])
        if result["mode"] != "sdkroot-resolved-no-explicit-isysroot":
            raise ValidationError(f"self-test expected effective SDKROOT mode, got {result['mode']}")
        if result["c_cxx_compile_commands"] != 1 or result["assembly_compile_commands"] != 1:
            raise ValidationError(f"self-test failed to separate C and assembly: {result}")

        explicit_overlay_args = [*c_args]
        explicit_overlay_args[explicit_overlay_args.index("-Werror"):explicit_overlay_args.index("-Werror")] = [
            "-isysroot", str(sdk_root)
        ]
        write_log(explicit_overlay_args, asm_args)
        result = verify(log, objroot, sdk_root, base_sdk_root, source_root, diagnostic, [compiler])
        if result["mode"] != "mixed-explicit-and-sdkroot":
            raise ValidationError(f"self-test expected mixed explicit/implicit overlay mode, got {result['mode']}")

        wrong_sysroot_args = [*c_args]
        wrong_sysroot_args[wrong_sysroot_args.index("-Werror"):wrong_sysroot_args.index("-Werror")] = ["-isysroot", str(base_sdk_root)]
        write_log(wrong_sysroot_args, asm_args)
        _expect_failure(lambda: verify(log, objroot, sdk_root, base_sdk_root, source_root, diagnostic, [compiler]), "wrong explicit target sysroot")

        no_sdk_notes_log = [f"xcrun: note: SDKROOT = '{base_sdk_root}'"]
        no_sdk_notes_log.extend((shlex.join(c_args), shlex.join(asm_args)))
        log.write_text("\n".join(no_sdk_notes_log) + "\n")
        _expect_failure(lambda: verify(log, objroot, sdk_root, base_sdk_root, source_root, diagnostic, [compiler]), "missing overlay SDKROOT evidence")

        missing_werror = [token for token in c_args if token != "-Werror"]
        write_log(missing_werror, asm_args)
        _expect_failure(lambda: verify(log, objroot, sdk_root, base_sdk_root, source_root, diagnostic, [compiler]), "C command without global Werror")

        excluded = sdk_root / EXCLUDED_SUFFIXES["Kernel_framework_platform"]
        excluded.mkdir(parents=True)
        excluded_header = excluded / "fixture.h"
        excluded_header.write_text("/* forbidden dependency */\n")
        c_dep.write_text(f"probe.o: {source_c} {private_header} {excluded_header}\n")
        write_log(c_args, asm_args)
        _expect_failure(lambda: verify(log, objroot, sdk_root, base_sdk_root, source_root, diagnostic, [compiler]), "dependency in an excluded SDK subtree")

    print("XNU target verifier self-tests passed: effective SDKROOT, rejected wrong SDK, assembly warning split, C/C++ Werror, and dependency closure")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--log", type=pathlib.Path)
    parser.add_argument("--objroot", type=pathlib.Path)
    parser.add_argument("--sdk-root", type=pathlib.Path)
    parser.add_argument("--base-sdk-root", type=pathlib.Path)
    parser.add_argument("--source-root", type=pathlib.Path)
    parser.add_argument("--diagnostic", type=pathlib.Path)
    parser.add_argument("--clang", type=pathlib.Path)
    parser.add_argument("--clangxx", type=pathlib.Path)
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    required = (args.log, args.objroot, args.sdk_root, args.base_sdk_root, args.source_root,
                args.diagnostic, args.clang, args.clangxx)
    if any(value is None for value in required):
        parser.error("build verification requires --log, --objroot, --sdk-root, --base-sdk-root, --source-root, --diagnostic, --clang, and --clangxx")
    try:
        result = verify(args.log, args.objroot, args.sdk_root, args.base_sdk_root,
                        args.source_root, args.diagnostic, [args.clang, args.clangxx])
    except (OSError, ValidationError) as error:
        if args.diagnostic is not None:
            args.diagnostic.parent.mkdir(parents=True, exist_ok=True)
            with args.diagnostic.open("a", encoding="utf-8") as diagnostic:
                diagnostic.write(f"verification_failure={str(error).replace(chr(10), ' ')}\n")
        print(f"XNU target build verification failed: {error}", file=sys.stderr)
        return 1
    print(
        f"Verified XNU target compile commands: C/C++={result['c_cxx_compile_commands']}, "
        f"assembly={result['assembly_compile_commands']}; SDK mode={result['mode']}; "
        f"SDKROOT overlay notes={result['sdkroot_overlay_notes']}; no explicit sysroot="
        f"{result['target_commands_without_explicit_isysroot']}; overlay header dependencies="
        f"{result['overlay_header_dependencies']}; all {result['target_dependency_file_entries']} dependency entries exist "
        f"({result['target_dependency_files']} .d files)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
