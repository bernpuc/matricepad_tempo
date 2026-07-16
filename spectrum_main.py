'''
Real-time frequency bar graph display. Captures WASAPI loopback audio,
computes a 16-band log-spaced FFT spectrum, and streams bar levels (0-100
each) plus elapsed/duration seconds to the Arduino as a plain CSV line,
e.g. "3,5,12,20,...,145,264\n".

Elapsed/duration comes from WinRT (GlobalSystemMediaTransportControls) only
-- unlike template.py, there's no window-title fallback here, so it's blank
for non-browser players that have no timeline API to query.

This is a standalone entrypoint (not template.py's media-info flow) -- see
CLAUDE.md for the TempoCore/tempo_core shared-library split this builds on.
'''
import argparse
import threading
import time

from serial.serialutil import SerialException

import tempo_core.debug as debug
from tempo_core import audio_capture, media_sources, serial_link
from tempo_core.debug import debugPrint

SEND_INTERVAL_S = 0.05  # ~20 fps

# media_sources only re-polls WinRT every 3s, so position_sec is a stair-step
# that jumps once per poll rather than ticking every frame. Smooth it locally
# by extrapolating forward from the last observed change using wall-clock
# time, capped just past one poll interval so a source that genuinely never
# updates its position (observed with some custom web players) settles to a
# small, bounded offset instead of drifting forever.
_MAX_EXTRAPOLATION_S = 4.0
_last_raw_position_sec = None
_last_change_time = None


def get_smoothed_elapsed(media_info):
    global _last_raw_position_sec, _last_change_time

    if media_info is None:
        _last_raw_position_sec = None
        _last_change_time = None
        return 0

    raw_position = media_info.get("position_sec", 0)
    now = time.monotonic()

    if raw_position != _last_raw_position_sec:
        _last_raw_position_sec = raw_position
        _last_change_time = now

    if media_info.get("playback_status") == media_sources.PLAYBACK_STATUS_PLAYING:
        extrapolated = min(now - _last_change_time, _MAX_EXTRAPOLATION_S)
        return int(_last_raw_position_sec + extrapolated)
    return _last_raw_position_sec


def build_frame(levels, elapsed_sec, duration_sec):
    fields = [str(v) for v in levels] + [str(elapsed_sec), str(duration_sec)]
    return ",".join(fields) + "\n"


def main(port: str | None):
    global_ser = None
    stop_event = threading.Event()
    # Staggered on purpose: each thread does its own first-time COM
    # initialization (soundcard/WASAPI here, WinRT in the media thread), and
    # starting them at the same instant is a real crash (observed SIGSEGV) --
    # letting one finish its init before the other starts avoids the race.
    audio_capture.start_capture_thread(stop_event)
    time.sleep(1)
    media_sources.start_media_info_thread(stop_event)

    try:
        while True:
            if global_ser is None or not global_ser.is_open:
                print("Serial port not connected or closed. Attempting to reconnect...")
                global_ser = serial_link.establish_serial_connection(port, 115200)
                if global_ser is None:
                    print("Waiting before next reconnection attempt...")
                    time.sleep(5)
                    continue

            try:
                levels = audio_capture.get_bar_levels()
                media_info = media_sources.get_shared_media_info()
                elapsed_sec  = get_smoothed_elapsed(media_info)
                duration_sec = media_info.get("duration_sec", 0) if media_info else 0
                frame = build_frame(levels, elapsed_sec, duration_sec)
                debugPrint(frame.strip())
                global_ser.write(frame.encode('ascii'))

            except SerialException as e:
                print(f"Serial communication error: {e}. Attempting to reconnect.")
                if global_ser and global_ser.is_open:
                    global_ser.close()
                global_ser = None
                continue

            except Exception as e:
                print(f"An unexpected error occurred in main loop: {e}")

            time.sleep(SEND_INTERVAL_S)

    except KeyboardInterrupt:
        debugPrint("\nShutting down gracefully.")
        stop_event.set()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Streams a real-time audio spectrum to the Arduino.")
    parser.add_argument("--port", type=str, help="COM port (auto-detected if omitted)", default=None)
    parser.add_argument("--debug", action="store_true", help="Enable debug output")
    args = parser.parse_args()
    debug.DEBUG = args.debug

    main(serial_link.find_arduino_port(args.port))
