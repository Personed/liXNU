# x86_64 Mach-O inspection helper

`xnu-macho-inspect` is a host-side, read-only inspector for the bounded x86_64
Mach-O subset accepted by `xnu_macho_x86_64_parse()`. Build it from the repo
root with:

```sh
cc -std=c11 -Wall -Wextra -Werror \
  tools/dev/x86_64/xnu-macho-inspect.c \
  xnu-ko/macho_parse_x86_64.c \
  -o /tmp/xnu-macho-inspect
```

Then inspect a candidate XNU kernel artifact:

```sh
/tmp/xnu-macho-inspect /path/to/kernel.development
```

On success it writes one JSON object with the accepted architecture/subtype,
the unslid RIP from `LC_UNIXTHREAD`, the aggregate VM range, and each
`LC_SEGMENT_64` name, file/VM range, protections, flags, and section count.
Addresses and offsets are printed as hexadecimal strings to preserve all 64
bits. Valid UTF-8 paths are preserved; invalid path bytes are represented as
their U+00XX code points for display and labeled in `file_path_encoding`.
`file_path_bytes_hex` always preserves the exact path bytes. Segment names are
displayed similarly, with each fixed 16-byte field preserved in
`name_bytes_hex`; this keeps distinct non-UTF-8 byte sequences distinguishable
even when their display strings collide. Chained fixups, dyld info, and other
linkedit payloads are surfaced as metadata only. An
unsupported ABI/required command, malformed range, or disagreement between
the parser and the bounded metadata walk exits nonzero without printing a
partial report. Inputs must be regular files no larger than 1 GiB; the helper
also limits output to 65,536 segments.

The helper reads file bytes into ordinary process memory for inspection only.
It does not map executable image segments, allocate kernel memory, relocate or
rebase pointers, interpret fixups, change protections, or execute XNU. A valid
report confirms only the parser's structural subset; it does not establish
that the image is loadable or runnable.

The synthetic fixtures run with:

```sh
tests/x86_64/test-macho-inspect.sh
```

## Verifying a built kernel artifact

`verify-xnu-artifact.py` combines the inspector with the matching XNU
`build-result.json` for the Tahoe 26.5 source build. It requires manifest
schema version 1, the pinned XNU commit/tag, source release, SDK, Xcode and
runtime target fields, plus the passed artifact's exact size and SHA-256. The
old absolute `artifact` path is not used. The CPU subtype must be selected
explicitly; it is not inferred from the parser's support list. The structural
profile requires a unique `__TEXT` segment at file offset zero, read/execute
protections, at least one section, one leading optional `__PAGEZERO`, unique
segment names, a file-backed RIP in `__TEXT`, and no unclassified optional
load commands. It also verifies that the manifest reports the pinned XNU
source patch and compatibility patch hashes, the expected non-Intel vendor
policy, and a native x86_64 build host/process.

```sh
python3 tools/dev/x86_64/verify-xnu-artifact.py \
  --output /tmp/xnu-artifact-report.json \
  --expected-subtype X86_64_ALL \
  /path/to/kernel.development /path/to/build-result.json
```

The verifier compiles and runs the inspector from this checkout, on a private,
read-only snapshot. It hashes the snapshot before and after inspection. The
report is structural preflight evidence only: the parser validates selected
Mach-O headers, segment/section ranges, thread state, entry range and relevant
linkedit command bounds. For chained-fixup metadata it validates selected
formats and table/range bounds, but does not resolve pointer values, relocate
the image, or write fixups. It does not perform a real load, establish
XNU/Linux ABI compatibility, or boot the kernel. Data in the
manifest about runtime targets is recorded as build metadata and is not runtime
evidence. Unknown optional load commands are rejected because the inspector
cannot classify their semantics.

The `__TEXT` layout profile is a provisional policy pending a run against the
actual #130 artifact. Synthetic accepted and rejected cases exercise the
policy, but do not establish that the real kernel artifact satisfies it. Run
the cases with:

```sh
tests/x86_64/test-xnu-artifact-verifier.sh
```

The CI integration runs this preflight in the Intel-native build job and
uploads its structural report with that job's artifact. It does not inspect or
include the separate Apple Silicon cross-build artifact.

No Apple image or firmware is included in this repository.
