# Software Specification: Firmware Updater
## Matrice Pad Tempo

**Version:** 1.2
**Status:** Implemented and verified live against real hardware — version check flow (§5), flash flow (§6), and the Start Menu shortcut (§8) are all shipped and installer-tested end to end. §7's Finish-page integration remains deliberately deferred, per that section's own reasoning; §10 lists remaining open questions.

---

## 1. Overview

`MatricePadApp` is a headless background service with no window or tray icon (`docs/spec-windows-app.md` §1) — when its serial version handshake (`CLAUDE.md`'s Serial Protocol section) detects a firmware/companion version mismatch, today all it can do is log a warning. There's no way for the person sitting at the PC to notice that, or to act on it, short of reading log files.

This spec adds a small, separate, user-facing tool — **MatricePad Firmware Updater** — that checks the connected board's firmware version and, if it's behind what this companion release expects, walks the user through flashing the bundled firmware. It is invoked deliberately (Start Menu shortcut, see §9), never automatically, given the higher failure cost of a bad firmware write versus a bad software update.

It builds directly on two things already shipped as of this session:
- The `VERSION?`/`PONG` handshake (`matrice_pad_tempo.ino`, `MatricePadApp/Services/SerialManager.cs`) — this spec does not change that protocol, it adds a second consumer of it.
- The discovery that the Leonardo's native-USB CDC stack requires **DTR (and RTS) actively asserted** before it reliably passes data — a real bug found and fixed in `SerialManager.cs` this session (it opened the port fine but silently never received a byte otherwise). Any new code that opens this port must do the same.

---

## 2. Architecture

**A genuinely separate project**, not a hidden flag on `MatricePadApp.exe`. `MatricePadApp` itself stays a pure headless `BackgroundService` — bolting a WinForms/WPF window mode onto a `Worker`/`Host.CreateApplicationBuilder` service is awkward, and this tool has its own lifecycle (launched once, does its thing, exits) that doesn't belong next to the always-running companion.

- **New project:** `MatricePadApp.FirmwareUpdater` — plain WPF, code-behind only, no MVVM/Prism/DI ceremony. This deviates from this developer's usual "Prism 9 + DryIoc for WPF" default, deliberately: a single-screen, single-purpose utility with no navigation and no testable business-logic surface beyond "call the shared library, show the result" doesn't earn that overhead.
- **New shared project:** `MatricePad.SerialCore` — a small class library holding exactly the logic that must not drift between the two exes:
  - VID/PID port discovery (the `FindArduinoPort()` WMI query already in `SerialManager.cs`)
  - The DTR/RTS-aware `SerialPort` open pattern
  - The `VERSION?` request / `PONG` response parse
  
  `SerialManager` is refactored to call into this library rather than keep its own copy. This is the one piece of this spec's architecture that touches existing, working code — the refactor should be a mechanical extraction (move methods, no behavior change), verified by re-running the same manual handshake test from this session (`dotnet run`, confirm the "Firmware handshake OK" log line still appears) before trusting it.

Both `MatricePadApp.FirmwareUpdater.csproj` and `MatricePadApp.csproj` reference `MatricePad.SerialCore.csproj`.

---

## 3. Firmware Package Bundling

**As implemented:** `MatricePadApp.FirmwareUpdater/stage-firmware.ps1` regenerates a gitignored `Firmware/` folder (not `build-installer.ps1` -- that's still installer-specific and untouched):
- Compiles the `.ino` via `arduino-cli` directly (same FQBN/libraries as `arduino/build.ps1`), reads `FIRMWARE_VERSION` out of the sketch source, and names the staged `.hex` after it (`matrice_pad_tempo-<version>.hex`) so the filename can never silently drift from what was actually compiled.
- Copies `avrdude.exe`/`avrdude.conf` from arduino-cli's own local install (`%LOCALAPPDATA%\Arduino15\packages\arduino\tools\avrdude\<version>\`) rather than requiring a separate download -- the same binary `arduino-cli`/`arduino/build.ps1` already shell out to.
- `Firmware/` is gitignored, matching the existing convention for build artifacts (the installer's own `.exe` output) plus a vendored third-party binary -- run the script before building/publishing the Updater, don't commit its output.

`MainWindow.xaml.cs`'s `ExpectedProtocolVersion`/`ExpectedFirmwareVersion`/`BundledHexFileName` constants must be kept in sync with the `.ino`'s `PROTOCOL_VERSION`/`FIRMWARE_VERSION` by hand for now -- `stage-firmware.ps1` prints a reminder if they might have drifted. Reading these from a manifest instead is still a TODO, not yet worth it with only one firmware build to track.

**Not implemented:** a SHA-256 checksum check on the bundled `.hex` before flashing. Skipped for now because the `.hex` is compiled locally and staged immediately before use, with no download/OTA step in between to protect against -- there's no untrusted transfer for a checksum to catch yet. Revisit if/when an OTA distribution path (still out of scope, §10) is ever added.

**`avrdude.exe` + `avrdude.conf` bundled** (via the staging script above), shelled out to with the same arguments `arduino-cli`'s own `platform.txt` recipe generates for a Leonardo (`-patmega32u4 -cavr109 -b57600 -D`, confirmed by reading `platform.txt`/`boards.txt` directly rather than guessing) — an earlier idea of a native C# avr109 client (for a single-signed-binary distribution story) was dropped in favor of this. Reasoning for the change:
- We already have a proven, working avrdude invocation in this repo, exercised live multiple times — implementing avr109 from scratch is real protocol work (page writes, per-page verify, chip-specific quirks avrdude's config already handles) that isn't worth the risk for a personal-scale tool.
- Code signing isn't done for *any* binary in this project yet (`spec-installer.md` §13 — "not yet done"). Bundling one more unsigned binary doesn't change the current security posture; revisit this choice together if/when signing is finally set up.

---

## 4. Bootloader Entry

Two-step approach, refined by testing live against the real board:

1. **Automatic 1200bps touch:** open then close the port at 1200 baud, then **discover the bootloader's own port by VID:PID rather than waiting for the same COM port name to reappear.** The bootloader is a distinct USB device from the running sketch, with its own enumeration — assuming the same port name reappears was tried first and failed outright (avrdude connected to the wrong, stale port). Worse, the VID:PID to search for isn't the obvious one either: this board's bootloader identifies as a **SparkFun Pro Micro bootloader (`1B4F:9205`)**, not an **Arduino Leonardo bootloader (`2341:0036`)** — the sketch is compiled with the `arduino:avr:leonardo` FQBN (same chip/protocol) so the *application*-mode board enumerates as "Arduino Leonardo" (`2341:8036`), but the bootloader itself keeps SparkFun's own identity regardless of which FQBN compiled what's currently flashed. The implementation checks both VID:PIDs so it also works against an actual Arduino-brand Leonardo. See `CLAUDE.md`'s "Serial Port Gotchas" for the general version of this note.
2. **Manual fallback:** if no bootloader-identity port appears within ~20s (matching `arduino/build.ps1`'s own discovery timeout), log: *"Double-tap the reset button on the board now, then click 'Update Firmware Now' again."* As implemented this is a log message plus a re-clickable button, not a live countdown UI — simpler than originally envisioned, sufficient in practice. The automatic touch was confirmed unreliable on this hardware during live testing, as anticipated from earlier development experience — budget for the manual-button-press path being the common case, not a rare fallback.

**Control-flow note, found only by testing the retry path live:** the companion must stay stopped, and the "Update Firmware Now" button must stay clickable, across a manual-retry cycle — an earlier version of this flow restarted the companion and hid the button as soon as the automatic touch failed, which re-locked the port and disabled the retry path at exactly the moment the user needed both to still be available.

---

## 5. Version Check Flow (read-only, safe to run without confirmation)

1. Enumerate candidate ports via VID/PID matching (`MatricePad.SerialCore`, mirrors `MatricePadOptions.ArduinoVidPidList`).
2. **Zero candidates:** *"No Matrice Pad board found — plug it in and try again."*
3. **More than one candidate:** list each by friendly name + COM port and require an explicit pick. Never silently guess — this dev machine alone enumerates COM9's Leonardo *and* several unrelated SparkFun Pro Micros (COM5–8, other personal projects); flashing the wrong one is a much worse mistake than the companion app picking the wrong port to send display packets to.
4. Open the chosen port with DTR/RTS asserted, send `VERSION?`, wait ~2s for `PONG||<protocol>||<firmware>`.
5. Three-way outcome:
   - **No response:** *"Couldn't detect a firmware version — this may be older firmware, or the board isn't fully connected."* Still offer to flash (with confirmation) — firmware predating the handshake can't identify itself any other way.
   - **Board's version < bundled version:** *"Update available: `<board>` → `<bundled>`."* Offer to flash.
   - **Board's version ≥ bundled version:** *"Firmware is already up to date."* No action offered — never auto-downgrade.

---

## 6. Update Flow (explicit confirmation required)

1. User confirms the offered update (clicks "Update Firmware Now").
2. `taskkill /F /IM MatricePadApp.exe` — stop the running companion so avrdude can get exclusive access to the port. (Unlike the installer's uninstall path, the scheduled task itself is *not* deleted — this is a temporary pause, not a removal.) Already done as part of the version check (§5) that preceded this, so this step is really "stays stopped," not a fresh stop.
3. Bootloader entry (§4). If the automatic touch fails, log the manual-reset instructions and return, keeping the companion stopped and the button clickable for a retry — no restart happens yet.
4. Flash via `avrdude`, streaming its console output live into the tool's own window so a failure is visible, not silent.
5. avrdude resets the board out of the bootloader automatically at the end of a normal invocation.
6. Re-discover the application-mode port by VID:PID (not the pre-flash port name, for the same reason as §4) and re-run the version check (§5) to confirm the new version is now reported.
7. `schtasks /Run /TN MatricePadApp` — restart the companion, but only once the flow has actually finished (terminal success or terminal failure) rather than while a manual-reset retry is still pending (§4's control-flow note).
8. Report success/failure clearly. On failure: *"Update failed, but your board is safe — the bootloader is protected and can't be damaged by this. Just try again."* (A normal avr109 application flash never touches the Caterina bootloader's protected boot section, so a failed/interrupted flash just leaves the application section to retry, not a bricked board.)

---

## 7. Installer Integration — Deliberately Deferred

`spec-installer.md` §12 documents that the installer today is **a single page** (`MUI_PAGE_INSTFILES` only — no Welcome, no Directory picker, no Finish page at all). Adding a `MUI_FINISHPAGE_RUN` checkbox (the idiomatic NSIS way to offer "run this after install") would mean introducing a Finish page where today there is none — a real change to the installer's deliberately minimal one-page flow, not a free addition.

**Decision for v1: skip installer integration.** Ship the Start Menu shortcut only (§8). This keeps the installer exactly as minimal as it is today, at the cost of the Updater being slightly less discoverable right after a fresh install. Revisit adding the Finish-page checkbox later if the Start Menu shortcut alone turns out to be too easy to miss in practice.

---

## 8. Start Menu Shortcut — Implemented

New to this feature — the main companion app deliberately has none (`spec-installer.md` §4: "No desktop shortcut. No Start Menu entry"), but that reasoning doesn't apply here: the Updater is something the user needs to actively find and launch, unlike the always-running background service.

**As implemented** (`Installer.nsi`), the Updater's self-contained publish output lives in its own `$INSTDIR\FirmwareUpdater\` subfolder rather than directly in `$INSTDIR` alongside `MatricePadApp.exe` — both are independently-published .NET apps, and keeping them apart avoids any risk of one's dependency DLLs overwriting the other's during install:

```nsis
SetOutPath "$INSTDIR\FirmwareUpdater"
File /r "${UPDATER_PUBLISH_DIR}\*.*"
SetOutPath "$INSTDIR"

CreateDirectory "$SMPROGRAMS\Matrice Pad Tempo Companion"
CreateShortcut "$SMPROGRAMS\Matrice Pad Tempo Companion\Matrice Pad Tempo Firmware Updater.lnk" "$INSTDIR\FirmwareUpdater\MatricePadApp.FirmwareUpdater.exe"
```

The Updater's own publish output is produced by `build-installer.ps1`, which now also runs `MatricePadApp.FirmwareUpdater/stage-firmware.ps1` (§3) before publishing it self-contained, mirroring exactly how `MatricePadApp` itself is published.

Removed on uninstall: the `FirmwareUpdater` subfolder goes with the rest of `$INSTDIR`'s recursive removal (`spec-installer.md` §9) automatically; the shortcut needs its own explicit `RMDir /r` since it lives under `$SMPROGRAMS`, not `$INSTDIR`.

Verified end to end: a real silent install (`/S`) correctly staged both apps, created the shortcut, and launching the Updater from that actual shortcut (not `dotnet run`) performed a successful version check against the real board.

---

## 9. Non-Functional Requirements

- Never flash automatically or silently — every flash requires an explicit user confirmation showing current → new version.
- Never guess which board to flash when more than one candidate is detected.
- Fully offline — the bundled `.hex` is the only source of truth for "latest" firmware, no network dependency, consistent with the rest of this project.
- Reuses `MatricePad.SerialCore` rather than re-implementing port detection or the handshake a second time.

---

## 10. Open Questions / Out of Scope

- OTA / remote update-package fetching — still out of scope.
- Whether the Updater ever gets folded into `MatricePadApp` itself as a tray-icon action — resolved for this spec's scope: separate app, definitively. Could still be revisited later if the companion ever grows a tray icon for other reasons.
- Code signing of `avrdude.exe` / the Updater exe — deferred until the installer's own signing story (`spec-installer.md` §13) is resolved; bundling one more unsigned binary doesn't change today's posture.
- Revisiting §7 (installer Finish-page integration) if the Start Menu shortcut proves too easy to miss.
