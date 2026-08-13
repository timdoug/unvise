# InstallerVISE format notes

This documents the subset implemented by `unvise`. Unless noted otherwise:

- Integers are unsigned and big-endian.
- Offsets prefixed with `+` are relative to the start of a record.
- Catalog names use MacRoman.
- Record boundaries are computed from the original loaders' fixed structures
  and declared variable-field lengths.

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

For a raw pair, the command-line input may name the stem, `.data` file, or
`.rsrc` file; all resolve to the `.data` fork and its matching `.rsrc` fork.

MacBinary, BinHex, and StuffIt are outer formats and must be removed first.

Output representations are raw `.data`/`.rsrc` fork pairs by default, hidden
AppleDouble sidecars with `-a`, or native macOS forks with `-n`. Native output
preserves Finder information and creation/modification times for files and
directories. AppleDouble preserves Finder information and resource forks;
AppleDouble and raw output apply the catalog modification time to each
ordinary filesystem object. Only native output preserves creation time.

## SVCT data fork

| Offset | Size | Meaning |
| ---: | ---: | --- |
| `0x00` | 4 | `SVCT` signature |
| `0x04` | 4 | archive version (`1` in the corpus) |
| `0x10` | 4 | format flags and revision; low byte is the catalog revision |
| `0x24` | 4 | catalog offset |

The catalog starts with `CVCT` and contains `CVCT`, `DVCT`, `FVCT`, and `PACK`
records.

`PACK` is an ordinary catalog record and can occur in an uncompressed catalog.
Catalog compression is indicated by the nonzero field at `CVCT+0x08`, not by
the presence of a `PACK` record.

`CVCT+0x10` contains the number of `DVCT` and `FVCT` records. Version-specific
fixed bodies come directly from the recovered loaders. Variable tails are
calculated from declared lengths and the record subtype, then the expected next
signature must occur at the computed boundary. A signature is never searched
for, so embedded tag-like bytes cannot create a record.

## CVCT catalog header

| Offset | Size | Meaning |
| ---: | ---: | --- |
| `+0x00` | 4 | `CVCT` signature |
| `+0x04` | 4 | compressed stream length when `+0x08` is nonzero |
| `+0x08` | 4 | nonzero when the catalog is compressed |
| `+0x10` | 2 | number of `DVCT` and `FVCT` records |
| `+0x64` | variable | word-swapped raw-DEFLATE stream when compressed |

A compressed catalog expands to ordinary `DVCT`, `FVCT`, and `PACK` records.
This form is present in the VISE 6.5, 7, and 8 corpus.

Layout selection uses the low revision byte and the `CVCT+0x08` compression
field; it does not inspect catalog contents for a likely structure:

| Layout | Revision and storage | InstallerVISE versions in the corpus |
| --- | --- | --- |
| Lite | revision 0, uncompressed | Lite 3.6 |
| Compact | revisions 2, 3, or 4, uncompressed | 4.2, 4.5, 4.6.1 |
| Normal | revisions 5 through 11, uncompressed | 5.0.1, 5.5, 5.5.1, 5.5.2, 6.0, 6.0.1 |
| Compressed | revisions 5 through 11, word-swapped raw DEFLATE | 6.5, 7.0, 7.3 |
| VISE 8 | revisions 12 or 14, compressed with later bodies | 8.0.2, 8.5 |

Revision 1 and revision 13 have not been recovered or observed and are
rejected. In particular, the verified VISE 5.0.1 installer has a normal
uncompressed catalog even though its catalog contains a `PACK` record.

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

For a compressed catalog, the name moves to `+0x98`; observed VISE 8 records
use `+0xa0` or `+0xa4`.

VISE 8 stores records in preorder and gives each directory an explicit nesting
depth in the high 16 bits at `+0x48`. Directory IDs remain at `+0x1c`, but the
preorder depth is sufficient to reconstruct the tree and is used by the
original PPC loaders.

The VISE 8.0.2 directory body is `0xa0` bytes and the VISE 8.5 body is `0xa4`
bytes. The primary and optional secondary trailing-name lengths are bytes at
`+0x50` and `+0x4f` respectively.

The VISE 4.5 68K loader copies raw directory `+0x04..+0x13` into the internal
Finder-information field and raw `+0x14/+0x18` into its creation/modification
fields. The VISE 8.5 PPC loader independently performs the same 16-byte
metadata copy. Directory metadata is applied after all children, in reverse
catalog order, so creating a child cannot replace the restored parent mtime.

### Compact catalog

- VISE 4.2, 4.5, and 4.6.1 installers use this layout.
- The original 68K loaders select fixed body sizes from the low revision byte
  at `SVCT+0x13`.
- Revisions 2 and 3 use `0x58`-byte directory bodies; revision 4 uses `0x68`.
- Directory and parent IDs remain at `+0x1c` and `+0x20`.
- The primary name length remains at `+0x50`.
- The MacRoman name follows the revision-specific fixed body.

### Lite 3.6 short catalog

- The original 68K loader reads a `0x58`-byte directory body and declared
  trailing Pascal fields; boundaries are mechanical.
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
| `+0x54` | 4 | CRC-32 of expanded data fork followed by resource fork |
| `+0x58` | 4 | parent directory ID or install-location token |
| `+0x64` | 4 | payload offset in the data fork |
| `+0x68` | 4 | data-fork offset in a shared or combined expanded member |
| `+0x6c` | 4 | resource-fork offset in that member |
| `+0x7a` | 1 | primary trailing-name length |
| `+0xba` | variable | MacRoman name |

For a compressed catalog, the name moves to `+0xbe`; VISE 8 moves it eight
bytes farther to `+0xc6`. The fork and payload fields do not move.

In VISE 8, the high 16 bits at `+0x60` are the preorder nesting depth. VISE
8.5 repurposes `+0x58`; it must not be interpreted as a parent ID. The original
PPC loaders reconstruct hierarchy from the explicit depth fields.

The primary trailing-name length is consistently the byte at `+0x7a`. The
VISE 8 fixed file/action body is `0xc6` bytes and can carry an optional
secondary-name length at `+0xb9`. In both recovered VISE 8.5 PPC loaders,
`BitTst(record + 0x0c, 4)` identifies an action and controls whether the byte
at `+0x7b` and subtype-specific action fields follow the primary name. Classic
bit numbering makes this mask `0x08000000` when the four bytes are read as a
big-endian integer. The secondary name is read afterward. `unvise` follows
that control flow directly and emits only records for which the action bit is
clear.

The VISE 4.5 68K loader independently copies raw `+0x0c` to internal `+0x8e`
and calls `BitTst(internal + 0x8e, 4)` before selecting its action-tail path.
The Lite 3.6 68K loader uses the same test and reads `0x78` bytes after the
`FVCT` tag (`0x7c` including it), confirming both the marker and short fixed
body without relying on corpus alignment.

The same PPC loader copies raw `+0x3c` and `+0x40` to internal record fields
`+0x42` and `+0x46`. File-creation paths pass those fields, in that order, to
the classic catalog-info routine that sets creation and modification dates.
The recovered VISE 4.5 68K loader performs the same two copies, independently
confirming the layout. Native output converts the 1904 Mac epoch to the Unix
epoch.

### Compact catalog

- Fork sizes and the payload offset retain their common offsets.
- The parent directory ID is at `+0x60`.
- Revisions 2 and 3 use `0x7c`-byte file/action bodies; revision 4 uses `0x8c`.
- The MacRoman name follows the revision-specific fixed body.
- Some localized self-installer records contain no usable trailing filename.
  Their shared payload members are still distinct; output paths use the stable
  catalog record number so no variant overwrites another.

### Lite 3.6 short catalog

- The original 68K loader reads a `0x7c`-byte file/action body and declared
  trailing Pascal fields; boundaries are mechanical.
- Fork sizes and payload offset retain their common offsets.
- The fixed body ends at `+0x7c`; the primary name length is at `+0x7a`.
- `+0x58` is the destination reference. It usually names a `DVCT` ID.
- The original 68K loader copies this field into its runtime destination
  object; it is not unconditionally a catalog directory ID.
- A custom folder icon (`Icon\r`, Finder type `icon`, creator `MACS`) carries
  that runtime object rather than a `DVCT` ID. The operation applies to the
  immediately preceding directory in catalog preorder. `unvise` recognizes
  this case explicitly and rejects other unresolved destination objects.

### Action records

`FVCT` also represents installer operations rather than files. Observed
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
- VISE 8 consistently uses the first form, including data-only members.

### Conditional path collisions

- Multiple file records may intentionally target the same logical pathname.
  They represent alternatives selected by installer conditions, packages,
  architecture, localization, or version checks.
- Alternatives are not assumed identical. Divergent fork contents occur in
  every affected catalog layout in the corpus.
- Records whose complete set of forks is byte-identical to an earlier record
  at the same logical pathname are omitted. The lowest-numbered distinct
  record retains the logical pathname. Later distinct records receive a
  `~NNNN` suffix, where `NNNN` is the stable catalog record number. Data and
  resource forks from one record retain the same suffix.
### VISE 8 framed payloads

- Shared payload field 0 holds the compressed member size.
- Shared payload field 1 holds the complete expanded size.
- The common fork-offset fields are authoritative; catalog order is not used
  to divide VISE 8 members.
- Bytes not covered by a catalog record are not output. One MacPython member
  contains an unreferenced duplicate of a file represented elsewhere in the
  catalog.
- A single data-only record can use the same framing and offset fields.

### File checksums

- `FVCT+0x54` is the CRC-32 of the expanded data fork followed by the expanded
  resource fork.
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
- The verified RAM Charger archive places it at `DATA 0+0x4ac`.
- `unvise` searches for a unique permutation instead of relying on that offset.
- No `CODE` 24 resource is required.

### VISE 4.2 through 7.0

- `DATA` resource 0 is normally an `A89F000C` packed-code stream.
- The verified 4.5 and 4.6.1 self-installers instead store the permutation
  literally; this does not make their catalogs Lite records.
- A VISE 8.0.2 68K installer stores the three initialization streams directly
  in `DATA 0`, without either the packed-code wrapper or a literal table.
- VISE 4.5 uses `CODE` 18 as its word dictionary and places the permutation
  directly in the expanded resource.
- `CODE` 24 normally supplies its word dictionary.
- VISE 7 uses `CODE` 23 instead.
- Some VISE 5 and 6 self-installers pack `CODE` 24 and put the required
  dictionary in uncompressed `CODE` 25 or `CODE` 1002.
- Expanding `DATA` 0 produces three initialization streams.
- The streams initialize a 256-byte substitution permutation in an emulated
  signed 16-bit A5-relative address space.
- `DATA 0` does not identify the table by offset or symbol. The original 68K
  decompressor references it as a compiled A5-relative global, and that address
  changes between builds.

Observed permutation addresses:

| InstallerVISE | A5-relative address |
| --- | ---: |
| 5.0.1 | `-0x14b0` |
| 5.5.2 | `-0x151c` |
| 6.5 | `-0x15ca` |

`unvise` identifies the sole initialized 256-byte permutation. Reproducing the
original lookup more literally would require recognizing a build-specific
machine-code reference and would not decode additional archive metadata.

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

- `DATA` 0 and its initialization program are absent in the verified Carbon
  installer.
- The permutation is recovered from the initialized data of the input PEF
  application, as in VISE 8.
- Catalog records retain the ordinary compressed-layout name offsets
  (`DVCT+0x98` and `FVCT+0xbe`).
- The two independently recovered PPC catalog loaders read fixed `0x94`-byte
  `DVCT` and `0xba`-byte `FVCT` bodies. The archive's low SVCT revision byte is
  11; VISE 8 starts at revision 12.

### VISE 8.0.2

- `DATA` 0 and its initialization program are absent.
- The main PPC PEF application begins at data-fork offset `0x30`.
- Expanding its packed-data section exposes the complete 256-byte permutation
  as a static byte array. The analyzed MacPython 2.3.3 installer loads it at
  `0x1006da7c`; a relocated global pointer at `0x1006bc90` refers to it.
- As in the 68K implementation, the table address belongs to the compiled
  application rather than to an InstallerVISE archive structure.
- `Dcmp` 1005, labeled `Version 27 (PPC, decomp)`, obtains already-transformed
  16-bit words through the host application's read callback. It implements the
  DEFLATE-family decoder but does not contain or generate the permutation.
- The embedded bytes exactly match the table independently reconstructed from
  every earlier corpus version.
- `unvise` parses the input PEF section headers, expands Apple's standard PEF
  packed-data opcodes, and locates the sole 256-byte permutation in the
  expanded section. Following the original relocated pointer would require a
  PEF relocation and machine-code analysis pass. No substitution table is
  built into `unvise`.
- A candidate is accepted only when all 256 byte values occur exactly once.
  The catalog CRC-32 of every extracted file then independently validates the
  selected table; all extractable corpus installers pass this check.

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
| `+0x14` | 4 | flags; observed high bits are `0x80000000` |
| `+0x18` | variable | literal words |

Control bytes select:

- A literal 16-bit word.
- A word from the associated `CODE` dictionary.
- A back-reference into already expanded output.

The dictionary-base expression was recovered from the 68K unpacker and
checked against the PowerPC implementation.

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

## Independent evidence

- The 68K and PowerPC implementations both contain a standard DEFLATE block
  dispatcher, literal/length and distance trees, and a 32 KiB window.
- PowerPC parameter layout and 68K call sites agree on compressed size,
  produced size, input/output callbacks, and progress callbacks.
- The corpus includes installers using both decompressor implementations.

## Unsupported or inferred behavior

- Password-protected members are unsupported.
- The installer contains a conventional ZipCrypto path, but the Escape
  Velocity installer has no `PsWd` resource and does not enable it.
- Carbon catalog layout follows the low SVCT revision byte: the original VISE
  7.3 PPC loader uses the ordinary layout at revision 11, while the VISE 8
  layout begins at revision 12. VISE 8.5 uses revision 14.
- Lite custom-folder-icon destination objects are resolved against the
  immediately preceding directory. Other unresolved Lite destinations are
  rejected.
- Both recovered VISE 8.0.2 PPC loaders read `0xa0`-byte `DVCT` bodies at
  revision 12. Both recovered VISE 8.5 loaders read `0xa4`-byte bodies at
  revision 14. Revision 13 has not been observed and is rejected rather than
  assigned an inferred body size.
- The fixed catalog structures and revision dispatch are loader-derived. The
  complete semantics of every `FVCT` subtype and flag have not yet been named;
  file/action classification and variable action tails reproduce the recovered
  loader control flow and are checked against the following record signature.
  Payload fork boundaries receive the stronger, independent check of each file
  record's CRC-32.
- Catalog names are converted with the complete MacRoman character mapping by
  default. `-r` preserves their original bytes instead on
  byte-oriented filesystems; modern macOS permits raw listing but not raw-name
  extraction.
- Native output preserves file and directory creation times, modification
  times, and 16-byte Finder information. AppleDouble preserves Finder
  information and resource forks. AppleDouble and raw output preserve
  modification times using POSIX filesystem timestamps. AppleDouble entry ID
  8 can represent dates historically, but current macOS `copyfile`/`dot_clean`
  rejects sidecars containing it; interoperable output therefore omits it.
  Directory permissions and installer-defined post-install actions are not
  applied.
- Installer actions, conditions, and package selection are catalog semantics,
  not file payloads; `unvise` extracts all distinct file alternatives rather
  than executing that installation policy.
- Self-hosting VISE 5.5 through 6.5 installers contain two records named
  `Installer VISE Archive Layouts` with logical resource sizes but no packed
  bytes. Their nominal payload offsets do not identify stored members and the
  field at `+0x54` is not a fork CRC. They are installer-internal references,
  not extractable byte streams, and are listed but not emitted.
- The corpus contains one VISE 6 Active Install stub in both StuffIt and
  MacBinary wrappers. Its catalog refers outside the stub; the external archive
  format and retrieval protocol have not been reverse-engineered.

## Verified versions

| InstallerVISE | Samples |
| --- | --- |
| Lite 3.6 | RAM Charger 8.1; IconDropper 3.0; BarbaBatch 3.1 Demo; UTC Display |
| 4.2 | Internet Explorer 3.0a Java; OMS 2.3.2 Web |
| 4.5 | PGP 5.0 Freeware; Installer VISE 4.5 |
| 4.6.1 | Installer VISE 4.6.1 |
| 5.0.1 | Mac F2C 1.4.1 |
| 5.5 | Startup Lock 2.5; InstallerVISE 5.5 Demo |
| 5.5.1 | BBEdit 5.0 Demo |
| 5.5.2 | Escape Velocity 1.0.5; MacPipes 2.2.7; IntelliNews 1.1.1; FreeMIDI 1.43 |
| 6.0 | InstallerVISE 6.0 Demo and Active Install demo |
| 6.0.1 | InstallerVISE 6.0.1 |
| 6.5 | Visual Projector 2.0; InstallerVISE 6.5 Demo; iControl 1.2 |
| 7.0 | Interarchy 4.0; Interarchy 3.8 |
| 7.0.1 | Tcl/Tk 8.3.2p1 Runtime and Web installers |
| 7.2 | Tcl/Tk 8.3.3 Runtime installer |
| 7.3 | HP LaserJet 4/5/6 legacy driver |
| 7.4 | Tcl/Tk 8.3.4 Full installer |
| 8.0.2 | MacPython 2.2.3; MacPython 2.3.3; Tcl/Tk 8.3.5 Runtime/Full and 8.4.1 Web installers |
| 8.5 | Installer VISE 8.5 |
