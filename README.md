# unvise

`unvise` extracts classic Macintosh InstallerVISE archives on modern macOS and
other Linux/BSD/POSIX systems. It reads and writes native macOS forks where supported, AppleDouble
sidecars, and raw `.data`/`.rsrc` fork pairs. I've verified it against 30
freeware, shareware, and demo installer archives; should be relatively comprehensive, but no promises, patches encouraged!

| InstallerVISE | Supported format features |
| --- | --- |
| Lite 3.6 | short catalogs, direct substitution tables |
| 4.2, 4.5 | compact catalogs, packed initialization, short action records and parameter fields |
| 5.0.1, 5.5, 5.5.1, 5.5.2 | packed initialization code, shared payloads, file and action records |
| 6.0, 6.0.1 | self-hosting dictionaries, recognition of Active Install stubs |
| 6.5 | compressed catalogs |
| 7.0 | alternate initialization resources, version-source payloads |
| 8.0.2 | later compressed catalogs, PEF-embedded substitution table, framed payloads |

Across these versions, `unvise` supports data and resource forks, catalog
hierarchy, and mixed VISE/DEFLATE members.

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

Standard hidden AppleDouble
sidecars are also supported:

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

Names are converted from MacRoman to UTF-8 by default. Linux VFS treats a
filename as opaque bytes except for NUL and `/`, although a mounted filesystem
may impose additional rules. On such a filesystem, `-r` writes the
original MacRoman bytes; UTF-8 tools may display them incorrectly.

Modern macOS APFS and HFS+ path handling requires valid Unicode filenames
represented as UTF-8. A raw MacRoman byte such as `0xC4` is not valid UTF-8 and
is rejected as a pathname. On macOS, `-r -l` remains useful for
inspection: non-ASCII bytes are printed as ASCII escapes such as `\xC4`, not
written raw to the terminal. Raw-name extraction is unavailable there.

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

AppleDouble is a metadata container. Its resource-fork entry contains the raw
fork plus a header and entry table; `._name` is the standard loose-file naming
convention. A raw `.rsrc` from `macunpack -f` contains only the fork bytes.
For a raw pair, `unvise` accepts `Installer`, `Installer.data`, or
`Installer.rsrc` and resolves both files automatically.

`unar` handles all three formats. For a direct MacBinary or StuffIt download:

```sh
unar -k hidden package.bin        # also accepts .sit
./unvise -x output Installer
```

The four HQX files in the corpus contain StuffIt archives. Extract these in
two stages; `unar` 1.10.7 on macOS loses the final hidden sidecar when it
recurses through both layers in one invocation:

```sh
unar -nr -k hidden package.hqx    # produces the enclosed .sit file
unar -k hidden package.sit        # produces Installer and ._Installer
./unvise -x output Installer
```

The traditional `macutils` tools provide a tested path for direct MacBinary
installers:

```sh
macunpack -f Installer.bin        # produces Installer.data and Installer.rsrc
./unvise -x output Installer
```

`hexbin` can also remove a BinHex layer, but none of the four HQX files in the
test corpus contains an InstallerVISE application directly. Each decodes to a
MacBinary-wrapped StuffIt archive whose compression is too new for
`macunpack`; use `unar` for those files. `macunpack` can also read early
StuffIt archives, but the 16 StuffIt files in the corpus require `unar`.

## Limitations

Password-protected archives are unsupported. The corpus contains one VISE 6
Active Install stub in two wrappers; `unvise` recognizes it, but does not
locate or decode its external payload archive. Native MSVC builds are
unsupported.

See [FORMAT.md](FORMAT.md) for the reverse-engineered format details.

## License

MIT. Third-party installers and extracted files remain subject to their
original licenses.
