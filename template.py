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
from serial.tools import list_ports
import asyncio
from winrt.windows.media.control import \
    GlobalSystemMediaTransportControlsSessionManager as MediaManager
from pycaw.pycaw import AudioUtilities
from pycaw.constants import AudioSessionState
from serial.serialutil import SerialException
import win32gui
import win32process
import re
import threading
ARDUINO_VID_PID = [("1B4F", "9206"), ("1B4F", "9205"), ("2341", "8036")]

def find_arduino_port(override=None):
    if override:
        return override
    for port in list_ports.comports():
        if port.vid and port.pid:
            vid = f"{port.vid:04X}"
            pid = f"{port.pid:04X}"
            if (vid, pid) in ARDUINO_VID_PID:
                print(f"Auto-detected Arduino on {port.device} ({port.description})")
                return port.device
    raise RuntimeError("Arduino not found. Plug in the board or use --port to specify.")

DEBUG = False    # Set to True for debugging output
def debugPrint(*args, **kwargs):
    if DEBUG:
        print(*args, **kwargs)

def establish_serial_connection(port, baud_rate, timeout=1, retries=5, delay_between_retries=2):
    """
    Attempts to establish a serial connection with retry logic.
    Returns the serial object on success, None on failure.
    """
    for attempt in range(retries):
        print(f"Attempting to connect to {port} (Attempt {attempt + 1}/{retries})...")
        try:
            ser = serial.Serial(port, baud_rate, timeout=timeout)
            time.sleep(2)  # Give Arduino time to reset and initialize
            print(f"Successfully connected to serial port {port}.")
            return ser
        except SerialException as e:
            print(f"Failed to connect to {port}: {e}")
            time.sleep(delay_between_retries)
        except Exception as e:
            print(f"An unexpected error occurred while connecting: {e}")
            time.sleep(delay_between_retries)
    print(f"Failed to establish serial connection to {port} after {retries} attempts.")
    return None

# Microsoft Zune. title field only. Formatted with '<track number> <artist> - <song title>'
# But
# VLC Player is Formatted '<track number> <artist> - <song title> - VLC media player'
def get_title_song(input_string):
    """Get song title from various formats"""
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
    """Get name of web browser or song title"""
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
        else:
            print(line)
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
    """Main"""
    global_ser = None

    # Set up audio device and volume interface
    devices = AudioUtilities.GetSpeakers()
    volume_i = devices.EndpointVolume
    # Start background thread to update media info
    stop_event = threading.Event()
    media_thread = threading.Thread(target=get_media_info_loop, args=(stop_event,), daemon=True)
    media_thread.start()

    try:
        while True:
            if global_ser is None or not global_ser.is_open:
                print("Serial port not connected or closed. Attempting to reconnect...")
                global_ser = establish_serial_connection(port, 115200)
                if global_ser is None:
                    print("Waiting before next reconnection attempt...")
                    time.sleep(5)  # Wait longer before retrying connection
                    continue # Skip the rest of the loop and try to reconnect again
            try:
                # Check incoming serial
                handle_serial_input(global_ser, volume_i)
                # Get audio settings
                current_volume = get_audio_settings(volume_i)
                # Get window_title
                window_title = get_window_title()
                # Get media_info
                media_info = None
                with media_info_lock:
                    if shared_media_info is not None:
                        media_info = shared_media_info.copy()
                # Assemble serial packet
                serial_packet = get_serial_packet(window_title, media_info, current_volume)
                # Send serial packet
                send_packet(global_ser, serial_packet)
                
            except SerialException as e:
                print(f"Serial communication error: {e}. Attempting to reconnect.")
                if global_ser and global_ser.is_open:
                    global_ser.close()
                global_ser = None # Mark as disconnected, next loop iteration will reconnect
                continue # Immediately try to reconnect

            except Exception as e:
                print(f"An unexpected error occurred in main loop: {e}")
                # Consider if this error also warrants closing the serial port
                # For now, it will likely be caught by the next SerialException if it affects serial comms

            time.sleep(0.1)
        
    except KeyboardInterrupt:
        debugPrint("\nShutting down gracefully.")
        stop_event.set()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="A script that runs forever.")
    parser.add_argument("--port", type=str, help="COM port (auto-detected if omitted)", default=None)
    args = parser.parse_args()

    media_info_lock = threading.Lock()
    shared_media_info = {}
    main(find_arduino_port(args.port))