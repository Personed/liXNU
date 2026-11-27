# x86_64 Mach-O inspection helper

`xnu-macho-inspect` is a host-side, read-only inspector for the bounded x86_64
`MH_EXECUTE` Mach-O subset accepted by `xnu_macho_x86_64_parse()`. Build it
from the repo root with:

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
even when their display strings collide. The report uses the parser's
validated descriptors, in load-command order, and is bounded to 24 segments.

Each segment's `chained_starts` is `null` when it has no starts record.
Otherwise the report includes the Mach-O `pointer_format` number,
`page_size` in bytes, and `page_count` from the starts record.
`linkedit_metadata.chained_starts_segment_count` counts slots in the encoded
starts table, including empty slots.
`linkedit_metadata.chained_starts_nonempty_segment_count` counts slots with
records, including records whose pages have no active chains. Both counts
are zero without an `LC_DYLD_CHAINED_FIXUPS` command. `page_count` describes
the page array in the record, which may cover only a prefix of the segment.

An unsupported ABI or required command, or a malformed range, exits nonzero
without printing a report. Inputs must be regular files no larger than 1 GiB.

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
