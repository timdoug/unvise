# Test data

Installer VISE fixtures used for compatibility and regression checks. The
third-party archives are stored locally and ignored by Git; `SHA256SUMS`
identifies their exact contents.

## Original fixtures

| Archive | Installer VISE | Wrapper or expected behavior |
| --- | --- | --- |
| `ram-charger-installer-81.hqx` | Lite 3.6 | StuffIt inside BinHex; extract with `unar` |
| `icondropper30.sit` | Lite 3.6 | extracts after `unar` |
| `barbabatch31.sit` | Lite 3.6 | extracts after `unar` |
| `utc-display.sit` | Lite 3.6 | extracts after `unar` |
| `surfjet-demo-installer-10.hqx` | Lite 3.6 | deep hierarchy and large shared payload groups |
| `ie30a-java.sit` | 4.2 | extracts after `unar` |
| `oms232-web.sit` | 4.2 | extracts after `unar` |
| `PGP50Freeware.hqx` | 4.5 | direct BinHex |
| `Installer_VISE_4.5.hqx` | 4.5 | direct BinHex |
| `installervise4.5.sea_.hqx` | 4.5 | nested BinHex; decoded recursively |
| `Installer_VISE_4.6.1.sit` | 4.6.1 | extracts after `unar` |
| `mac-f2c-141.hqx` | 5.0.1 | StuffIt inside BinHex; extract with `unar` |
| `startup-lock-25.hqx` | 5.5 | StuffIt inside BinHex; extract with `unar` |
| `vise55.sit` | 5.5 | extracts after `unar` |
| `vise55-macbinary.bin` | 5.5 | MacBinary; matches `vise55.sit` |
| `bbedit5.sit` | 5.5.1 | extracts after `unar` |
| `dos-mounter-98-demo.hqx` | 5.5.1 | relocated initializer dictionary and extended move-action messages |
| `EV_Installer_1.0.5.bin` | 5.5.2 | direct MacBinary |
| `macpipes-227.hqx` | 5.5.2 | StuffIt inside BinHex; extract with `unar` |
| `intellinews111.sit` | 5.5.2 | extracts after `unar` |
| `freemidi143.sit` | 5.5.2 | extracts after `unar` |
| `vise60-normal.sit` | 6.0 | extracts after `unar` |
| `vise60-normal-macbinary.bin` | 6.0 | MacBinary; matches `vise60-normal.sit` |
| `vise60-active.sit` | 6.0 Active Install | reports missing external payload |
| `vise60-active-macbinary.bin` | 6.0 Active Install | matches `vise60-active.sit` |
| `vise601.sit` | 6.0.1 | extracts after `unar` |
| `vise65.sit` | 6.5 | extracts after `unar` |
| `vise65-macbinary.bin` | 6.5 | MacBinary; matches `vise65.sit` |
| `icontrol12.sit` | 6.5 | extracts after `unar` |
| `install_aol_5.0.hqx` | 6.5 | action-heavy catalog with shared payload groups |
| `super-lock-pro-451.hqx` | 6.5 | action-heavy catalog with an unpaired base-dependent update |
| `interarchy40.sit` | 7.0 | extracts after `unar` |
| `interarchy38.sit` | 7.0 | extracts after `unar` |
| `TclTk_8.3.2p1_RuntimeInstall.bin` | 7.0.1 | direct MacBinary |
| `TclTk_8.3.2_WebInstall.bin` | 7.0.1 Active Install | reports missing external payload |
| `default-folder-312.hqx` | 7.0.1 | extracts complete files; recognizes paired base-dependent updates |
| `AIM_4.3.hqx` | 7.0.1 | uncompressed wide catalog with action records |
| `TclTk_8.3.3_RuntimeInstall.bin` | 7.2 | direct MacBinary |
| `TclTk_8.3.3_FullInstall.bin` | 7.2 | direct MacBinary |
| `TclTk_8.3.3_WebInstall.bin` | 7.2 Active Install | reports missing external payload |
| `ljlegacy-en.sit` | 7.3 | extracts after `unar` |
| `c7200_install_v11.hqx` | 7.3 | extracts complete files; recognizes paired base-dependent updates |
| `ce_install_v231.hqx` | 7.4 | StuffIt inside BinHex; extract with `unar` |
| `TclTk_8.3.4_FullInstall.bin` | 7.4 | direct MacBinary |
| `MacPython223full.bin` | 8.0.2 | direct MacBinary |
| `MacPython223full.hqx` | 8.0.2 | direct BinHex; matches the `.bin` fixture |
| `MacPython233full.bin` | 8.0.2 | direct MacBinary |
| `MacPython233full.hqx` | 8.0.2 | direct BinHex; matches the `.bin` fixture |
| `MacTclTk_8.3.5_RuntimeInstl.bin` | 8.0.2 | direct MacBinary |
| `MacTclTk_8.3.5_FullInstall.bin` | 8.0.2 | direct MacBinary |
| `MacTclTk_8.4.1_WebInstall.bin` | 8.0.2 Active Install | reports missing external payload |
| `IVISE8.5_NormalInstall.bin` | 8.5 | direct MacBinary |
| `InstallerVISE85.sit_.bin` | 8.5 | StuffIt inside MacBinary; contains the preceding installer |

## File-format sample corpus

The 38 additional files from Sembiance's
[`installerVISE` sample directory](https://sembiance.com/fileFormatSamples/archive/installerVISE/)
normalize to 36 distinct Installer VISE applications:

- 32 have complete local payloads and extract successfully.
- `Installer` and `インストーラ` are multi-segment catalogs. Their local
  members extract; members assigned to the absent media segments are listed
  but not emitted.
- `Install Secret Paths` and `Install ScreenSaver` arrived as flat data forks
  without their resource forks and therefore cannot be decoded.
- `Tropico Demo Installer.data` is the companion payload archive for `Tropico
  Demo Installer`; keeping the files together exercises automatic companion
  discovery.
- Seven `.bin` files contain two nested MacBinary layers and yield six distinct
  applications; the two Office Manager wrappers decode identically. Both
  layers are intentionally retained as transport fixtures.
- Cinepak's inner MacBinary header has an invalid CRC but valid fork bounds and
  contents. `unvise` tolerates the bad transport checksum and extracts the
  enclosed installer.

This set covers SVCT generations 0, 2, and 3; short direct and embedded-`FVCT`
payloads; media segments; a companion payload archive; revisions 13 and 14;
late action records; and late base-dependent updates.

## Coverage

- 91 downloaded fixtures represent 81 distinct Installer VISE applications.
- 72 distinct applications contain complete local payloads and extract
  successfully.
- One application contains self-contained files plus an unpaired base-dependent
  update. Its files extract successfully and the unavailable update is listed
  without being decoded against the wrong history.
- Four distinct applications are Active Install stubs. Five outer fixtures
  represent them because the two VISE 6 wrappers contain the same
  stub; the VISE 7.0.1, 7.2, and 8.0.2 stubs are independent. All five report
  the unsupported external payload rather than producing a partial extraction.
- Two multi-segment applications extract their local members and report the
  absent media members.
- Two flat uploads cannot be decoded because their resource forks were lost
  before archival.
- The paired `.bin` and `.hqx` MacPython fixtures decode to identical data and
  resource forks. Both forms are retained to cover their transport paths.

## Outer archives

- `unvise` directly decodes the 59 fixtures whose transport chain consists
  only of MacBinary and/or BinHex. This includes all seven nested-MacBinary
  fixtures and the nested-BinHex Installer VISE 4.5 fixture.
- 29 fixtures contain StuffIt somewhere in their wrapper chain and still need
  an external StuffIt extractor. The remaining standalone MacBinary fixture is
  Tropico's companion payload rather than an installer.
- In the original 50-file set, `unar` unwraps 49 fixtures completely. Its
  MacBinary reader fails on the
  18 MB Tcl/Tk 8.3.4 file; splitting that file's forks directly produces a
  valid installer.
- HQX/StuffIt fixtures still require an external StuffIt extractor; `unar`
  can remove the complete chain while retaining the final AppleDouble sidecar.
- Debian `macutils` 2.0b3 was checked against a 25-fixture subset. It removes
  all five MacBinary wrappers in that subset, yielding four complete
  extractions and one expected Active Install result. It also decodes the four
  BinHex layers, but their enclosed StuffIt compression methods and the 16
  direct StuffIt archives are newer than its StuffIt reader.

## Sources

- The Tcl/Tk files are original releases from
  `https://sourceforge.net/projects/tcl/files/Tcl/`.
- `ljlegacy-en.sit` is HP's public legacy LaserJet installer from
  `https://ftp.hp.com/pub/softlib/software4/lj606/lj-29264-1/ljlegacy-en.sit`.
- `ce_install_v231.hqx` is Sonnet's Crescendo/Encore 2.3.1 installer from
  `https://www.sonnettech.com/downloads/software/ce_install_v231.hqx`.
- `c7200_install_v11.hqx` is Sonnet's Crescendo 7200 1.1 installer from
  `https://www.sonnettech.com/downloads/software/c7200_install_v11.hqx`.
- `default-folder-312.hqx` is St. Clair Software's Default Folder 3.1.2 from
  `https://ftp.lip6.fr/pub/mac/info-mac/gui/default-folder-312.hqx`.
- `super-lock-pro-451.hqx` is TriVectus' SuperLock Pro 4.5.1 from
  `https://ftp.lip6.fr/pub/mac/info-mac/disk/super-lock-pro-451.hqx`.
- `surfjet-demo-installer-10.hqx` is the SurfJet Agent 1.0 demo from
  `https://ftp.lip6.fr/pub/mac/info-mac/inet/surfjet-demo-installer-10.hqx`.
- `dos-mounter-98-demo.hqx` is Software Architects' DOS Mounter 98 demo from
  `https://ftp.lip6.fr/pub/mac/info-mac/app/dos-mounter-98-demo.hqx`.
- `install_aol_5.0.hqx` is America Online 5.0 from the preserved PPCMLA
  downloads directory at `https://ppcmla.org/downloads/`.
- `AIM_4.3.hqx` is AOL Instant Messenger 4.3 from the same preserved PPCMLA
  downloads directory.
- The 38 file-format samples came from Sembiance's public Installer VISE sample
  directory linked above.
