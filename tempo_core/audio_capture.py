import threading
import warnings

import numpy as np
import soundcard as sc

from .debug import debugPrint

# Benign, frequent on some WASAPI loopback devices -- an occasional dropped
# frame is imperceptible in a bar-graph visualizer, not worth surfacing.
warnings.filterwarnings("ignore", category=sc.SoundcardRuntimeWarning)

SAMPLE_RATE  = 44100
CHUNK        = 1024
NUM_BARS     = 16
BAND_MIN_HZ  = 60.0
BAND_MAX_HZ  = 16000.0
DB_FLOOR     = -60.0
DB_CEIL      = 0.0
DECAY_STEP   = 8   # per-frame level decay (0-100 units) so bars fall smoothly instead of flickering

_window     = np.hanning(CHUNK)
_window_sum = np.sum(_window)   # normalizes rfft magnitude back to ~0-1 scale (window attenuates energy)
_freqs      = np.fft.rfftfreq(CHUNK, d=1.0 / SAMPLE_RATE)
_band_edges = np.logspace(np.log10(BAND_MIN_HZ), np.log10(BAND_MAX_HZ), NUM_BARS + 1)

_levels_lock = threading.Lock()
_levels      = [0] * NUM_BARS


def get_bar_levels():
    """Returns a copy of the latest NUM_BARS levels (each 0-100)."""
    with _levels_lock:
        return list(_levels)


def _bin_to_levels(fft_mag):
    """Aggregates an rfft magnitude array into NUM_BARS log-spaced 0-100 levels."""
    levels = [0] * NUM_BARS
    for i in range(NUM_BARS):
        lo, hi = _band_edges[i], _band_edges[i + 1]
        if i == NUM_BARS - 1:
            mask = (_freqs >= lo) & (_freqs <= hi)
        else:
            mask = (_freqs >= lo) & (_freqs < hi)
        band_mag = fft_mag[mask].max() if mask.any() else 0.0
        db = 20.0 * np.log10(band_mag + 1e-9)
        db = max(DB_FLOOR, min(DB_CEIL, db))
        levels[i] = int(round((db - DB_FLOOR) / (DB_CEIL - DB_FLOOR) * 100))
    return levels


def _capture_loop(stop_event):
    try:
        speaker = sc.default_speaker()
        mic = sc.get_microphone(id=str(speaker.name), include_loopback=True)
    except Exception as e:
        print(f"[Audio Capture Error] Could not open loopback device: {e}")
        return

    try:
        with mic.recorder(samplerate=SAMPLE_RATE, blocksize=CHUNK) as recorder:
            while not stop_event.is_set():
                try:
                    data = recorder.record(numframes=CHUNK)
                    mono = data.mean(axis=1) if data.ndim > 1 else data
                    windowed = mono * _window
                    fft_mag = np.abs(np.fft.rfft(windowed)) * 2.0 / _window_sum
                    instant_levels = _bin_to_levels(fft_mag)

                    with _levels_lock:
                        for i in range(NUM_BARS):
                            _levels[i] = max(instant_levels[i], _levels[i] - DECAY_STEP)
                except Exception as e:
                    debugPrint(f"[Audio Capture] frame error: {e}")
    except Exception as e:
        print(f"[Audio Capture Error] {e}")


def start_capture_thread(stop_event):
    """Starts the background WASAPI loopback capture + FFT thread. Read results
    via get_bar_levels()."""
    thread = threading.Thread(target=_capture_loop, args=(stop_event,), daemon=True)
    thread.start()
    return thread
