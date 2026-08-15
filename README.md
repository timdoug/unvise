# unvise

`unvise` extracts classic Macintosh Installer VISE archives on modern macOS,
Linux, BSD, and other POSIX systems. It handles data and resource forks,
MacBinary, BinHex, AppleDouble, and raw fork pairs.

## Compatibility

I've tested against hundreds of freeware, shareware, and demo installer fixtures:

| Installer VISE | Supported format features |
| --- | --- |
| 3.0 | early compact catalogs |
| 3.5 | early short catalogs, direct substitution tables, self-parent roots |
| Lite 3.6 | Lite destinations, direct substitution tables, shared payload groups |
| 4.0, 4.2, 4.5, 4.6.1 | revision-sized compact records, literal or packed initialization, short actions |
| 5.0.1, 5.5, 5.5.1, 5.5.2 | packed and relocated initialization code, shared payloads, extended action records |
| 6.0, 6.0.1 | self-hosting dictionaries, recognition of Active Install stubs |
| 6.5 | compressed catalogs, shared and framed payloads, unpaired base-dependent updates |
| 7.0, 7.0.1, 7.1, 7.1.1, 7.2 | alternate initialization resources, offset-fork and gapped shared payloads |
| 7.3 | PEF-embedded substitution table, word-oriented members, base-dependent updates |
| 7.4, 8.0.1, 8.0.2, 8.2, 8.2.1, 8.3, 8.4, 8.5 | late catalogs, framed and overlapping update payloads, mixed stored/compressed blocks |

See [test-data/README.md](test-data/README.md) for
more, and [FORMAT.md](FORMAT.md) for format details.

## Quick start

Build with a C99 compiler and zlib:

```sh
make
```

List or extract an installer:

```sh
./unvise -l Installer
./unvise -x output Installer
```

A listing looks like this:

```console
$ ./unvise -l EV_Installer_1.0.5.bin
SVCT version=1 size=5145057 catalog=0x4E6960
0000 0x004E6960 CVCT size=0x14
0001 0x004E6974 PACK size=0x50
0002 0x004E69C4 DVCT size=0xAB name="Escape Velocity 1.0.5 ƒ"
0003 0x004E6A6F FVCT size=0xBF payload=0x2C data=0x0->0x0 rsrc=0x3F6->0xA6E segment=1 name="Icon\x0D"
...
```

Expanded files are checked against the CRC-32 values in the catalog.
Directory hierarchy, Finder information, and timestamps are preserved where
the selected output format supports them.

## Output formats

By default, each fork is an ordinary file named `name.data` or `name.rsrc`:

```console
$ ./unvise -x output EV_Installer_1.0.5.bin
SVCT version=1 size=5145057 catalog=0x4E6960
$ find output -type f | sort | head -n4
output/Escape Velocity 1.0.5 ƒ/ • READ ME • .data
output/Escape Velocity 1.0.5 ƒ/ • READ ME • .rsrc
output/Escape Velocity 1.0.5 ƒ/Documentation ƒ/Ambrosia FAQ.text.data
output/Escape Velocity 1.0.5 ƒ/Documentation ƒ/Ambrosia FAQ.text.rsrc
```

Use `-a` for standard hidden AppleDouble sidecars:

```console
$ ./unvise -a -x output EV_Installer_1.0.5.bin
$ find output -type f | sort | head -n4
output/Escape Velocity 1.0.5 ƒ/ • READ ME •
output/Escape Velocity 1.0.5 ƒ/._ • READ ME •
output/Escape Velocity 1.0.5 ƒ/._EV Data
output/Escape Velocity 1.0.5 ƒ/._EV Graphics
```

On macOS, `-n` stores both forks in one native file:

```console
$ ./unvise -n -x output EV_Installer_1.0.5.bin
$ file=$(find output -type f -size +500k -print -quit)
$ stat -f 'data fork: %z bytes' "$file"
data fork: 514331 bytes
$ stat -f 'resource fork: %z bytes' "$file/..namedfork/rsrc"
resource fork: 500871 bytes
```

By default, MacRoman filenames are converted to UTF-8 for both listing and
extraction. With `-r`, no character conversion is performed: listings write the
original bytes to the terminal, and extraction uses them as pathname bytes.
(Control characters are still escaped in listings so as not to disrupt the
terminal.)

Bemusingly, `-r` can list but cannot extract on modern macOS; its filesystem APIs
require valid UTF-8 paths, and arbitrary MacRoman byte strings are not guaranteed
to be valid UTF-8. Raw-name extraction works on byte-oriented filesystems such as
those commonly used on Linux.

## Inputs

`unvise` accepts:

| Input | Representation |
| --- | --- |
| `Installer.bin` | MacBinary |
| `Installer.hqx` | BinHex |
| `Installer` with a native resource fork | macOS |
| `Installer` plus `._Installer` | AppleDouble |
| `Installer.data` plus `Installer.rsrc` | raw forks from `macunpack -f` |

MacBinary and BinHex layers are decoded recursively and may be nested in any
order.

For a raw pair, any of `Installer`, `Installer.data`, or `Installer.rsrc` may
be passed; the other fork is found automatically. A separate
`Installer.data` payload archive beside an installer is also detected.

StuffIt decompression requires an external tool such as `unar`. Unpack an
archive while preserving both forks, then pass the resulting application to
`unvise`:

```sh
unar -k hidden package.sit
./unvise -x output Installer
```

## Limitations

- Password-protected archives are unsupported.
- Active Install web stubs are recognized, but their external payload archives
  are not downloaded or decoded.
- Members assigned to absent media segments are listed but not emitted.
- Installer actions and directory permissions are not applied.
- Native MSVC builds are unsupported.

## License

MIT. Third-party installers and extracted files remain subject to their
original licenses.
