# Software Specification: Windows Installer
## Matrice Pad Tempo

**Version:** 2.0

---

## 1. Overview

The installer packages the Windows App into a single downloadable `.exe` file. A user downloads it, double-clicks it, and after a UAC prompt the app is installed and registered to start at the next logon (and immediately, for the current session) — there is no multi-page wizard to click through beyond the single branded progress page (§12). The app is a **per-user process, not a Windows Service** (see `docs/spec-windows-app.md` §1 for why) — it runs in the interactive user's session, started via a Task Scheduler "At log on" trigger rather than the Service Control Manager. Uninstallation is handled through Windows **Add or Remove Programs** and removes all installed files and the scheduled task cleanly.

The product is branded **"Matrice Pad Tempo Companion"** (publisher: Matrice Technologies, Inc.) in the installer title, Add/Remove Programs entry, and the compiled exe's version metadata — distinct from "Matrice Pad Tempo," the hardware itself.

---

## 2. Technology

**NSIS** (Nullsoft Scriptable Install System), generating a single self-extracting `.exe`, via a `.nsi` script (`MatricePadApp/Package/Installer.nsi`) and a driving PowerShell build script (`MatricePadApp/build-installer.ps1`).

This deviates from an earlier draft of this spec, which called for the WiX Toolset (MSI + Bundle). Rationale for the change:

- **Portfolio consistency.** This developer's other WPF/MAUI installers (InvoicingApp, HeatingOilTracker, BookChat) all already use NSIS with a near-identical script template (`Package/Installer.nsi` + a `build-installer.ps1` that reads the version from the `.csproj` and shells out to `makensis`). Matching that keeps one installer toolchain across the portfolio instead of two.
- **Already installed, zero new toolchain.** `makensis.exe` was already present at `C:\Program Files (x86)\NSIS\`. WiX v4 was not installed at all and would have meant standing up a new toolchain, including the WiX `dotnet-bootstrapper` extension the original MSI/Bundle design needed for the .NET runtime prerequisite (§3).
- **MSI's enterprise benefits don't apply.** The original rationale for MSI — Group Policy deployment, enterprise silent-install tooling — doesn't matter for a personal/small-scale tool distributed via a single GitHub Release download (§14).

Trade-offs accepted with this choice, relative to what WiX would have given:
- No native upgrade mechanism equivalent to WiX's `MajorUpgrade`/`UpgradeCode` — handled instead by making the install section idempotent (§8).
- No downgrade blocking.
- Scheduled task registration is still a shell-out to `schtasks.exe` either way — NSIS doesn't make this any easier or harder than a WiX custom action would have.

---

## 3. Prerequisites

None. `MatricePadApp` is published **self-contained** (`dotnet publish -r win-x64 --self-contained true`, see §15), bundling the full .NET runtime into the install output. This removes the entire ".NET Runtime prerequisite check + silent bootstrap" problem an earlier draft of this spec solved with a WiX Bundle wrapping a `dotnet-bootstrapper` package — NSIS has no equivalent extension, and self-contained publish sidesteps needing one.

Trade-off: a larger installer (~57MB, vs. a few MB framework-dependent) — acceptable for a tool distributed as an occasional direct download rather than something update-checked constantly.

Windows 10 version 1903 or later is still implicitly required (WinRT `GlobalSystemMediaTransportControls` depends on it, per `docs/spec-windows-app.md`), but the installer does not actively verify this — it's not enforced, just assumed.

---

## 4. Installed Components

| File | Destination |
|---|---|
| `MatricePadApp.exe` | `C:\Program Files\MatricePad\` |
| `appsettings.json` | `C:\Program Files\MatricePad\` |
| All runtime `.dll` dependencies (self-contained publish output) | `C:\Program Files\MatricePad\` |
| `app.ico` | Embedded in `MatricePadApp.exe` and in the installer/uninstaller exe (not a separate installed file) |
| `README.md` | `C:\Program Files\MatricePad\` — end-user doc, see §4.1 |
| Uninstall entry | Windows Add or Remove Programs |

No desktop shortcut. No Start Menu entry. The app runs silently in the background, started by its scheduled task (§6) at logon.

`appsettings.json` is treated as user-configurable: the install section only copies it if it doesn't already exist at the destination, preserving any manual edits (e.g. a manually set `ComPort`) across upgrades. `appsettings.Development.json` is deliberately excluded from the published/installed output — it's a dev-only override (bumps file logging to Debug level) with no place in a production install.

### 4.1 End-User README

`MatricePadApp/README.md` — separate from the developer-facing repo-root `README.md`/`CLAUDE.md` — is copied into the install directory on every install/upgrade (always overwritten, unlike `appsettings.json`; it's static documentation, not user state). It exists because the app has no window, taskbar icon, or tray icon (§4, §1 of `docs/spec-windows-app.md`) — anyone who stumbles on `MatricePadApp.exe` running in Task Manager or finds the install folder has nothing else to tell them what it is. It covers:

- That the app auto-starts on install and on every login, and that having no visible window/icon is expected, not a bug
- How to stop it immediately (Task Manager, since there's no window to close)
- How to disable auto-start (Task Scheduler → find the `MatricePadApp` task → Disable or Delete)
- How to fully remove it (Add/Remove Programs)
- Where to find logs for troubleshooting (`%APPDATA%\MatricePad\logs\`)

---

## 5. Installation Directory

Default and only option: `C:\Program Files\MatricePad\`

The installer does not offer a directory picker. The install location is fixed to `Program Files` to ensure the app binary is in a UAC-protected location, even though the app itself runs per-user rather than as SYSTEM.

---

## 6. Scheduled Task Registration

Registered by shelling out to `schtasks.exe` from the install section, targeting the **current interactive user** — not `LocalSystem`:

| Property | Value |
|---|---|
| Task name | `MatricePadApp` |
| Trigger | At log on (`/SC ONLOGON`), current user |
| Action | Run `MatricePadApp.exe` from the install directory |
| Run level | Least privilege (`/RL LIMITED`) — no elevation; the app needs no admin rights at runtime |

Install sequence:
1. `taskkill /F /IM MatricePadApp.exe` and `schtasks /Delete /TN MatricePadApp /F` — best-effort, ignoring failure (nothing running/registered on a first install) — so re-running the installer over an existing install behaves as an upgrade (§8)
2. Copy files (§4)
3. Write registry uninstall entries (§11) and `WriteUninstaller`
4. `schtasks /Create ...` to register the task
5. `schtasks /Run /TN MatricePadApp` to launch it immediately — deliberately *not* a raw `Exec` of the exe, so the app runs under the task's own least-privilege security context from the start, rather than briefly inheriting the elevated installer's admin token until the next logon

Uninstall sequence: stop the process, delete the task (§9).

Because the task is registered per-user (not machine-wide), an installer run under one Windows account only starts the app for that account; a shared/multi-user PC would need the installer re-run (or the task re-registered) under each account that wants the Panel active — acceptable given the target use case (a single user's own desktop).

---

## 7. UAC

The installer requires elevation (`RequestExecutionLevel admin` in `Installer.nsi`) — needed to register the scheduled task and write to Program Files. Windows prompts for UAC on launch. The app itself runs unelevated at logon, per §6.

---

## 8. Upgrade Behavior

NSIS has no direct equivalent to WiX's `MajorUpgrade`/`UpgradeCode` mechanism. Instead, the install section (§6) is written to be idempotent: it unconditionally stops any running instance and deletes any existing scheduled task before proceeding, so simply re-running the installer over an existing install — at any version — behaves as an upgrade: files are overwritten (except `appsettings.json`, §4), the task is re-registered, and the app relaunches.

There is no downgrade blocking — running an older installer over a newer install is not prevented. Accepted trade-off; not enforced by the other NSIS installers in this developer's other projects either.

---

## 9. Uninstall Behavior

Triggered from Windows **Add or Remove Programs** → **Matrice Pad Tempo Companion** → **Uninstall**.

Uninstall sequence:
1. `taskkill /F /IM MatricePadApp.exe` — stop the running process, if any
2. `schtasks /Delete /TN MatricePadApp /F` — delete the scheduled task
3. A short (500ms) sleep, giving the OS a moment to release the just-killed process's file handles — `taskkill` returning doesn't guarantee they're freed instantly
4. `RMDir /r /REBOOTOK` on the install directory. The `/REBOOTOK` flag matters in practice: if something still has a handle open on the folder — most commonly File Explorer, which holds a directory handle just from having it open for browsing, not from any file actually being in use — a plain recursive delete can silently fail to remove the directory. `/REBOOTOK` schedules the leftover removal for the next reboot instead of abandoning it. (Observed once during manual testing: File Explorer open on the install folder blocked cleanup until this flag was added.)
5. Remove the Add/Remove Programs registry entries

`appsettings.json` is removed with the rest of the install directory on uninstall. Log files under `%APPDATA%\MatricePad\logs\` (per-user, outside the install directory) are deliberately left in place; nothing else needs cleanup.

No reboot is required for install or uninstall (the `/REBOOTOK` fallback in step 4 is a safety net for an edge case, not the normal path).

---

## 10. Silent Install

NSIS's native silent-install flag:

```
"Matrice Pad Tempo Companion 1.0.0 Installer.exe" /S
```

This deviates from an earlier draft of this spec, which specified `msiexec`-style `/quiet /norestart` flags (a WiX/MSI convention). `/S` is the standard silent-install convention NSIS provides and what every other installer in this developer's other projects uses.

---

## 11. Add or Remove Programs Entry

| Field | Value |
|---|---|
| Display name | Matrice Pad Tempo Companion |
| Publisher | Matrice Technologies, Inc. |
| Version | From `MatricePadApp.csproj`'s `<Version>`, passed to `makensis` at build time |
| Install location | `C:\Program Files\MatricePad\` |
| Estimated size | ~57 MB (self-contained .NET publish, §3) |

Written via `WriteRegStr`/`WriteRegDWORD` under `HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\MatricePadApp` (`NoModify`/`NoRepair` set to 1 — no Modify/Repair option, only Uninstall).

---

## 12. Branding

Source logo assets live in `MatricePadApp/Assets/`:
- `matrice tech logo instagram.jpg` — a 150×150 black square with a white "M" monogram
- `MATRICE-LOGO-WHITE-ALT23.png` — a white "MATRICE TECHNOLOGIES" wordmark on a transparent background

These were converted (via a one-off PowerShell + `System.Drawing` script — no ImageMagick was installed on the build machine, so a proper multi-resolution ICO container was hand-assembled rather than using a dedicated conversion tool) into two derived assets, both committed to the repo:

| Derived asset | Used for |
|---|---|
| `Assets/app.ico` | Multi-resolution (16–256px) icon, shared by **both** the installer/uninstaller exe (`MUI_ICON`/`MUI_UNICON`) and `MatricePadApp.exe` itself (`ApplicationIcon` in the `.csproj`) — one shared icon so the running process is recognizable by the same image used during install |
| `Assets/header.bmp` | The wordmark composited onto solid black (matching the icon's background, not left transparent — a transparent source flattened directly to BMP would either vanish against or corrupt against a differently-colored target), sized to NSIS's standard 150×57 header-banner slot (`MUI_HEADERIMAGE_BITMAP`). The source wordmark's native aspect ratio (2.61:1) happened to already be very close to the banner's (2.63:1), so the fit needed almost no letterboxing |

The installer's single progress page uses `MUI_PAGE_INSTFILES`/`MUI_UNPAGE_INSTFILES` (not the old-style `Page instfiles` command) specifically so the header banner renders — this required including `MUI2.nsh`, but does **not** add a Welcome/Directory-picker/Finish wizard flow; it's still just the one page.

Version metadata (`VIProductVersion`/`VIAddVersionKey` — ProductName, CompanyName, FileDescription, FileVersion, LegalCopyright) is embedded in the compiled installer exe, visible via its file Properties dialog in Windows.

**Known issue (as of 2026-07, unresolved):** the black header banner contrasts hard against the wizard page's default white background — flagged during review, not yet fixed. A future logo revision with a transparent or light background would sit more naturally; alternatively the header background itself could be recolored to match, but that's a bigger visual change (affects the whole page, not just the banner corner).

---

## 13. Code Signing

**Recommended** before public distribution, but not yet done:

- Sign the installer `.exe` and `MatricePadApp.exe` with an Authenticode certificate (EV certificate preferred to avoid Windows SmartScreen warnings on first download)
- Signing is a build pipeline step, independent of NSIS

Without code signing, Windows SmartScreen displays an "Unknown publisher" warning when the user runs the installer. Accepted for now — the same posture as an earlier draft of this spec took for initial development builds.

---

## 14. Distribution

Published as a downloadable asset on **GitHub Releases** (`https://github.com/bernpuc/matricepad_tempo/releases`), tagged per version (e.g. `v1.0.0`). No auto-update mechanism is in scope. Users download and run a new installer to upgrade; the upgrade path in §8 handles the old version cleanly.

---

## 15. Build Pipeline

```
MatricePadApp/build-installer.ps1
  1. Read <Version> from MatricePadApp.csproj
  2. dotnet publish -c Release -r win-x64 --self-contained true -o publish\win-x64
  3. Locate makensis.exe (checks the two standard install paths)
  4. makensis -DVERSION=<version> Package\Installer.nsi
  → MatricePadApp\Package\Matrice Pad Tempo Companion <version> Installer.exe
```

The built installer `.exe` is a gitignored build artifact (`MatricePadApp/Package/*.exe`), not committed to the repo — only `Installer.nsi` and `build-installer.ps1` are. Releasing a build means running `build-installer.ps1` and uploading the resulting exe to a GitHub Release (§14) by hand; there is no CI pipeline automating this yet.
