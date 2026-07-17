# Software Specification: Firmware Flasher
## Matrice Pad Sound Panel

**Version:** 1.0
**Status:** Concept — idea capture, not yet a committed design

---

## 1. Overview

Field firmware updates for the Panel today require the Arduino IDE or `arduino-cli` plus the `TempoCore` library on the end user's machine — fine for development, not something to hand a normal user. This spec captures the idea of an **updater** that can flash a new sketch onto the Panel using nothing but the Windows App installer (or a small companion utility), so firmware fixes can reach the field the same way software updates do.

The Panel's ATmega32U4 already ships with a Caterina bootloader (avr109 protocol) — the same one `arduino-cli upload`/the Arduino IDE use today. No new firmware capability is required to make this possible; the work is entirely on the PC side.

---

## 2. Technology

Two approaches were considered for speaking avr109 from .NET:

| Approach | Description | Trade-off |
|---|---|---|
| **Bundle `avrdude.exe`** (simpler) | Ship the same `avrdude.exe`/`avrdude.conf` that `arduino-cli` uses, shell out to it with the same args `build.ps1` already generates | Reuses proven, battle-tested tooling; adds an extra native binary to bundle and code-sign |
| **Native avr109 client in C#** (recommended) | Implement the avr109 request/response protocol directly over `System.IO.Ports.SerialPort` | Keeps the installer/updater fully self-contained (one signed binary, no bundled native tool); is real protocol-implementation work, but avr109 is a simple, well-documented ASCII/binary protocol (enter programming mode, write page, verify, exit) |

**Recommended:** native C# implementation, for a simpler distribution/signing story. Fall back to bundling `avrdude.exe` if the native protocol implementation turns out to have more edge cases than expected (e.g. chip-specific quirks already handled by avrdude's config file).

---

## 3. Bootloader Entry

Same two-step approach already proven out manually this session (`arduino/build.ps1`'s `-DiscoveryTimeout`):

1. **Automatic 1200bps touch:** open then immediately close the Panel's current serial port at 1200 baud. On a well-behaved Leonardo/Pro Micro this alone should trigger a self-reset into the bootloader.
2. **Manual fallback:** the touch reset was found unreliable on the actual hardware used during development — if the bootloader port doesn't appear within a few seconds, prompt the user: *"Double-tap the reset button on the Panel now"*, with a generous countdown (~20s, matching the port's real bootloader window) before giving up.

This means field updates likely can't be made fully hands-off for every unit — budget for the manual-button-press UX rather than promising a silent update.

---

## 4. Firmware Versioning

The current sketch has no way to report its own version. Add a minimal serial command to `TempoCore` or the sketch itself:

- PC sends `VERSION\n` (or similar) while the Panel is running its normal app (not the bootloader)
- Sketch responds with a version string (e.g. a `#define FIRMWARE_VERSION "1.3.0"` baked in at compile time)

The updater compares this against the bundled `.hex`'s known version before offering an update, and re-queries it after a successful flash to confirm the new version took effect. If the Panel doesn't respond (older firmware predating this feature, or the query times out), fall back to just offering the update unconditionally with a confirmation prompt.

---

## 5. Update Package Format

- The compiled `.hex` (built via the same `arduino/build.ps1` / `arduino-cli compile` pipeline used in this repo) ships as a versioned resource embedded in the installer, or as a separately downloadable update package fetched by a running service.
- Include a checksum (e.g. SHA-256) alongside the `.hex` so the updater can verify the artifact wasn't corrupted before attempting to flash it — a bad flash is a much worse failure mode than a bad software update.

---

## 6. Update Flow (UX)

Explicit, user-initiated action — **never automatic or silent**, given the higher failure cost of a bad firmware write:

1. User clicks "Check for firmware update" (in the Windows App's tray UI, a companion utility, or during installer upgrade)
2. Detect the Panel's COM port via VID:PID (reusing the existing auto-detection logic)
3. Query current firmware version (section 4); compare against the bundled/downloaded update
4. If an update is available, show a confirmation dialog (current version → new version) before proceeding
5. Attempt bootloader entry (section 3), falling back to the manual-button prompt if needed
6. Flash the `.hex` page-by-page, verifying each page as it's written
7. Reset the Panel out of the bootloader; re-query the version to confirm success
8. Report success/failure clearly, including what to do if it failed (see section 7)

---

## 7. Failure Handling / Recovery

The Caterina bootloader itself lives in a protected boot section that a normal avr109 application flash does not touch — so even a failed or interrupted update **cannot brick the board's ability to be reflashed**. If a flash fails partway:

- The application section may now be corrupt or empty, so the Panel won't run normally
- The board will still enter (or can be manually reset into) the same bootloader on the next attempt
- Recovery is just: retry the update flow from step 5

The updater should communicate this clearly on failure ("Update failed, but your Panel is safe — just try again") rather than leaving the user thinking they've bricked the device.

---

## 8. Non-Functional Requirements

- Never require the Arduino IDE or `arduino-cli` on the end user's machine
- Never flash automatically/silently without explicit user confirmation
- If bundling `avrdude.exe` (the fallback approach in section 2), it must be code-signed like the rest of the installer's binaries

---

## 9. Open Questions / Out of Scope

- Whether this lives inside `MatricePadApp` itself (a tray icon action) or as a standalone "MatricePad Updater" utility invoked from the installer — undecided
- Whether update packages are bundled into the installer at build time or fetched from a remote source at runtime — undecided, has real implications for how firmware fixes get distributed between installer releases
- Multi-board support, OTA/wireless update mechanisms — out of scope, consistent with the rest of this project's scope boundaries
