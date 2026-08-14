# unvise

`unvise` extracts classic Macintosh InstallerVISE archives on modern macOS and
Linux, BSD, and other POSIX systems. It reads and writes native macOS forks,
AppleDouble sidecars, and raw `.data`/`.rsrc` fork pairs.

Expanded files are verified against the CRC-32 stored in their catalog
records. Directory hierarchy, Finder information, and timestamps are preserved
where the selected output format supports them.

Compatibility has been verified with freeware, shareware, and demo installers:

| InstallerVISE | Supported format features |
| --- | --- |
| Lite 3.6 | short catalogs, direct substitution tables, shared payload groups |
| 4.2, 4.5, 4.6.1 | revision-sized compact records, literal or packed initialization, short actions |
| 5.0.1, 5.5, 5.5.1, 5.5.2 | packed and relocated initialization code, shared payloads, extended action records |
| 6.0, 6.0.1 | self-hosting dictionaries, recognition of Active Install stubs |
| 6.5 | compressed catalogs, shared payloads, unpaired base-dependent updates |
| 7.0, 7.0.1, 7.2 | alternate initialization resources, offset-fork and gapped shared payloads |
| 7.3 | PEF-embedded substitution table, word-oriented members, base-dependent updates |
| 7.4, 8.0.2, 8.5 | later compressed catalogs, framed and overlapping update payloads, mixed stored/compressed blocks |

Across these versions, `unvise` supports data and resource forks, catalog
hierarchy, and mixed VISE/DEFLATE members.

The compatibility corpus contains 91 downloaded fixtures representing 81
distinct applications. It also exercises early SVCT generations 0 and 2 and
revision 13, whose exact InstallerVISE marketing versions are not embedded in
the samples.

## Usage

Build with a C99 compiler and zlib:

```sh
make
```

After extracting any outer archive, list the installer catalog:

```console
$ installer="Escape Velocity Installer"
$ ./unvise -l "$installer"
SVCT version=1 size=5145057 catalog=0x4E6960
0000 0x004E6960 CVCT size=0x14
0001 0x004E6974 PACK size=0x50
0002 0x004E69C4 DVCT size=0xAB name="Escape Velocity 1.0.5 ƒ"
0003 0x004E6A6F FVCT size=0xBF payload=0x2C data=0x0->0x0 rsrc=0x3F6->0xA6E name="Icon\x0D"
...
```

Extract it. By default, the extracted forks are raw files named `name.data` or
`name.rsrc`, matching `macunpack`'s conventions:

```console
$ ./unvise -x raw-output "$installer"
SVCT version=1 size=5145057 catalog=0x4E6960
$ find raw-output -type f | sort | head -n4
raw-output/Escape Velocity 1.0.5 ƒ/ • READ ME • .data
raw-output/Escape Velocity 1.0.5 ƒ/ • READ ME • .rsrc
raw-output/Escape Velocity 1.0.5 ƒ/Documentation ƒ/Ambrosia FAQ.text.data
raw-output/Escape Velocity 1.0.5 ƒ/Documentation ƒ/Ambrosia FAQ.text.rsrc
```

Standard hidden AppleDouble sidecars are also supported:

```console
$ ./unvise -a -x appledouble-output "$installer"
SVCT version=1 size=5145057 catalog=0x4E6960
$ find appledouble-output -type f | sort | head -4
appledouble-output/Escape Velocity 1.0.5 ƒ/ • READ ME •
appledouble-output/Escape Velocity 1.0.5 ƒ/._ • READ ME •
appledouble-output/Escape Velocity 1.0.5 ƒ/._EV Data
appledouble-output/Escape Velocity 1.0.5 ƒ/._EV Graphics
```

On macOS, both forks can be preserved in one native file:

```console
$ ./unvise -n -x native-output "$installer"
SVCT version=1 size=5145057 catalog=0x4E6960
$ file=$(find native-output -type f -size +500k -print -quit)
$ printf '%s\n' "$file"
native-output/Escape Velocity 1.0.5 ƒ/Escape Velocity
$ stat -f 'data fork: %z bytes' "$file"
data fork: 514331 bytes
$ stat -f 'resource fork: %z bytes' "$file/..namedfork/rsrc"
resource fork: 500871 bytes
```

Names are converted from MacRoman to UTF-8 by default. On byte-oriented
filesystems, `-r` instead preserves the original filename bytes. Raw-name
listing escapes non-ASCII bytes; raw-name extraction is unavailable on macOS,
whose filesystem APIs require valid UTF-8 paths.

## Preparing inputs

MacBinary, BinHex, and StuffIt are common outer containers. Unpack them while
keeping both Macintosh forks, then pass the InstallerVISE data fork to
`unvise`.

Accepted installer inputs:

| Data fork | Resource fork | Representation |
| --- | --- | --- |
| `Installer` | native fork on the same file | macOS |
| `Installer` | `._Installer` | AppleDouble |
| `Installer.data` | raw `Installer.rsrc` | `macunpack -f` |

For a raw pair, `unvise` accepts `Installer`, `Installer.data`, or
`Installer.rsrc` and resolves both files automatically.

`unar` handles all three formats. For a direct MacBinary or StuffIt download:

```sh
unar -k hidden package.bin        # also accepts .sit
./unvise -x output Installer
```

BinHex downloads commonly contain a StuffIt archive. Extract each layer while
retaining hidden AppleDouble files:

```sh
unar -nr -k hidden package.hqx    # produces the enclosed .sit file
unar -k hidden package.sit        # produces Installer and ._Installer
./unvise -x output Installer
```

Keep a separate `Installer.data` payload archive beside `Installer`; it is
detected automatically. This is distinct from a macutils `.data`/`.rsrc` fork
pair, for which `Installer` itself does not exist.

The traditional `macutils` tools provide a tested path for direct MacBinary
installers:

```sh
macunpack -f Installer.bin        # produces Installer.data and Installer.rsrc
./unvise -x output Installer
```

`hexbin` can also remove a BinHex layer. If its output is a StuffIt archive,
extract that separately before running `unvise`.

## Limitations

Password-protected archives are unsupported. Active Install web stubs are
recognized, but their external payload archives are not downloaded or decoded.
Members assigned to absent media segments are listed but not emitted. Installer
actions and directory permissions are not applied. Native MSVC builds are
unsupported.

See [FORMAT.md](FORMAT.md) for format details and
[test-data/README.md](test-data/README.md) for the compatibility corpus.

## License

MIT. Third-party installers and extracted files remain subject to their
original licenses.
