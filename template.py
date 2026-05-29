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
import win32gui  # pylint: disable=no-member
import win32process  # pylint: disable=no-member
import re
import threading
import unicodedata

_UNICODE_REPLACEMENTS = str.maketrans({
    '‘': "'", '’': "'", '‚': "'", '‛': "'",  # single quotes
    '“': '"', '”': '"', '„': '"', '‟': '"',  # double quotes
    '–': '-', '—': '-', '―': '-',                 # dashes
    '…': '...', '•': '*', '·': '.',               # ellipsis, bullets
})
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

DEBUG = False
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
    """Parse artist/song from local player window titles (Zune, VLC, Media Player)."""
    regex_patterns = [
        (r"^(\d+)\s(.*)\s-\s(.*)\s-\s.*$", lambda m: {'track': m.group(1), 'artist': m.group(2), 'song': m.group(3)}),
        (r"^(\d+)\s(.*)\s-\s(.*)$",        lambda m: {'track': m.group(1), 'artist': m.group(2), 'song': m.group(3)}),
        (r"^(.*)\s-\s(.*)$",               lambda m: {'artist': m.group(1), 'song': m.group(2)}),
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

# --- YouTube / WinRT title parser ---

_QUALIFIER_PATS = [re.compile(p, re.I) for p in [
    r'\s*\(Official(?:\s+Music)?\s+Video\)',
    r'\s*\[Official(?:\s+(?:Music|HD)\s+)?Video\]',
    r'\s*\(Official(?:\s+Music)?\s+Audio\)',
    r'\s*\(Official\s+Lyric\s+Video\)',
    r'\s*\(Official\s+Visualizer\)',
    r'\s*\(Official\)',
    r'\s*\(Lyric\s+Video\)',
    r'\s*\[4K[^\]]*\]',
    r'\s*\[HD\]',
    r'\s*\(Full\s+Album\)',
    r'\s*\(feat\.[^)]*\)',
    r'\s*\(ft\.[^)]*\)',
    r'\s+ft\.\s+[^()\[\]]+$',
    r'\s*[-–]\s*YouTube(?:\s+Music)?\s*$',
]]

_NON_MUSIC_PAT = re.compile(
    r'\b(lofi|lo-fi|beats\s+to\s+(relax|chill|study)|playlist|compilation'
    r'|mix\b|full\s+album|live\s+stream|24/7|vol\.?\s*\d)\b', re.I
)

def _strip_qualifiers(t):
    for pat in _QUALIFIER_PATS:
        t = pat.sub('', t)
    return t.strip()

def _is_likely_artist(s):
    words = s.split()
    if re.search(r'\b(&|\+|and|feat\.|ft\.)\b', s, re.I):
        return True
    if re.match(r'^The\s+[A-Z]', s) and len(words) <= 4:
        return True
    if len(words) == 1 and s.isupper() and len(s) >= 3:
        return True
    if len(words) <= 3 and all(w[0].isupper() or not w[0].isalpha() for w in words if w):
        return True
    return False

def _is_likely_song_phrase(s):
    STARTERS = r'^(In\s+The|Somebody|Something|Nothing|Everything|Anyone|Everyone|Never|Always|Sometimes|Forever|Tonight|Yesterday|Tomorrow|Beautiful|Wonderful|Crazy|Falling|Running|Standing|What\s+|How\s+)'
    if re.match(STARTERS, s, re.I) and len(s.split()) >= 3:
        return True
    return len(s.split()) >= 5

def parse_youtube_title(raw_title, winrt_artist):
    """Parse artist and song from a YouTube WinRT title + artist field.
    Returns (artist, song)."""
    if _NON_MUSIC_PAT.search(raw_title):
        debugPrint(f"[youtube] non-music stream")
        return winrt_artist, _strip_qualifiers(raw_title)

    t = _strip_qualifiers(raw_title)
    is_vevo = winrt_artist.upper().endswith('VEVO')

    # Topic channel / YouTube Music: no ' - ' in title, artist field is clean
    if ' - ' not in t and winrt_artist and not is_vevo:
        debugPrint(f"[youtube] Topic channel — WinRT fields used directly")
        return winrt_artist, t

    # No delimiter — fall back to WinRT artist
    if ' - ' not in t:
        return winrt_artist, t

    idx = t.index(' - ')
    left = t[:idx].strip()
    right = _strip_qualifiers(t[idx + 3:]).strip()

    # Inversion: "Song Title - Artist Name"
    if not _is_likely_artist(left) and _is_likely_song_phrase(left) and \
            (len(right.split()) <= 3 or _is_likely_artist(right)):
        debugPrint(f"[youtube] inverted title")
        return right, left

    return left, right

_BROWSER_PROCS = {"chrome.exe", "firefox.exe", "msedge.exe", "opera.exe"}

_window_title_cache = "No media playing"
_window_title_next  = 0.0
_WINDOW_TITLE_INTERVAL = 1.0

def get_audio_playing_window_title():
    """Get window title of the active non-browser audio process, or '' for browsers."""

    sessions = AudioUtilities.GetAllSessions()
    for session in sessions:
        if session.State == AudioSessionState.Active:
            process = session.Process
            if process:
                pid = process.pid
                proc_name = process.name().lower()

                # Browser audio — WinRT is the source for browser content, not window title
                if proc_name in _BROWSER_PROCS:
                    return ""

                # Otherwise, get window titles for the process
                titles = []

                def enum_handler(hwnd, titles, pid=pid):
                    if win32gui.IsWindowVisible(hwnd):  # pylint: disable=no-member
                        _, window_pid = win32process.GetWindowThreadProcessId(hwnd)  # pylint: disable=no-member
                        if window_pid == pid:
                            title = win32gui.GetWindowText(hwnd)  # pylint: disable=no-member
                            if title and not title.isspace():
                                titles.append(title)
                    return True

                win32gui.EnumWindows(enum_handler, titles)  # pylint: disable=no-member
                if titles:
                    return max(titles, key=len)

    return "No media playing"

_PLAYBACK_STATUS_PLAYING = 4   # GlobalSystemMediaTransportControlsSessionPlaybackStatus
_PLAYBACK_STATUS_PAUSED  = 5

async def get_media_info_async():
    sessions = await MediaManager.request_async()       # Sometimes this never returns

    # Prefer a session that is actively playing over whatever Windows calls "current".
    # get_current_session() returns the last-focused session, which can be a stale
    # browser session even when another app is actively playing.
    chosen = None
    all_sessions = sessions.get_sessions()
    for s in all_sessions:
        try:
            pb = s.get_playback_info()
            status = pb.playback_status if pb else -1
            debugPrint(f"[WinRT session] {s.source_app_user_model_id}  status={status}")
            if status == _PLAYBACK_STATUS_PLAYING:
                chosen = s
                break
        except Exception as e:
            debugPrint(f"[WinRT session] error: {e}")
    if chosen is None:
        cs = sessions.get_current_session()
        debugPrint(f"[WinRT] no Playing session found; current={cs.source_app_user_model_id if cs else 'none'}")
        chosen = cs

    if chosen:
        pb = chosen.get_playback_info()
        chosen_status = pb.playback_status if pb else -1
        info = await chosen.try_get_media_properties_async()
        # genres, thumbnail, playback_type raise Traceback — skip them
        info_dict = {
            "artist":          info.artist,
            "album_artist":    info.album_artist,
            "title":           info.title,
            "album_title":     info.album_title,
            "track_number":    info.track_number,
            "playback_status": chosen_status,
        }
        debugPrint(f"[WinRT raw] title='{info.title}' artist='{info.artist}'"
                   f" album_artist='{info.album_artist}' album='{info.album_title}'"
                   f" track={info.track_number} status={chosen_status}")
        return info_dict

def handle_serial_input(ser, volume_i):
    global _volume_next
    while ser.in_waiting:
        line = ser.readline().decode('utf-8').strip()

        if line.startswith("VOL:"):
            try:
                new_volume = int(line[4:])
                new_volume = max(0, min(100, new_volume))  # clamp to 0–100
                volume_i.SetMasterVolumeLevelScalar(new_volume / 100.0, None)
                _volume_next = 0.0  # force re-read on next cycle
                debugPrint(f"[Arduino -> PC] Volume set to {new_volume}%")
            except ValueError:
                print("[Error] Invalid volume format:", line)
        else:
            print(line)

_volume_cache    = 0
_volume_next     = 0.0
_VOLUME_INTERVAL = 2.0

def get_audio_settings(volume_i):
    global _volume_cache, _volume_next
    now = time.monotonic()
    if now >= _volume_next:
        _volume_cache = int(volume_i.GetMasterVolumeLevelScalar() * 100)
        _volume_next  = now + _VOLUME_INTERVAL
    return _volume_cache

def get_window_title():
    global _window_title_cache, _window_title_next
    now = time.monotonic()
    if now >= _window_title_next:
        _window_title_cache = get_audio_playing_window_title()
        _window_title_next  = now + _WINDOW_TITLE_INTERVAL
    return _window_title_cache

def get_media_info_loop(stop_event):
    global shared_media_info, last_playing_media_info
    none_count = 0
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    while not stop_event.is_set():
        try:
            media_info = loop.run_until_complete(get_media_info_async())
            if media_info is not None:
                none_count = 0
                with media_info_lock:
                    shared_media_info = media_info
                    if media_info.get("playback_status") == _PLAYBACK_STATUS_PLAYING:
                        last_playing_media_info = media_info
                        debugPrint(f"[WinRT] updated last_playing: '{media_info['title']}'")
            else:
                none_count += 1
                if none_count >= 3:  # ~9s of no session — real app switch, clear stale data
                    none_count = 0
                    with media_info_lock:
                        shared_media_info = None
                        last_playing_media_info = None
                    debugPrint("[WinRT] session gone — cleared shared/last_playing media_info")
        except Exception as e:
            print(f"[Media Info Thread Error] {e}")
        time.sleep(3)

def to_ascii(text: str) -> str:
    text = text.translate(_UNICODE_REPLACEMENTS)
    text = unicodedata.normalize('NFKD', text)
    return text.encode('ascii', errors='ignore').decode('ascii')

def get_serial_packet(window_title, media_info, last_playing, current_volume):
    song = ""
    artist = ""
    if window_title == "":
        # Browser is the active audio source — use WinRT metadata
        if media_info and len(media_info["title"]) > 0:
            winrt_artist = media_info.get("artist", "")
            artist, song = parse_youtube_title(media_info["title"], winrt_artist)
            debugPrint(f"[parse] title='{media_info['title']}' winrt_artist='{winrt_artist}'"
                       f" → artist='{artist}' song='{song}'")
    elif window_title == "No media playing":
        # No active audio session — show the most recently playing track if available
        if last_playing and last_playing["title"]:
            winrt_artist = last_playing.get("artist", "")
            artist, song = parse_youtube_title(last_playing["title"], winrt_artist)
            debugPrint(f"[paused] title='{last_playing['title']}' winrt_artist='{winrt_artist}'"
                       f" → artist='{artist}' song='{song}'")
    else:
        # Non-browser app is playing — use its window title
        debugPrint(f"window: {window_title}")
        song, artist = get_title_song(window_title)
        if artist == "": artist = window_title

    serial_output = f"{to_ascii(song.strip())}||{to_ascii(artist.strip())}||{current_volume}\n"
    debugPrint(serial_output)
    return serial_output

def send_packet(ser, serial_packet: str):
    ser.write(serial_packet.encode('ascii'))
    return

def main(port: str | None):
    """Main"""
    global_ser       = None
    last_packet      = ""
    last_send_time   = 0.0

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
                last_packet    = ""   # force resend after reconnect
                last_send_time = 0.0
            try:
                # Check incoming serial
                handle_serial_input(global_ser, volume_i)
                # Get audio settings
                current_volume = get_audio_settings(volume_i)
                # Get window_title
                window_title = get_window_title()
                # Get media_info
                media_info = None
                last_playing = None
                with media_info_lock:
                    if shared_media_info is not None:
                        media_info = shared_media_info.copy()
                    if last_playing_media_info is not None:
                        last_playing = last_playing_media_info.copy()
                # Assemble serial packet
                serial_packet = get_serial_packet(window_title, media_info, last_playing, current_volume)
                # Send when content changed or keepalive interval elapsed (Arduino timeout = 5s)
                now = time.monotonic()
                if serial_packet != last_packet or now - last_send_time >= 2.5:
                    send_packet(global_ser, serial_packet)
                    last_packet    = serial_packet
                    last_send_time = now
                
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

            time.sleep(0.2)
        
    except KeyboardInterrupt:
        debugPrint("\nShutting down gracefully.")
        stop_event.set()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="A script that runs forever.")
    parser.add_argument("--port", type=str, help="COM port (auto-detected if omitted)", default=None)
    parser.add_argument("--debug", action="store_true", help="Enable debug output")
    args = parser.parse_args()
    DEBUG = args.debug

    media_info_lock = threading.Lock()
    shared_media_info = None
    last_playing_media_info = None
    main(find_arduino_port(args.port))