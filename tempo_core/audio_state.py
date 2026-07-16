import time

from pycaw.pycaw import AudioUtilities
from pycaw.constants import AudioSessionState

_volume_cache     = 0
_mute_cache       = False
_app_volume_cache = 0
_volume_next      = 0.0
_app_volume_next  = 0.0
_VOLUME_INTERVAL  = 2.0


def get_active_audio_session():
    """Returns the pycaw session for the active audio process, or None."""
    for session in AudioUtilities.GetAllSessions():
        if session.State == AudioSessionState.Active and session.Process:
            return session
    return None


def get_app_volume():
    global _app_volume_cache, _app_volume_next
    now = time.monotonic()
    if now >= _app_volume_next:
        session = get_active_audio_session()
        _app_volume_cache = int(session.SimpleAudioVolume.GetMasterVolume() * 100) if session else 0
        _app_volume_next  = now + _VOLUME_INTERVAL
    return _app_volume_cache


def force_app_volume_refresh():
    """Invalidates the app-volume cache so the next get_app_volume() re-reads it."""
    global _app_volume_next
    _app_volume_next = 0.0


def get_audio_settings(volume_i):
    global _volume_cache, _mute_cache, _volume_next
    now = time.monotonic()
    if now >= _volume_next:
        _volume_cache = int(volume_i.GetMasterVolumeLevelScalar() * 100)
        _mute_cache   = bool(volume_i.GetMute())
        _volume_next  = now + _VOLUME_INTERVAL
    return _volume_cache, _mute_cache, get_app_volume()
