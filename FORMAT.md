# InstallerVISE format notes

This documents the subset implemented by `unvise`. Unless noted otherwise:

- Integers are unsigned and big-endian.
- Offsets prefixed with `+` are relative to the start of a record.
- Catalog names use MacRoman.
- Record boundaries and several variant rules are inferred from the test corpus.

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
AppleDouble sidecars with `-a`, or native macOS forks with `-n`.

## SVCT data fork

| Offset | Size | Meaning |
| ---: | ---: | --- |
| `0x00` | 4 | `SVCT` signature |
| `0x04` | 4 | format version observed by the installer |
| `0x24` | 4 | catalog offset |

The catalog starts with `CVCT` and contains `CVCT`, `DVCT`, `FVCT`, and `PACK`
records.

`PACK` is an ordinary catalog record and can occur in an uncompressed catalog.
Catalog compression is indicated by the nonzero field at `CVCT+0x08`, not by
the presence of a `PACK` record.

No authoritative record-length field has been identified. `unvise` finds
record boundaries by scanning for those four tags. Tag-like bytes in an
unknown catalog variant could therefore be misidentified.

## CVCT catalog header

| Offset | Size | Meaning |
| ---: | ---: | --- |
| `+0x00` | 4 | `CVCT` signature |
| `+0x04` | 4 | compressed stream length when `+0x08` is nonzero |
| `+0x08` | 4 | nonzero when the catalog is compressed |
| `+0x64` | variable | word-swapped raw-DEFLATE stream when compressed |

A compressed catalog expands to ordinary `DVCT`, `FVCT`, and `PACK` records.
This form is present in the VISE 6.5, 7, and 8 corpus.

Observed layouts:

| Layout | Catalog storage | InstallerVISE versions in the corpus |
| --- | --- | --- |
| Lite | uncompressed, names at record ends | Lite 3.6 |
| Compact | uncompressed, shorter records | 4.2, 4.5, 4.6.1 |
| Normal | uncompressed | 5.0.1, 5.5, 5.5.1, 5.5.2, 6.0, 6.0.1 |
| Compressed | word-swapped raw DEFLATE | 6.5, 7.0, 7.3 |
| VISE 8 | compressed, with later name offsets | 8.0.2 |

These are observed record/storage layouts, not version numbers. In particular,
the verified VISE 5.0.1 installer has a normal uncompressed catalog even though
its catalog contains a `PACK` record.

## DVCT directory record

### Normal catalog

| Offset | Size | Meaning |
| ---: | ---: | --- |
| `+0x00` | 4 | `DVCT` signature |
| `+0x1c` | 4 | directory ID |
| `+0x20` | 4 | parent directory ID |
| `+0x94` | variable | MacRoman name |

For a compressed catalog, the name moves to `+0x98`; VISE 8 moves it eight
bytes farther to `+0xa0`. The ID fields do not move.

### Compact catalog

- VISE 4.2, 4.5, and 4.6.1 installers use this layout.
- Their directory records are only `0x5b` to `0x68` bytes long, ruling out the
  normal layout's name at `+0x94`.
- Directory and parent IDs remain at `+0x1c` and `+0x20`.
- The MacRoman name begins at `+0x58`, or `+0x68` when the record contains the
  observed 16-byte extension.

### Lite 3.6 short catalog

- Directory and parent IDs remain at `+0x1c` and `+0x20`.
- The name is stored at the end of the record.
- Its representation is one length byte, seven reserved bytes, then the name.

## FVCT file or action record

### Common fields

| Offset | Size | Meaning |
| ---: | ---: | --- |
| `+0x00` | 4 | `FVCT` signature |
| `+0x04` | 4 | type or flags |
| `+0x44` | 4 | packed data-fork size |
| `+0x48` | 4 | expanded data-fork size |
| `+0x4c` | 4 | packed resource-fork size |
| `+0x50` | 4 | expanded resource-fork size |
| `+0x58` | 4 | parent directory ID or install-location token |
| `+0x64` | 4 | payload offset in the data fork |
| `+0x68` | 4 | VISE 7 version-source gap size |
| `+0xba` | variable | MacRoman name |

For a compressed catalog, the name moves to `+0xbe`; VISE 8 moves it eight
bytes farther to `+0xc6`. The fork and payload fields do not move.

### Compact catalog

- Fork sizes and the payload offset retain their common offsets.
- The parent directory ID is at `+0x60`.
- The MacRoman name begins at `+0x7c`, or `+0x8c` with the 16-byte extension.
- Some localized self-installer records contain no usable trailing filename.
  Their shared payload members are still distinct; output paths use the stable
  catalog record number so no variant overwrites another.

### Lite 3.6 short catalog

- Fork sizes and payload offset retain their common offsets.
- The name is stored at the end as one length byte, `0x2c`, then the name.
- Most records store their parent ID at `+0x58`.
- Some records store an install-location token at `+0x58`; `unvise` then
  infers the containing directory from catalog order.

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
- Expanded bytes are assigned in catalog order.
- Each record's data fork precedes its resource fork.
- Expanded fork sizes determine how the member is divided.
- For combined-fork groups, packed field 0 holds the compressed size and
  packed field 1 may hold the total expanded size.
- For resource-only groups, packed field 1 holds the compressed size.

### VISE 8 framed payloads

- Shared payload field 0 holds the compressed member size.
- Shared payload field 1 holds the complete expanded size.
- Declared forks occupy the beginning in catalog order; installer bookkeeping
  follows them and is not part of any output fork.
- A single data-only record can use the same framing: its data fork begins the
  expanded member and the bookkeeping follows it.

### VISE 7 version-source payload

One member may contain:

```text
data fork | version-source material | resource fork
```

| Field | Meaning |
| --- | --- |
| `+0x48` | expanded data-fork size |
| `+0x50` | expanded resource-fork size |
| `+0x68` | intervening version-source size |
| `+0x4c` | complete expanded member size in this layout |

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
- VISE 4.5 uses `CODE` 18 as its word dictionary and places the permutation
  directly in the expanded resource.
- `CODE` 24 normally supplies its word dictionary.
- VISE 7 uses `CODE` 23 instead.
- Some VISE 5 and 6 self-installers pack `CODE` 24 and put the required
  dictionary in uncompressed `CODE` 25 or `CODE` 1002.
- Expanding `DATA` 0 produces three initialization streams.
- The streams initialize a 256-byte substitution permutation in an emulated
  signed 16-bit A5-relative address space.

Observed permutation addresses:

| InstallerVISE | A5-relative address |
| --- | ---: |
| 5.0.1 | `-0x14b0` |
| 5.5.2 | `-0x151c` |
| 6.5 | `-0x15ca` |

`unvise` identifies the unique initialized 256-byte permutation instead of
selecting an address by version.

### VISE 7.3

- `DATA` 0 and its initialization program are absent in the verified Carbon
  installer.
- The permutation is recovered from the initialized data of the input PEF
  application, as in VISE 8.
- Catalog records retain the ordinary compressed-layout name offsets
  (`DVCT+0x98` and `FVCT+0xbe`).
- `unvise` distinguishes this layout from VISE 8 by checking those offsets for
  complete MacRoman names after inflating the catalog.

### VISE 8.0.2

- `DATA` 0 and its initialization program are absent.
- The main PPC PEF application begins at data-fork offset `0x30`.
- Expanding its packed-data section exposes the complete 256-byte permutation
  as a static byte array. The analyzed MacPython 2.3.3 installer loads it at
  `0x1006da7c`; a relocated global pointer at `0x1006bc90` refers to it.
- `Dcmp` 1005, labeled `Version 27 (PPC, decomp)`, obtains already-transformed
  16-bit words through the host application's read callback. It implements the
  DEFLATE-family decoder but does not contain or generate the permutation.
- The embedded bytes exactly match the table independently reconstructed from
  every earlier corpus version.
- `unvise` parses the input PEF section headers, expands Apple's standard PEF
  packed-data opcodes, and locates the unique 256-byte permutation in the
  expanded section. No substitution table is built into `unvise`.

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
- For compressed-only streams, swapping each 16-bit word produces standard
  raw DEFLATE.

### Stored blocks

- Blocks align to a 16-bit boundary rather than an 8-bit boundary.
- The header contains `LEN` and one's-complement `NLEN`.
- Payload bytes are ordered as big-endian 16-bit words.
- An odd payload length consumes one padding byte.
- Stored-only members are copied directly.
- For mixed streams, `unvise` parses Huffman symbols only to locate block
  boundaries, rewrites the representation, and leaves actual decompression to
  zlib.

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
- Catalog record boundaries are inferred from record tags.
- Catalog names are converted with the complete MacRoman character mapping by
  default. `-r` preserves their original bytes instead on
  byte-oriented filesystems; modern macOS permits raw listing but not raw-name
  extraction.
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
| 7.3 | HP LaserJet 4/5/6 legacy driver |
| 8.0.2 | MacPython 2.2.3; MacPython 2.3.3 |
