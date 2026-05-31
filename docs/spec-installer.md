# Software Specification: Windows Installer
## Matrice Pad Sound Panel

**Version:** 1.0

---

## 1. Overview

The installer packages the Windows Service into a single downloadable `.exe` file. A user downloads it, double-clicks it, clicks through a standard wizard, and the service is installed and running. Uninstallation is handled through Windows **Add or Remove Programs** and removes all installed files and the service registration cleanly.

---

## 2. Technology

**WiX Toolset v4** generating a standard Windows Installer (`.msi`) package, wrapped in a **WiX Bundle** (`.exe` bootstrapper).

Rationale:
- Native `ServiceInstall` and `ServiceControl` WiX elements handle service registration, start, stop, and removal without custom scripts.
- MSI is the standard format for Windows enterprise environments and is handled correctly by Add or Remove Programs, Group Policy deployment, and silent install scenarios.
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
| `MatricePadService.exe` | `[ProgramFiles64Folder]\MatricePad\` |
| `appsettings.json` | `[ProgramFiles64Folder]\MatricePad\` |
| All `.dll` dependencies | `[ProgramFiles64Folder]\MatricePad\` |
| Uninstall entry | Windows Add or Remove Programs |

No desktop shortcut. No Start Menu entry. The service runs silently in the background.

`appsettings.json` is marked as a **non-versioned configuration file**: upgrades do not overwrite it if it already exists, preserving any user configuration (e.g. a manually set `ComPort`).

---

## 5. Installation Directory

Default: `C:\Program Files\MatricePad\`

The installer does not offer a directory picker. The install location is fixed to `Program Files` to ensure the service binary is in a UAC-protected location.

---

## 6. Windows Service Registration

Registered via WiX `ServiceInstall` element:

| Property | Value |
|---|---|
| Service name | `MatricePadService` |
| Display name | `Matrice Pad Sound Panel` |
| Description | `Sends now-playing audio information to the Matrice Pad hardware panel.` |
| Start type | Automatic (delayed start) |
| Account | `LocalSystem` |
| Error control | Normal |

WiX `ServiceControl` elements:
- **On install:** start the service after installation completes
- **On uninstall:** stop the service before file removal
- **On upgrade:** stop the service before upgrade, start it after

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
1. Stop the `MatricePadService` Windows Service
2. Delete the service registration
3. Remove all installed files from `[ProgramFiles64Folder]\MatricePad\`
4. Remove the install directory if empty
5. Remove the Add or Remove Programs entry

`appsettings.json` is removed on uninstall. There is no separate user data to clean up as the service writes nothing outside its install directory.

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
- Sign `MatricePadService.exe`
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
