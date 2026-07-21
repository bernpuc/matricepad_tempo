# Software Specification: Firmware Updater
## Matrice Pad Tempo

**Version:** 1.0
**Status:** Committed design — supersedes the version-check/UX-flow portions of `docs/spec-flasher.md` (still "Concept" status); the OTA/remote-package question in that doc's §9 remains open and out of scope here too.

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

- A prebuilt `.hex`, compiled once via `arduino/build.ps1`'s compile-only path (no `-Port`) as a step in `build-installer.ps1`, copied into the Updater's own publish output. Version-tagged to match `FIRMWARE_VERSION` in the `.ino` (currently `"1.1.0"`).
- A SHA-256 checksum alongside it, verified before flashing — a corrupted flash is a much worse failure mode than a corrupted software update (`spec-flasher.md` §5).
- **`avrdude.exe` + `avrdude.conf` bundled directly**, shelled out to with the same arguments `arduino/build.ps1` already generates successfully (used repeatedly and successfully this very session, including recovering the board after a freeze).

**This deviates from `spec-flasher.md` §2's recommendation** (native C# avr109 client, for a single-signed-binary distribution story). Reasoning for the change:
- We already have a proven, working avrdude invocation in this repo, exercised live multiple times this session — implementing avr109 from scratch is real protocol work (page writes, per-page verify, chip-specific quirks avrdude's config already handles) that isn't worth the risk for a personal-scale tool.
- Code signing isn't done for *any* binary in this project yet (`spec-installer.md` §13 — "not yet done"). Bundling one more unsigned binary doesn't change the current security posture; revisit this choice together if/when signing is finally set up.

---

## 4. Bootloader Entry

Same two-step approach `spec-flasher.md` §3 already laid out, now informed by this session's DTR finding:

1. **Automatic 1200bps touch:** open then close the port at 1200 baud. This is exactly the same DTR-driven reset mechanism `arduino-cli`/`avrdude` already rely on for Leonardo-class boards — the same control-line assertion this session found is also required for *normal* data flow.
2. **Manual fallback:** if the bootloader's COM port doesn't reappear within ~20s (matching `arduino/build.ps1`'s own discovery timeout), prompt: *"Double-tap the reset button on the board now,"* with a visible countdown, rather than promising a silent hands-off update — this was already found unreliable on the real hardware during earlier development.

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

1. User confirms the offered update.
2. `taskkill /F /IM MatricePadApp.exe` — stop the running companion so avrdude can get exclusive access to the port. (Unlike the installer's uninstall path, the scheduled task itself is *not* deleted — this is a temporary pause, not a removal.)
3. Verify the bundled `.hex`'s checksum (§3).
4. Bootloader entry (§4).
5. Flash via `avrdude`, streaming its console output live into the tool's own window so a failure is visible, not silent.
6. avrdude resets the board out of the bootloader automatically at the end of a normal invocation.
7. Re-run the version check (§5) to confirm the new version is now reported.
8. `schtasks /Run /TN MatricePadApp` — restart the companion.
9. Report success/failure clearly. On failure: *"Update failed, but your board is safe — the bootloader is protected and can't be damaged by this. Just try again."* (`spec-flasher.md` §7 — a normal avr109 application flash never touches the Caterina bootloader's protected boot section, so a failed/interrupted flash just leaves the application section to retry, not a bricked board.)

---

## 7. Installer Integration — Deliberately Deferred

`spec-installer.md` §12 documents that the installer today is **a single page** (`MUI_PAGE_INSTFILES` only — no Welcome, no Directory picker, no Finish page at all). Adding a `MUI_FINISHPAGE_RUN` checkbox (the idiomatic NSIS way to offer "run this after install") would mean introducing a Finish page where today there is none — a real change to the installer's deliberately minimal one-page flow, not a free addition.

**Decision for v1: skip installer integration.** Ship the Start Menu shortcut only (§8). This keeps the installer exactly as minimal as it is today, at the cost of the Updater being slightly less discoverable right after a fresh install. Revisit adding the Finish-page checkbox later if the Start Menu shortcut alone turns out to be too easy to miss in practice.

---

## 8. Start Menu Shortcut

New to this feature — the main companion app deliberately has none (`spec-installer.md` §4: "No desktop shortcut. No Start Menu entry"), but that reasoning doesn't apply here: the Updater is something the user needs to actively find and launch, unlike the always-running background service.

```nsis
CreateDirectory "$SMPROGRAMS\Matrice Pad Tempo Companion"
CreateShortcut "$SMPROGRAMS\Matrice Pad Tempo Companion\Check Firmware Version.lnk" "$INSTDIR\MatricePadApp.FirmwareUpdater.exe"
```
Removed on uninstall alongside the rest of `$INSTDIR` (`spec-installer.md` §9 already recursively removes the install directory; the shortcut folder needs an explicit `RMDir` since it lives under `$SMPROGRAMS`, not `$INSTDIR`).

---

## 9. Non-Functional Requirements

- Never flash automatically or silently — every flash requires an explicit user confirmation showing current → new version.
- Never guess which board to flash when more than one candidate is detected.
- Fully offline — the bundled `.hex` is the only source of truth for "latest" firmware, no network dependency, consistent with the rest of this project.
- Reuses `MatricePad.SerialCore` rather than re-implementing port detection or the handshake a second time.

---

## 10. Open Questions / Out of Scope

- OTA / remote update-package fetching — still out of scope, unchanged from `spec-flasher.md` §9.
- Whether the Updater ever gets folded into `MatricePadApp` itself as a tray-icon action — `spec-flasher.md`'s open question on this is now resolved *for this spec's scope*: separate app, definitively. Could still be revisited later if the companion ever grows a tray icon for other reasons.
- Code signing of `avrdude.exe` / the Updater exe — deferred until the installer's own signing story (`spec-installer.md` §13) is resolved; bundling one more unsigned binary doesn't change today's posture.
- Revisiting §7 (installer Finish-page integration) if the Start Menu shortcut proves too easy to miss.
