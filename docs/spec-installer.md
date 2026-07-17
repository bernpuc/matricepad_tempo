# Software Specification: Windows Installer
## Matrice Pad Sound Panel

**Version:** 1.1

---

## 1. Overview

The installer packages the Windows App into a single downloadable `.exe` file. A user downloads it, double-clicks it, clicks through a standard wizard, and the app is installed and registered to start at the next logon (and immediately, for the current session). The app is a **per-user process, not a Windows Service** (see `docs/spec-windows-app.md` §1 for why) — it runs in the interactive user's session, started via a Task Scheduler "At log on" trigger rather than the Service Control Manager. Uninstallation is handled through Windows **Add or Remove Programs** and removes all installed files and the scheduled task cleanly.

---

## 2. Technology

**WiX Toolset v4** generating a standard Windows Installer (`.msi`) package, wrapped in a **WiX Bundle** (`.exe` bootstrapper).

Rationale:
- MSI is the standard format for Windows enterprise environments and is handled correctly by Add or Remove Programs, Group Policy deployment, and silent install scenarios.
- Scheduled task registration (section 6) is not a native WiX element the way `ServiceInstall` is for services, so it's done via a small custom action (`schtasks.exe` invocation or the Task Scheduler COM API) run at install/uninstall time — the trade-off accepted for moving off the Windows Service model.
- Integrates cleanly with the .NET 9 publish pipeline (`dotnet publish` → WiX harvest → `.msi`).
- Proper upgrade support via `UpgradeCode` GUID — reinstalling a newer version automatically removes the old one.

---

## 3. Prerequisites

The installer must verify the following before proceeding:

| Prerequisite | Action if missing |
|---|---|
| Windows 10 version 1903 or later | Block install with message; WinRT `GlobalSystemMediaTransportControls` requires it |
| .NET 9 Runtime (Windows x64) | Download and run the .NET 9 Runtime installer silently as a bootstrapper prerequisite |

The .NET 9 Runtime check and silent install is handled by the **WiX Bundle** (`.exe` bootstrapper) that wraps the `.msi`. The user downloads and runs the `.exe`; the bundle installs the runtime if needed, then installs the `.msi`.

The distributed download is the **bundle `.exe`**, not the raw `.msi`.

---

## 4. Installed Components

| File | Destination |
|---|---|
| `MatricePadApp.exe` | `[ProgramFiles64Folder]\MatricePad\` |
| `appsettings.json` | `[ProgramFiles64Folder]\MatricePad\` |
| All `.dll` dependencies | `[ProgramFiles64Folder]\MatricePad\` |
| Uninstall entry | Windows Add or Remove Programs |

No desktop shortcut. No Start Menu entry. The app runs silently in the background, started by its scheduled task (section 6) at logon.

`appsettings.json` is marked as a **non-versioned configuration file**: upgrades do not overwrite it if it already exists, preserving any user configuration (e.g. a manually set `ComPort`).

---

## 5. Installation Directory

Default: `C:\Program Files\MatricePad\`

The installer does not offer a directory picker. The install location is fixed to `Program Files` to ensure the app binary is in a UAC-protected location, even though the app itself runs per-user rather than as SYSTEM.

---

## 6. Scheduled Task Registration

Registered via a custom action invoking `schtasks.exe` (or the Task Scheduler COM API) during the WiX install sequence, targeting the **current interactive user** — not `LocalSystem`:

| Property | Value |
|---|---|
| Task name | `MatricePadApp` |
| Trigger | At log on, current user |
| Action | Run `MatricePadApp.exe` from the install directory |
| Run level | Least privilege (no elevation — the app needs no admin rights at runtime) |
| Settings | Do not stop on battery, restart on failure (e.g. up to 3 times, 1-minute intervals) |

Install/uninstall/upgrade custom actions:
- **On install:** register the task, then launch `MatricePadApp.exe` immediately so it's running for the current session without requiring a fresh logon
- **On uninstall:** stop the running process (if any) and delete the scheduled task
- **On upgrade:** stop the running process, delete and re-register the task, relaunch

Because the task is registered per-user (not machine-wide), an installer run under one Windows account only starts the app for that account; a shared/multi-user PC would need the installer re-run (or the task re-registered) under each account that wants the Panel active — acceptable given the target use case (a single user's own desktop).

---

## 7. UAC

The installer requires elevation. The WiX bundle manifest declares `requireAdministrator`. Windows will prompt for UAC on launch.

---

## 8. Upgrade Behavior

The WiX package includes a stable `UpgradeCode` GUID that never changes across versions. The `MajorUpgrade` element is configured to:
- Automatically remove the previous version before installing the new one (major upgrade sequence)
- Block downgrade attempts with a user-visible error message
- Preserve `appsettings.json` (handled by the non-versioned file attribute in section 4)

Version number in the MSI `Product` element must be incremented on every release.

---

## 9. Uninstall Behavior

Triggered from Windows **Add or Remove Programs** → **Matrice Pad Sound Panel** → **Uninstall**.

Uninstall sequence:
1. Stop the running `MatricePadApp.exe` process, if any
2. Delete the `MatricePadApp` scheduled task
3. Remove all installed files from `[ProgramFiles64Folder]\MatricePad\`
4. Remove the install directory if empty
5. Remove the Add or Remove Programs entry

`appsettings.json` is removed on uninstall. Log files under `%APPDATA%\MatricePad\logs\` (per-user, outside the install directory — see `docs/spec-windows-app.md` §2) are left in place; nothing else needs cleanup.

No reboot is required for install or uninstall.

---

## 10. Silent Install

The bundle supports silent install for scripted/enterprise deployment:

```
MatricePadSoundPanel-Setup.exe /quiet /norestart
```

The MSI alone also supports:

```
msiexec /i MatricePadSoundPanel.msi /quiet /norestart
```

Silent uninstall:

```
msiexec /x MatricePadSoundPanel.msi /quiet /norestart
```

---

## 11. Add or Remove Programs Entry

| Field | Value |
|---|---|
| Display name | Matrice Pad Sound Panel |
| Publisher | *(to be set)* |
| Version | *(from product version)* |
| Install location | `C:\Program Files\MatricePad\` |
| Support URL | *(to be set)* |
| Estimated size | ~20 MB (includes .NET runtime if bundled) |

---

## 12. Code Signing

**Recommended** before public distribution:

- Sign the `.exe` bundle and the `.msi` with an Authenticode certificate (EV certificate preferred to avoid Windows SmartScreen warnings on first download)
- Sign `MatricePadApp.exe`
- Signing is a build pipeline step, not a WiX concern

Without code signing, Windows SmartScreen will display an "Unknown publisher" warning when the user runs the installer. This is acceptable for initial development builds.

---

## 13. Distribution

The installer `.exe` is hosted for direct download. No auto-update mechanism is in scope. Users download and run a new installer to upgrade; the upgrade path in section 8 handles the old version cleanly.

---

## 14. Build Pipeline

```
dotnet publish -c Release -r win-x64 --self-contained false
  → wix build MatricePad.wxs -o MatricePadSoundPanel.msi
  → wix build Bundle.wxs -o MatricePadSoundPanel-Setup.exe
```

The bundle `.wxs` references:
- The .NET 9 Runtime bootstrapper package (from the WiX `dotnet-bootstrapper` extension)
- The `MatricePadSoundPanel.msi`
