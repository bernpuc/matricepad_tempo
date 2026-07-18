; Installer.nsi
; NSIS installer for the Matrice Pad Tempo Companion Windows App.
; Usage locally: makensis -DVERSION=1.0.0 Installer.nsi
; In CI: makensis -DVERSION=%VERSION% Installer.nsi
;
; Unlike the other NSIS installers in this developer's other projects
; (InvoicingApp, HeatingOilTracker, BookChat), this one has no directory
; picker or shortcuts -- it registers a Task Scheduler "at logon" entry
; instead, per docs/spec-installer.md. Published self-contained (see
; build-installer.ps1), so there is no .NET runtime prerequisite to check.

!define APP_PUBLISHER "Matrice Technologies, Inc."
!define APP_NAME "Matrice Pad Tempo Companion"
!define TASK_NAME "MatricePadApp"
!define EXE_NAME "MatricePadApp.exe"
!define INSTALL_DIR_NAME "MatricePad"
!ifndef VERSION
  !define VERSION "1.0.0"
!endif
!define INSTALLER_EXE "${APP_NAME} ${VERSION} Installer.exe"
!define PUBLISH_DIR "..\publish\win-x64" ; relative to this .nsi file -- see build-installer.ps1

; The installer needs elevation to register the scheduled task and write to
; Program Files. The app itself runs at logon with no elevation (RL LIMITED
; below).
RequestExecutionLevel admin

; Branding: app.ico is the 150x150 black "M" monogram (Assets/matrice tech
; logo instagram.jpg), converted to a multi-resolution ICO. header.bmp is the
; white "MATRICE TECHNOLOGIES" wordmark (Assets/MATRICE-LOGO-WHITE-ALT23.png,
; originally transparent) composited onto matching black at 150x57, the
; standard MUI header-banner size -- see scratchpad/Convert-ImageToIco.ps1
; used to generate both from the source assets.
!include "MUI2.nsh"
!define MUI_ICON "..\Assets\app.ico"
!define MUI_UNICON "..\Assets\app.ico"
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_BITMAP "..\Assets\header.bmp"
!define MUI_HEADERIMAGE_RIGHT

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "CompanyName" "${APP_PUBLISHER}"
VIAddVersionKey "FileDescription" "${APP_NAME} Installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "${APP_PUBLISHER}"

Name "${APP_NAME} ${VERSION}"
OutFile "${INSTALLER_EXE}"
InstallDir "$PROGRAMFILES64\${INSTALL_DIR_NAME}"

; No directory picker -- fixed install location per docs/spec-installer.md §5.
; Still routed through the MUI page macro (not the old-style `Page instfiles`)
; so the branded header banner renders on it.
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "Install"
  ; Stop any running instance and remove any existing scheduled task first, so
  ; re-running this installer over an existing install behaves as an upgrade
  ; (there is no WiX-style MajorUpgrade/UpgradeCode equivalent here).
  nsExec::ExecToLog 'taskkill /F /IM "${EXE_NAME}"'
  Pop $0
  nsExec::ExecToLog 'schtasks /Delete /TN "${TASK_NAME}" /F'
  Pop $0

  SetOutPath "$INSTDIR"

  ; appsettings.json is user-configurable (e.g. a manually set ComPort) --
  ; don't overwrite an existing one on upgrade.
  IfFileExists "$INSTDIR\appsettings.json" appsettings_exists appsettings_copy
  appsettings_copy:
    File "${PUBLISH_DIR}\appsettings.json"
  appsettings_exists:

  ; Everything else in the publish output. appsettings.json is handled above;
  ; appsettings.Development.json is a dev-only artifact, not shipped.
  File /r /x "appsettings.json" /x "appsettings.Development.json" "${PUBLISH_DIR}\*.*"

  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${TASK_NAME}" "DisplayName" "${APP_NAME}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${TASK_NAME}" "UninstallString" "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${TASK_NAME}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${TASK_NAME}" "Publisher" "${APP_PUBLISHER}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${TASK_NAME}" "InstallLocation" "$INSTDIR"
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${TASK_NAME}" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${TASK_NAME}" "NoRepair" 1

  WriteRegStr HKLM "Software\${TASK_NAME}" "InstallPath" "$INSTDIR"

  WriteUninstaller "$INSTDIR\Uninstall.exe"

  ; Register the scheduled task: current interactive user, at-logon trigger,
  ; least privilege (the app needs no admin rights at runtime -- only this
  ; installer, running elevated, needs it to register the task itself).
  nsExec::ExecToLog 'schtasks /Create /TN "${TASK_NAME}" /TR "\"$INSTDIR\${EXE_NAME}\"" /SC ONLOGON /RL LIMITED /F'
  Pop $0

  ; Launch immediately via the task itself (not a raw Exec) so it runs under
  ; the task's own least-privilege context rather than inheriting this
  ; elevated installer's admin token -- gets it running for the current
  ; session without requiring a fresh logon.
  nsExec::ExecToLog 'schtasks /Run /TN "${TASK_NAME}"'
  Pop $0
SectionEnd

Section "Uninstall"
  nsExec::ExecToLog 'taskkill /F /IM "${EXE_NAME}"'
  Pop $0
  nsExec::ExecToLog 'schtasks /Delete /TN "${TASK_NAME}" /F'
  Pop $0

  ; Give the OS a moment to release the just-killed process's file handles
  ; before deleting -- taskkill returning doesn't guarantee handles are freed
  ; instantly.
  Sleep 500

  ; Log files under %APPDATA%\MatricePad\logs\ are outside $INSTDIR and
  ; deliberately left in place, per docs/spec-installer.md §9.
  ;
  ; /REBOOTOK: if something still has a handle open on $INSTDIR or a file in
  ; it -- most commonly File Explorer, which holds a directory handle just
  ; from having it open for browsing, not from any file being in use -- RMDir
  ; can otherwise silently fail to remove the directory. /REBOOTOK schedules
  ; the leftover removal for next reboot instead of abandoning it.
  RMDir /r /REBOOTOK "$INSTDIR"

  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${TASK_NAME}"
  DeleteRegKey HKLM "Software\${TASK_NAME}"
SectionEnd
