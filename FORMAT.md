# InstallerVISE format notes

This documents the subset implemented by `unvise`. Unless noted otherwise:

- Integers are unsigned and big-endian.
- Offsets prefixed with `+` are relative to the start of a record.
- Catalog names use MacRoman.
- Record boundaries are computed from revision-specific fixed structures and
  declared variable-field lengths.

## Input forks

An installer is a classic Macintosh file with two forks:

| Fork | Contents | Required |
| --- | --- | --- |
| Data | `SVCT` header, payload members, catalog | yes |
| Resource | `DATA`/`CODE` decompressor initialization through VISE 7; application resources in VISE 8 | yes |

Accepted representations:

- Native macOS data and resource forks.
- `Installer` plus `._Installer` AppleDouble from `unar -k hidden`.
- Raw `Installer.data` plus `Installer.rsrc` from `macunpack -f`.

MacBinary, BinHex, and StuffIt are outer formats and must be removed first.

## SVCT data fork

| Offset | Size | Meaning |
| ---: | ---: | --- |
| `0x00` | 4 | `SVCT` signature |
| `0x04` | 4 | archive version (`1` in supported files) |
| `0x10` | 4 | format flags and revision; low byte is the catalog revision |
| `0x24` | 4 | catalog offset |

The catalog starts with `CVCT` and contains `CVCT`, `DVCT`, `FVCT`, and `PACK`
records.

`PACK` is an ordinary catalog record and can occur in an uncompressed catalog.
Catalog compression is indicated by the nonzero field at `CVCT+0x08`, not by
the presence of a `PACK` record.

`CVCT+0x10` contains the number of `DVCT` and `FVCT` records. Fixed body sizes
depend on the catalog revision. Variable tails are calculated from declared
lengths and the record subtype, then the expected next signature must occur at
the computed boundary. A signature is never searched for, so embedded tag-like
bytes cannot create a record.

## CVCT catalog header

| Offset | Size | Meaning |
| ---: | ---: | --- |
| `+0x00` | 4 | `CVCT` signature |
| `+0x04` | 4 | compressed stream length when `+0x08` is nonzero |
| `+0x08` | 4 | nonzero when the catalog is compressed |
| `+0x10` | 2 | number of `DVCT` and `FVCT` records |
| `+0x64` | variable | word-swapped raw-DEFLATE stream when compressed |

A compressed catalog expands to ordinary `DVCT`, `FVCT`, and `PACK` records.
This storage choice is independent of the record-body layout. VISE 7.0.1, for
example, can store the same wide record bodies either directly or compressed.

`SVCT+0x14` is the number of media segments. In a multi-segment installer,
`FVCT+0x62` selects the segment containing the member. The catalog-bearing
application can contain segment-1 members itself; records assigned to another
segment, or whose member extends beyond the application's payload area, are
external until the corresponding segment file is supplied.

Some single-segment installers keep the payload bytes in a sibling file named
`Installer.data`. Payload offsets address that companion directly; `unvise`
uses it automatically when it is present beside the catalog-bearing
application.

Layout selection uses the low revision byte and the `CVCT+0x08` compression
field; it does not inspect catalog contents for a likely structure:

| Layout | Revision and storage | InstallerVISE versions |
| --- | --- | --- |
| Lite | revision 0, uncompressed | Lite 3.6 |
| Short | generations 0 and 2, revisions 1 or 2, uncompressed | early VISE archives |
| Compact | revisions 2, 3, or 4, uncompressed | 4.2, 4.5, 4.6.1 |
| Normal | revisions 5 through 9 when uncompressed | 5.0.1, 5.5, 5.5.1, 5.5.2, 6.0, 6.0.1 |
| Wide | revisions 5 through 11 when compressed; revisions 10 and 11 when uncompressed | 6.5, 7.0, 7.0.1, 7.2, 7.3 |
| Late | revisions 12 through 14, compressed with later bodies | 7.4, 8.0.2, 8.5 |

Revision 13 file/action catalogs are supported; no revision-13 directory record
has been observed, so its `DVCT` body size remains unknown. VISE 5.0.1 uses a
normal uncompressed catalog even when it contains a `PACK` record.

## DVCT directory record

### Normal catalog

| Offset | Size | Meaning |
| ---: | ---: | --- |
| `+0x00` | 4 | `DVCT` signature |
| `+0x04` | 16 | classic directory Finder information |
| `+0x14` | 4 | classic Mac creation time |
| `+0x18` | 4 | classic Mac modification time |
| `+0x1c` | 4 | directory ID |
| `+0x20` | 4 | parent directory ID |
| `+0x50` | 1 | primary trailing-name length |
| `+0x94` | variable | MacRoman name |

For a compressed catalog, the name moves to `+0x98`; late records use `+0xa0`
or `+0xa4`.

The late layout stores records in preorder and gives each directory an explicit
nesting depth in the high 16 bits at `+0x48`. Directory IDs remain at `+0x1c`,
but the preorder depth is sufficient to reconstruct the tree.

Revision 12 uses a `0xa0`-byte directory body; revision 14 uses `0xa4`. The
primary and optional secondary trailing-name lengths are bytes at `+0x50` and
`+0x4f` respectively.

Directory `+0x04..+0x13` contains Finder information; `+0x14` and `+0x18`
contain creation and modification times. Directory metadata is applied after
all children, in reverse catalog order, so creating a child cannot replace the
restored parent mtime.

### Compact catalog

- VISE 4.2, 4.5, and 4.6.1 installers use this layout.
- Fixed body sizes are selected by the low revision byte at `SVCT+0x13`.
- Revisions 2 and 3 use `0x58`-byte directory bodies; revision 4 uses `0x68`.
- Directory and parent IDs remain at `+0x1c` and `+0x20`.
- The primary name length remains at `+0x50`.
- The MacRoman name follows the revision-specific fixed body.

### Lite 3.6 short catalog

- The record has a `0x58`-byte directory body followed by declared Pascal
  fields.
- Directory and parent IDs remain at `+0x1c` and `+0x20`.
- The fixed body ends at `+0x58`; the primary name length is at `+0x50`.

## FVCT file or action record

### Common fields

| Offset | Size | Meaning |
| ---: | ---: | --- |
| `+0x00` | 4 | `FVCT` signature |
| `+0x04` | 4 | type or flags |
| `+0x0c` | 4 | record flags; classic bit 4 marks an installer action |
| `+0x2c` | 16 | classic Finder information: type, creator, flags, location, folder |
| `+0x3c` | 4 | classic Mac creation time |
| `+0x40` | 4 | classic Mac modification time |
| `+0x44` | 4 | packed data-fork size |
| `+0x48` | 4 | expanded data-fork size |
| `+0x4c` | 4 | packed resource-fork size |
| `+0x50` | 4 | expanded resource-fork size |
| `+0x54` | 4 | CRC-32 of expanded forks for a self-contained file |
| `+0x58` | 4 | parent directory ID or install-location token |
| `+0x60` | 4 | payload mode in the wide layout |
| `+0x64` | 4 | payload offset in the data fork |
| `+0x68` | 4 | data-fork offset in a shared or combined expanded member |
| `+0x6c` | 4 | resource-fork offset in that member |
| `+0x7a` | 1 | primary trailing-name length |
| `+0xba` | variable | MacRoman name |

For a compressed catalog, the name moves to `+0xbe`; the late layout moves it
eight bytes farther to `+0xc6`. The fork and payload fields do not move.

In the late layout, the high 16 bits at `+0x60` are the preorder nesting depth.
VISE 8.5 repurposes `+0x58`; it must not be interpreted as a parent ID.
Hierarchy is reconstructed from the explicit depth fields.

The primary trailing-name length is consistently the byte at `+0x7a`. The
late fixed file/action body is `0xc6` bytes and can carry an optional
secondary-name length at `+0xb9`. Classic bit 4 at `+0x0c` identifies an
action and controls whether the byte
at `+0x7b` and subtype-specific action fields follow the primary name. Classic
bit numbering makes this mask `0x08000000` when the four bytes are read as a
big-endian integer. The secondary name is read afterward. File records have
this bit clear. Timestamps use the classic Mac 1904 epoch.

Late action tails additionally use these subtype fields:

- normal-layout subtype 2 appends `be16(+0x38)` bytes after its two declared
  names;
- subtype 14 appends `be16(+0x38)` bytes;
- subtype 15 appends the byte counts at `+0xc1` and `+0xc5`;
- subtype 17 appends `be16(+0x38)` bytes.

Some generation-0 short catalogs address an embedded copy of the corresponding
`FVCT` record; others address the member directly. The embedded form is
self-identifying by its `FVCT` signature, and every payload in the catalog must
use the same form. Its header length is computed with the same fixed body and
trailing-name fields as the catalog copy. Generation 2 uses direct offsets in
the observed corpus.

### Compact catalog

- Fork sizes and the payload offset retain their common offsets.
- The parent directory ID is at `+0x60`.
- Revisions 2 and 3 use `0x7c`-byte file/action bodies; revision 4 uses `0x8c`.
- The MacRoman name follows the revision-specific fixed body.
- Some localized self-installer records contain no usable trailing filename.
  Their shared payload members are still distinct; output paths use the stable
  catalog record number so no variant overwrites another.

### Lite 3.6 short catalog

- The record has a `0x7c`-byte file/action body followed by declared Pascal
  fields.
- Fork sizes and payload offset retain their common offsets.
- The fixed body ends at `+0x7c`; the primary name length is at `+0x7a`.
- `+0x58` is the destination reference. It usually names a `DVCT` ID.
- This is not unconditionally a catalog directory ID.
- A custom folder icon (`Icon\r`, Finder type `icon`, creator `MACS`) carries
  that runtime object rather than a `DVCT` ID. The operation applies to the
  immediately preceding directory in catalog preorder. `unvise` recognizes
  this case explicitly and rejects other unresolved destination objects.

### Action records

`FVCT` also represents installer operations rather than files. Known
identifiers include:

- Type `0x00008000`: message or location action.
- Type `0x03xxxxxx`: folder-search action.
- Packed fields `0x00010001`, `0x00020001`, or `0x00040001`, together with
  resource packed field `0x00010001`: parameter fields used by search/delete
  actions in VISE 4.2 and 5.5.2.

These parameter values are not compressed sizes, even when several records
share the apparent payload offset `0x2c`.

### Shared payloads

Several `FVCT` records may reference one compressed member:

- Records are grouped by equal payload offset.
- `FVCT+0x68` and `FVCT+0x6c` locate each fork in the expanded member.
- Expanded fork sizes delimit the ranges beginning at those offsets.
- A group containing data forks stores the compressed member size in packed
  field 0 and its complete expanded size in packed field 1. Declared fork
  offsets can leave installer-private gaps in that expansion.
- A resource-only group stores its compressed member size in packed field 1;
  its declared resource-fork sizes determine the expanded size.
- Late data-bearing groups use the first form, including data-only members.

### Conditional path collisions

- Multiple file records may intentionally target the same logical pathname.
  They represent alternatives selected by installer conditions, packages,
  architecture, localization, or version checks.
- Alternatives are not assumed identical.
- Records whose complete set of forks is byte-identical to an earlier record
  at the same logical pathname are omitted. The lowest-numbered distinct
  record retains the logical pathname. Later distinct records receive a
  `~NNNN` suffix, where `NNNN` is the stable catalog record number. Data and
  resource forks from one record retain the same suffix.

### Late framed and overlapping payloads

- Shared payload field 0 holds the compressed member size.
- Shared payload field 1 holds the complete expanded size.
- The common fork-offset fields are authoritative; catalog order is not used
  to divide framed members.
- Bytes not covered by a catalog record are not output.
- A single data-only record can use the same framing and offset fields.
- A subtype-6 resource-update record can share the following complete member's
  payload offset while giving an earlier packed endpoint. The shorter endpoint
  does not terminate a DEFLATE stream, and its `+0x54` field is not a fork CRC.
  It is installer policy rather than a self-contained file and is not emitted.

### Base-dependent update members

Some wide and late catalogs contain same-name records with identical expanded
sizes, Finder information, and reconstructed parents:

- A self-contained record has the low payload-mode bit set at `+0x60`,
  compressed bytes, and a CRC-32 at `+0x54`.
- Its alternative has the low payload-mode bit clear and a small selector at
  `+0x54` rather than a CRC-32. Higher mode bits differ between loader builds.
- The alternative is a DEFLATE-family update stream whose history is
  initialized from the existing installed fork. It is not independently
  extractable and is listed as `base-dependent` rather than emitted.
- The installer can therefore update a matching installed file cheaply or use
  the paired self-contained member for a fresh installation.

### File checksums

- For self-contained files, `FVCT+0x54` is the CRC-32 of the expanded data fork
  followed by the expanded resource fork.
- The checksum excludes compression framing, shared-member gaps, and
  installer-private bytes outside the declared fork ranges.
- `unvise` verifies it before accepting an extracted file.
- A loader-marked file whose four fork size fields are zero is an empty file,
  not an absent payload. Its checksum is verified as the CRC of an empty byte
  stream and an empty data fork is emitted so the catalog entry is retained.

### VISE 7 offset-fork payload

One member may contain:

```text
installer-private prefix | data fork | resource fork
```

| Field | Meaning |
| --- | --- |
| `+0x48` | expanded data-fork size |
| `+0x50` | expanded resource-fork size |
| `+0x68` | data-fork offset in the expanded member |
| `+0x6c` | resource-fork offset in the expanded member |
| `+0x4c` | complete expanded member size in this layout |

The prefix is not a second file form. The CRC at `+0x54` covers the two forks
at their declared offsets and excludes the prefix.

## Resource-fork initialization

### Lite 3.6

- `DATA` resource 0 contains the 256-byte substitution permutation directly.
- Its position is not fixed; it is identified as a 256-byte permutation.
- No `CODE` 24 resource is required.

### VISE 4.2 through 7.0

- `DATA` resource 0 is normally an `A89F000C` packed-code stream.
- Some 4.5 and 4.6.1 self-installers instead store the permutation literally;
  this does not make their catalogs Lite records.
- A VISE 8.0.2 68K installer stores the three initialization streams directly
  in `DATA 0`, without either the packed-code wrapper or a literal table.
- VISE 4.5 uses `CODE` 18 as its word dictionary and places the permutation
  directly in the expanded resource.
- `CODE` 24 normally supplies its word dictionary.
- Some VISE 5.5.1 applications place that unpacker and dictionary in
  uncompressed `CODE` 5002 instead. Its code and four word-table headers match
  the `CODE` 24 implementation; only the resource ID and table contents differ.
- VISE 7 uses `CODE` 23 instead.
- Some VISE 5 and 6 self-installers pack `CODE` 24 and put the required
  dictionary in uncompressed `CODE` 25 or `CODE` 1002.
- Expanding `DATA` 0 produces three initialization streams.
- The streams initialize a 256-byte substitution permutation in an emulated
  signed 16-bit A5-relative address space.
- `DATA 0` does not identify the table by offset or symbol. Its A5-relative
  address varies between builds, so it is identified as the sole initialized
  256-byte permutation.

The initializer command byte is decoded exhaustively:

| Command | Meaning |
| ---: | --- |
| `0x00` | end of stream |
| `0x01` to `0x04` | four relocation templates |
| `0x05` to `0x0f` | reserved; rejected |
| `0x10` to `0x1f` | emit a run of `0xff` |
| `0x20` to `0x3f` | emit a repeated following byte |
| `0x40` to `0x7f` | advance over zero-initialized storage |
| `0x80` to `0xff` | copy literal bytes |

### VISE 7.3

- `DATA` 0 and its initialization program are absent.
- The permutation is read from the initialized data of the input PEF
  application, as in VISE 8.
- Catalog records retain the wide-layout name offsets
  (`DVCT+0x98` and `FVCT+0xbe`).
- Catalog records have fixed `0x94`-byte `DVCT` and `0xba`-byte `FVCT` bodies.
  The low SVCT revision byte is 11; the late layout starts at revision 12.
- `Dcmp` 1005 implements DEFLATE with a 64 KiB circular history buffer. Its
  stored-block reader accepts transformed input as big-endian 16-bit words;
  one host path reverses the two bytes in each word before decoding.
- The same decoder can begin with caller-supplied history, which implements the
  base-dependent resource updates described above.

### VISE 8.0.2

- `DATA` 0 and its initialization program are absent.
- The main PPC PEF application begins at data-fork offset `0x30`.
- Expanding its packed-data section exposes the complete 256-byte permutation
  as a static byte array.
- The table belongs to the compiled application rather than to an
  InstallerVISE archive structure.
- `Dcmp` 1005, labeled `Version 27 (PPC, decomp)`, obtains already-transformed
  16-bit words through the host application's read callback. It implements the
  DEFLATE-family decoder but does not contain or generate the permutation.
- `unvise` parses the input PEF section headers, expands Apple's standard PEF
  packed-data opcodes, and locates the sole 256-byte permutation in the
  expanded section. No substitution table is built into `unvise`.
- A candidate is accepted only when all 256 byte values occur exactly once.
  Catalog CRC-32 values validate the selected table during extraction.

Relevant PEF structures:

| Location | Size | Meaning |
| ---: | ---: | --- |
| PEF `+0x20` | 2 | section count |
| PEF `+0x28` | 28 each | section headers |
| section `+0x0c` | 4 | expanded initialized size |
| section `+0x10` | 4 | packed container size |
| section `+0x14` | 4 | container offset from the PEF header |
| section `+0x18` | 1 | section kind; `2` is packed data |

The packed-data control byte uses its upper three bits as an opcode and lower
five bits as a count. A zero count is followed by a base-128 variable-length
count. Opcodes used by the PEF specification are zero fill, literal block,
repeated block, repeated-common/interleaved-literal, and
repeated-zero/interleaved-literal.

## A89F000C packed code

| Offset | Size | Meaning |
| ---: | ---: | --- |
| `+0x00` | 4 | magic `0xa89f000c` |
| `+0x08` | 4 | expanded size |
| `+0x10` | 4 | control-stream offset |
| `+0x14` | 4 | flags; known high bits are `0x80000000` |
| `+0x18` | variable | literal words |

Control bytes select:

- A literal 16-bit word.
- A word from the associated `CODE` dictionary.
- A back-reference into already expanded output.

The dictionary base is derived from the control code and associated `CODE`
resource.

## Member compression

Processing order:

1. Map every packed byte through the VISE substitution permutation.
2. Interpret the result as VISE's 16-bit-word representation of DEFLATE.
3. Normalize mixed/compressed streams to standard raw DEFLATE.
4. Delegate decompression to zlib.

### Compressed blocks

- Fixed and dynamic Huffman blocks use standard DEFLATE symbols.
- Bits are stored in big-endian 16-bit words rather than ordinary DEFLATE
  bytes.
- `unvise` parses the block structure and rewrites the bit representation as
  standard raw DEFLATE.

### Stored blocks

- Blocks align to a 16-bit boundary rather than an 8-bit boundary.
- The header contains `LEN` and one's-complement `NLEN`.
- Payload bytes are ordered as big-endian 16-bit words.
- An odd payload length consumes one padding byte.
- Stored-only and mixed streams follow the same normalization path. This is
  required because VISE 8.5 streams may begin with stored blocks and later
  switch to Huffman-compressed blocks.
- `unvise` parses Huffman symbols only to locate block boundaries, rewrites the
  representation, and leaves actual decompression to zlib.

## Incomplete semantics

- Password-protected members are unsupported.
- Password handling uses a conventional ZipCrypto path, but is not
  implemented.
- Carbon catalog layout follows the low SVCT revision byte: VISE 7.3 uses the
  ordinary layout at revision 11, the late layout begins at revision 12 in
  VISE 7.4, and VISE 8.5 uses revision 14.
- Lite custom-folder-icon destination objects are resolved against the
  immediately preceding directory. Other unresolved Lite destinations are
  rejected.
- Revision 12 uses `0xa0`-byte `DVCT` bodies; revision 14 uses `0xa4`-byte
  bodies. Revision 13 is accepted when no `DVCT` occurs; its directory body
  has not been observed.
- The complete semantics of every `FVCT` subtype and flag are not known.
  File/action classification and variable action tails follow the format's
  revision-specific structure and require the next record signature at the
  computed boundary. Payload fork boundaries are checked by each file's
  CRC-32.
- Installer actions, conditions, and package selection are catalog semantics,
  not file payloads; `unvise` extracts all distinct file alternatives rather
  than executing that installation policy.
- Self-hosting VISE 5.5 through 6.5 installers contain two records named
  `Installer VISE Archive Layouts` with logical resource sizes but no packed
  bytes. Their nominal payload offsets do not identify stored members and the
  field at `+0x54` is not a fork CRC. They are installer-internal references,
  not extractable byte streams, and are listed but not emitted.
- Base-dependent update members require a previous installed fork as decoder
  history. Their paired self-contained files are extracted; the update members
  are recognized, listed, and not emitted.
- Active Install catalogs can refer to an external archive. Its container and
  retrieval protocol are unsupported.
- Multi-segment catalogs are recognized and their locally stored members are
  extracted. Members assigned to absent media segments are listed but not
  emitted.
