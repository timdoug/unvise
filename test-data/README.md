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

`SHA256SUMS` records the local files exactly.

All 25 fixtures unwrap with `unar -k hidden` and are accepted by `unvise`; the
four nested HQX/StuffIt fixtures require two explicit `unar` stages on macOS.
Full extraction succeeds for 23; the two VISE 6 Active Install copies contain
the same stub and correctly report that its external payload is missing.

Debian's `macutils` 2.0b3 removes all five MacBinary wrappers, after which
`unvise` gives four complete extractions and the same expected web-stub result.
It decodes the BinHex layer of all four HQX fixtures, but their enclosed
StuffIt archives use compression methods too new for `macunpack`. The 16
direct StuffIt fixtures are likewise newer than its StuffIt reader.
