import serial
import time
from comtypes import CLSCTX_ALL
from pycaw.pycaw import AudioUtilities, IAudioEndpointVolume, IAudioSessionManager2
from pycaw.constants import AudioSessionState
import win32gui
import win32process

# Set your COM port
ser = serial.Serial('COM10', 115200, timeout=1)
time.sleep(2)

# Set up audio device and volume interface
devices = AudioUtilities.GetSpeakers()
interface = devices.Activate(IAudioEndpointVolume._iid_, CLSCTX_ALL, None)
volume = interface.QueryInterface(IAudioEndpointVolume)

# Set up session manager for audio sessions
session_manager = devices.Activate(
    IAudioSessionManager2._iid_, CLSCTX_ALL, None
)
session_manager = session_manager.QueryInterface(IAudioSessionManager2)

def get_audio_playing_window_title():
    browser_names = {
        "chrome.exe": "Google Chrome",
        "firefox.exe": "Mozilla Firefox",
        "msedge.exe": "Microsoft Edge",
        "opera.exe": "Opera",
        # add other browsers if needed
    }

    sessions = AudioUtilities.GetAllSessions()
    for session in sessions:
        if session.State == AudioSessionState.Active:
            process = session.Process
            if process:
                pid = process.pid
                proc_name = process.name().lower()

                # If it's a browser, return browser name instead of title
                if proc_name in browser_names:
                    return browser_names[proc_name]+" - Web Browser Media"

                # Otherwise, get window titles for the process
                titles = []

                def enum_handler(hwnd, titles):
                    if win32gui.IsWindowVisible(hwnd):
                        _, window_pid = win32process.GetWindowThreadProcessId(hwnd)
                        if window_pid == pid:
                            title = win32gui.GetWindowText(hwnd)
                            if title and not title.isspace():
                                titles.append(title)
                    return True

                win32gui.EnumWindows(enum_handler, titles)
                if titles:
                    return titles[0]

    return "No media playing"


while True:
    try:
        # Handle incoming messages from Arduino
        if ser.in_waiting:
            line = ser.readline().decode('utf-8').strip()

            if line.startswith("VOL:"):
                try:
                    new_volume = int(line[4:])
                    new_volume = max(0, min(100, new_volume))  # clamp to 0–100
                    volume.SetMasterVolumeLevelScalar(new_volume / 100.0, None)
                    print(f"[Arduino -> PC] Volume set to {new_volume}%")
                except ValueError:
                    print("[Error] Invalid volume format:", line)

        # Get current media info
        current_volume = int(volume.GetMasterVolumeLevelScalar() * 100)
        title = get_audio_playing_window_title()

        if " - " in title:
            artist, song = title.split(" - ", 1)
        else:
            artist, song = title, ""

        artist = "[ " + artist + " ]"

        # Send formatted info to Arduino
        serial_output = f"{song.strip()}||{artist.strip()}||{current_volume}\n"
        ser.write(serial_output.encode('utf-8'))

        time.sleep(0.10)

    except Exception as e:
        print("Error:", e)
        time.sleep(0.10)
