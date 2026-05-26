import serial
import time
import datetime
from comtypes import CLSCTX_ALL
from pycaw.pycaw import AudioUtilities, IAudioEndpointVolume, IAudioSessionManager2
from pycaw.constants import AudioSessionState
import win32gui
import win32process
import re
import asyncio
from winrt.windows.media.control import \
    GlobalSystemMediaTransportControlsSessionManager as MediaManager

# Set your COM port
ser = serial.Serial('COM4', 115200, timeout=1)
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

# Store an initial timestamp in the past so it fires immediately first time thru
last_fired_time = datetime.datetime.now() - datetime.timedelta(seconds=60)

# Define the minimum time delta 
time_delta_threshold = datetime.timedelta(seconds=5)  # Example: 5 seconds

# Microsoft Zune. title field only. Formatted with '<track number> <artist> - <song title>'
def get_title_song(input_string):
    regex = r"(\d+)\s(.*)\s-\s(.*)"

    # Attempt to match the pattern in the 'media' string
    match = re.search(regex, input_string)

    # Check if a match was found
    if match:
       # Extract the captured groups
        track = match.group(1)
        artist = match.group(2)
        song = match.group(3)

        # Print the extracted information
        print(f"Track: {track}")
        print(f"Artist: {artist}")
        print(f"Song: {song}")
    else:
        print("No match found.")
        song = ""
        artist = ""
    return song, artist


async def get_media_info():
    sessions = await MediaManager.request_async()

    current_session = sessions.get_current_session()
    if current_session:  # there needs to be a media session running
        info = await current_session.try_get_media_properties_async()
        # Additional fields (usually blank for streaming sources)
        # album_artist
        # album_title
        # album_track_count
        # artist
        # subtitle
        # title
        # track_number
        # These seemed to be broken (Traceback) genres, thumbnail, playback_type
        
        info_dict = {"artist": info.artist, "title": info.title}
        return info_dict

def get_audio_playing_window_title():
    browser_names = {
        "chrome.exe": "Google Chrome",
        "firefox.exe": "Mozilla Firefox",
        "msedge.exe": "Microsoft Edge",
        "opera.exe": "Opera",
        "microsoft.media.player.exe": "Media Player",
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
        window_title = get_audio_playing_window_title()

        # Calculate the time difference (time delta)
        current_time = datetime.datetime.now()
        time_difference = current_time - last_fired_time

        # Check if the time delta is greater than the threshold
        if time_difference > time_delta_threshold:
            current_media_info = asyncio.run(get_media_info())
            last_fired_time = current_time
            if current_media_info and current_media_info["artist"] != "":
                song = current_media_info["title"]
                artist = current_media_info["artist"]
                artist = "[ " + artist + " ]"
            elif current_media_info:
                song, artist = get_title_song(current_media_info["title"])
                artist = "[ " + artist + " ]"
            else:
                # fallback to window title info
                song = ""
                if " - " in window_title:
                    artist, song = window_title.split(" - ", 1)
                else:
                    artist, song = window_title, ""
                artist = "[ " + artist + " ]"
                
        # Send formatted info to Arduino
        serial_output = f"{song.strip()}||{artist.strip()}||{current_volume}\n"
        ser.write(serial_output.encode('utf-8'))
        time.sleep(0.10)

    except Exception as e:
        print("Error:", e)
        time.sleep(0.10)
