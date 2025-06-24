'''
This version uses threading to isolate the WinRT fetching to a separate thread with a
different rate.
WinRT library returns song and artist info when playing audio from a streaming service 
within a browser window.
Audio players that display song and artist info in the window title bar are supported
using the pycaw library.
'''
import argparse
import time
import serial
import asyncio
from winrt.windows.media.control import \
    GlobalSystemMediaTransportControlsSessionManager as MediaManager
from comtypes import CLSCTX_ALL
from pycaw.pycaw import AudioUtilities, IAudioEndpointVolume, IAudioSessionManager2
from pycaw.constants import AudioSessionState
import win32gui
import win32process
import re
import datetime
import threading
DEBUG = True    # Set to True for debugging output
def debugPrint(*args, **kwargs):
    if DEBUG:
        print(*args, **kwargs)

# Microsoft Zune. title field only. Formatted with '<track number> <artist> - <song title>'
# But
# VLC Player is Formatted '<track number> <artist> - <song title> - VLC media player'
def get_title_song(input_string):
    # List of (regex, group count or a custom parser)
    regex_patterns = [
        (r"^(\d+)\s(.*)\s-\s(.*)\s-\s.*$", lambda m: {'track': m.group(1), 'artist': m.group(2), 'song': m.group(3)}),
        (r"^(\d+)\s(.*)\s-\s(.*)$",        lambda m: {'track': m.group(1), 'artist': m.group(2), 'song': m.group(3)}),
        (r"^(.*)\s-\s(.*)$",               lambda m: {'artist': m.group(1), 'song': m.group(2)})
    ]

    parsed = None
    for pattern, parser in regex_patterns:
        match = re.search(pattern, input_string)
        if match:
            parsed = parser(match)
            break

    if parsed:
        debugPrint("Parsed data:", parsed)
        return parsed['song'], parsed['artist']
    else:
        debugPrint("No match found.")
        return "", ""

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

async def get_media_info_async():
    sessions = await MediaManager.request_async()       # Sometimes this never returns

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

def handle_serial_input(ser, volume_i):
    if ser.in_waiting: 
        line = ser.readline().decode('utf-8').strip()

        if line.startswith("VOL:"):
            try:
                new_volume = int(line[4:])
                new_volume = max(0, min(100, new_volume))  # clamp to 0–100
                volume_i.SetMasterVolumeLevelScalar(new_volume / 100.0, None)
                debugPrint(f"[Arduino -> PC] Volume set to {new_volume}%")
            except ValueError:
                print("[Error] Invalid volume format:", line)
    return

def get_audio_settings(volume_i):
    current_volume = int(volume_i.GetMasterVolumeLevelScalar() * 100)
    return current_volume

def get_window_title():
    window_title = get_audio_playing_window_title()
    return window_title

def get_media_info_loop(stop_event):
    global shared_media_info
    while not stop_event.is_set():
        try:
            media_info = asyncio.run(get_media_info_async())
            with media_info_lock:
                shared_media_info = media_info
        except Exception as e:
            print(f"[Media Info Thread Error] {e}")
        time.sleep(3)

def get_serial_packet(window_title, media_info, current_volume):
    song = ""
    artist = ""
    if len(window_title) > 0:
        debugPrint(f"window: {window_title}")
        song, artist = get_title_song(window_title)
        if artist == "": artist = window_title

    # TODO: Determine which has the best data
    if media_info and len(media_info["artist"]) > 0 and len(media_info["title"]) > 0:
        artist = media_info["artist"]
        song = media_info["title"]
        debugPrint(f"media: {artist} {song}")

    artist = "[ " + artist + " ]"
    serial_output = f"{song.strip()}||{artist.strip()}||{current_volume}\n"
    debugPrint(serial_output)
    return serial_output

def send_packet(ser, serial_packet: str):
    ser.write(serial_packet.encode('utf-8'))
    return

def main(port: str | None):
    try:
        # Set up serial communication
        ser = serial.Serial(port, 115200, timeout=1)
        time.sleep(2)
        # Set up audio device and volume interface
        devices = AudioUtilities.GetSpeakers()
        interface = devices.Activate(IAudioEndpointVolume._iid_, CLSCTX_ALL, None)
        volume_i = interface.QueryInterface(IAudioEndpointVolume)
        # Start background thread to update media info
        stop_event = threading.Event()
        media_thread = threading.Thread(target=get_media_info_loop, args=(stop_event,), daemon=True)
        media_thread.start()

        while True:
            # Check incoming serial
            handle_serial_input(ser, volume_i)
            # Get audio settings
            current_volume = get_audio_settings(volume_i)
            # Get window_title
            window_title = get_window_title()
            # Get media_info
            with media_info_lock:
                media_info = shared_media_info.copy()
            # Assemble serial packet
            serial_packet = get_serial_packet(window_title, media_info, current_volume)
            # Send serial packet
            send_packet(ser, serial_packet)
            time.sleep(.1)

    except KeyboardInterrupt:
        debugPrint("\nShutting down gracefully.")
        stop_event.set()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="A script that runs forever.")
    parser.add_argument("--port", type=str, help="Comm Port, default is com4", default="com4")
    args = parser.parse_args()

    media_info_lock = threading.Lock()
    shared_media_info = {}
    main(args.port)