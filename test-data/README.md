# Test data

Local InstallerVISE fixtures used for compatibility and regression checks.
The archives are ignored by Git because they are third-party software.

| Archive | InstallerVISE | Expected result |
| --- | --- | --- |
| `EV_Installer_1.0.5.bin` | 5.5.2 | MacBinary |
| `ram-charger-installer-81.hqx` | Lite 3.6 | decode HQX, then extract with `unar` |
| `icondropper30.sit` | Lite 3.6 | extracts after `unar` |
| `barbabatch31.sit` | Lite 3.6 | extracts after `unar` |
| `utc-display.sit` | Lite 3.6 | extracts after `unar` |
| `ie30a-java.sit` | 4.2 | extracts after `unar` |
| `oms232-web.sit` | 4.2 | extracts after `unar` |
| `PGP50Freeware.hqx` | 4.5 | extracts after decoding HQX |
| `Installer_VISE_4.5.hqx` | 4.5 | extracts after decoding HQX |
| `installervise4.5.sea_.hqx` | 4.5 | outer HQX contains the preceding HQX archive |
| `Installer_VISE_4.6.1.sit` | 4.6.1 | extracts after `unar` |
| `mac-f2c-141.hqx` | 5.0.1 | decode HQX, then extract with `unar` |
| `startup-lock-25.hqx` | 5.5 | decode HQX, then extract with `unar` |
| `bbedit5.sit` | 5.5.1 | extracts after `unar` |
| `macpipes-227.hqx` | 5.5.2 | decode HQX, then extract with `unar` |
| `intellinews111.sit` | 5.5.2 | extracts after `unar` |
| `freemidi143.sit` | 5.5.2 | extracts after `unar` |
| `vise55.sit` | 5.5 | extracts after `unar` |
| `vise55-macbinary.bin` | 5.5 | MacBinary; matches `vise55.sit` |
| `vise60-normal.sit` | 6.0 | extracts after `unar` |
| `vise60-normal-macbinary.bin` | 6.0 | MacBinary; matches `vise60-normal.sit` |
| `vise60-active.sit` | 6.0 Active Install | reports missing external payload |
| `vise60-active-macbinary.bin` | 6.0 Active Install | matches `vise60-active.sit` |
| `vise601.sit` | 6.0.1 | extracts after `unar` |
| `vise65.sit` | 6.5 | extracts after `unar` |
| `vise65-macbinary.bin` | 6.5 | MacBinary; matches `vise65.sit` |
| `icontrol12.sit` | 6.5 | extracts after `unar` |
| `interarchy40.sit` | 7.0 | extracts after `unar` |
| `interarchy38.sit` | 7.0 | extracts after `unar` |
| `TclTk_8.3.2p1_RuntimeInstall.bin` | 7.0.1 | MacBinary; extracts after `unar` |
| `TclTk_8.3.2_WebInstall.bin` | 7.0.1 Active Install | reports missing external payload |
| `ljlegacy-en.sit` | 7.3 | extracts after `unar` |
| `MacPython223full.bin` | 8.0.2 | extracts after removing MacBinary |
| `MacPython223full.hqx` | 8.0.2 | matches `MacPython223full.bin` after decoding |
| `MacPython233full.bin` | 8.0.2 | extracts after removing MacBinary |
| `MacPython233full.hqx` | 8.0.2 | matches `MacPython233full.bin` after decoding |
| `IVISE8.5_NormalInstall.bin` | 8.5 | extracts after removing MacBinary |
| `InstallerVISE85.sit_.bin` | 8.5 | contains the preceding installer |

`SHA256SUMS` records the local files exactly.

`ljlegacy-en.sit` is HP's public legacy LaserJet installer from
`https://ftp.hp.com/pub/softlib/software4/lj606/lj-29264-1/ljlegacy-en.sit`.

All 38 fixtures unwrap with the current `unar`. Nested HQX/StuffIt fixtures
require two explicit `unar` stages on macOS. Full extraction succeeds for 35.
The two VISE 6 Active Install files contain the same stub; the independent
Tcl/Tk VISE 7.0.1 web installer is another stub. All three correctly report
that their external payload is missing.

The Tcl/Tk files are the original SourceForge releases:
`https://sourceforge.net/projects/tcl/files/Tcl/8.3.2/`.

The `.bin` and `.hqx` copies of each MacPython installer decode to identical
data and resource forks. They are retained to test both transport paths.

For the original 25-file corpus, Debian's `macutils` 2.0b3 removes all five
MacBinary wrappers; `unvise` then gives four complete extractions and the same
expected web-stub result. It decodes the BinHex layer of the four original HQX
fixtures, but their enclosed StuffIt archives use compression methods too new
for `macunpack`. The 16 direct StuffIt fixtures are likewise newer than its
StuffIt reader.
