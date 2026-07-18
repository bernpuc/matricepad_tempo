# Matrice Pad Tempo Companion

This app feeds your Matrice Pad Tempo's screen — now-playing song/artist, volume, and the frequency bar graph — by watching what Windows is playing and sending it to the device over USB.

## What to expect

- **Starts automatically** right after you install it, and again every time you log in.
- **No window, no taskbar icon, no system tray icon.** It runs silently in the background — this is expected, not a bug.

## Stopping it right now

It has no window to close, so use Task Manager:

1. Open Task Manager (right-click the taskbar → **Task Manager**, or `Ctrl+Shift+Esc`).
2. On the **Details** tab (or **Processes**), find `MatricePadApp.exe`.
3. Right-click it → **End task**.

This stops it until the next login/restart.

## Stopping it from starting automatically

The app starts via a Windows scheduled task, not the Startup folder:

1. Open **Task Scheduler** (Start menu → search "Task Scheduler").
2. In the left pane, click **Task Scheduler Library**.
3. Find **MatricePadApp** in the list.
4. Right-click → **Disable** (keeps it around in case you want it back) or **Delete** (removes it entirely).

## Removing it completely

Use **Settings → Apps** (or Control Panel → **Programs and Features**) and uninstall **Matrice Pad Tempo Companion**. This removes the app, the scheduled task, and its installed files.

## Troubleshooting

Logs are written to `%APPDATA%\MatricePad\logs\`. If the Pad's screen isn't updating, that's the first place to check.
