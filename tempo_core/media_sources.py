import asyncio
import datetime
import re
import threading
import time
import unicodedata

from pycaw.pycaw import AudioUtilities
from pycaw.constants import AudioSessionState
from winrt.windows.media.control import \
    GlobalSystemMediaTransportControlsSessionManager as MediaManager
import win32gui  # pylint: disable=no-member
import win32process  # pylint: disable=no-member

from .debug import debugPrint

_UNICODE_REPLACEMENTS = str.maketrans({
    '‘': "'", '’': "'", '‚': "'", '‛': "'",  # single quotes
    '“': '"', '”': '"', '„': '"', '‟': '"',  # double quotes
    '–': '-', '—': '-', '―': '-',                 # dashes
    '…': '...', '•': '*', '·': '.',               # ellipsis, bullets
})


def to_ascii(text: str) -> str:
    text = text.translate(_UNICODE_REPLACEMENTS)
    text = unicodedata.normalize('NFKD', text)
    return text.encode('ascii', errors='ignore').decode('ascii')


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
        debugPrint(f"[youtube] Topic channel -- WinRT fields used directly")
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
_NOISE_TITLES  = {"libvlcsharp.wpf", "libvlcsharp", "vlcsharp"}

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
                            if title and not title.isspace() and title.lower() not in _NOISE_TITLES:
                                titles.append(title)
                    return True

                win32gui.EnumWindows(enum_handler, titles)  # pylint: disable=no-member
                if titles:
                    return max(titles, key=len)

    return "No media playing"


def get_window_title():
    global _window_title_cache, _window_title_next
    now = time.monotonic()
    if now >= _window_title_next:
        _window_title_cache = get_audio_playing_window_title()
        _window_title_next  = now + _WINDOW_TITLE_INTERVAL
    return _window_title_cache


PLAYBACK_STATUS_PLAYING = 4   # GlobalSystemMediaTransportControlsSessionPlaybackStatus
PLAYBACK_STATUS_PAUSED  = 5


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
            if status == PLAYBACK_STATUS_PLAYING:
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

        position_sec = 0
        duration_sec = 0
        try:
            timeline = chosen.get_timeline_properties()
            position = timeline.position
            # Chrome (and most browsers) only calls setPositionState() on
            # discrete events (seek, play, pause) -- not every frame -- so
            # `position` is a snapshot as of `last_updated_time`, not a live
            # ticking value. Extrapolate forward by wall-clock time elapsed
            # since that snapshot while actually playing, same as Windows'
            # own Now Playing widget does.
            if chosen_status == PLAYBACK_STATUS_PLAYING:
                elapsed_since_update = (
                    datetime.datetime.now(datetime.timezone.utc) - timeline.last_updated_time
                ).total_seconds()
                position += datetime.timedelta(seconds=max(0.0, elapsed_since_update))
            position_sec = int(position.total_seconds())
            duration_sec = int(timeline.end_time.total_seconds())
        except Exception as e:
            debugPrint(f"[WinRT timeline] error: {e}")

        # genres, thumbnail, playback_type raise Traceback — skip them
        info_dict = {
            "artist":          info.artist,
            "album_artist":    info.album_artist,
            "title":           info.title,
            "album_title":     info.album_title,
            "track_number":    info.track_number,
            "playback_status": chosen_status,
            "position_sec":    position_sec,
            "duration_sec":    duration_sec,
        }
        debugPrint(f"[WinRT raw] title='{info.title}' artist='{info.artist}'"
                   f" album_artist='{info.album_artist}' album='{info.album_title}'"
                   f" track={info.track_number} status={chosen_status}")
        return info_dict


_media_info_lock         = threading.Lock()
_shared_media_info        = None
_last_playing_media_info  = None


def get_shared_media_info():
    """Returns a copy of the latest WinRT media_info dict, or None."""
    with _media_info_lock:
        return _shared_media_info.copy() if _shared_media_info is not None else None


def get_last_playing_media_info():
    """Returns a copy of the last media_info dict seen while actually playing, or None."""
    with _media_info_lock:
        return _last_playing_media_info.copy() if _last_playing_media_info is not None else None


def _media_info_loop(stop_event):
    global _shared_media_info, _last_playing_media_info
    none_count = 0
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    while not stop_event.is_set():
        try:
            media_info = loop.run_until_complete(get_media_info_async())
            if media_info is not None:
                none_count = 0
                with _media_info_lock:
                    _shared_media_info = media_info
                    if media_info.get("playback_status") == PLAYBACK_STATUS_PLAYING:
                        _last_playing_media_info = media_info
                        debugPrint(f"[WinRT] updated last_playing: '{media_info['title']}'")
            else:
                none_count += 1
                if none_count >= 3:  # ~9s of no session — real app switch, clear stale data
                    none_count = 0
                    with _media_info_lock:
                        _shared_media_info = None
                        _last_playing_media_info = None
                    debugPrint("[WinRT] session gone -- cleared shared/last_playing media_info")
        except Exception as e:
            print(f"[Media Info Thread Error] {e}")
        time.sleep(3)


def start_media_info_thread(stop_event):
    """Starts the background WinRT polling thread. Read results via
    get_shared_media_info() / get_last_playing_media_info()."""
    thread = threading.Thread(target=_media_info_loop, args=(stop_event,), daemon=True)
    thread.start()
    return thread
