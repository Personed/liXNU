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
their U+00XX code points and labeled in `file_path_encoding`. Chained fixups,
dyld info, and other linkedit payloads are surfaced as metadata only. An
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

No Apple image or firmware is included in this repository.
