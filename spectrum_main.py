'''
Real-time frequency bar graph display. Captures WASAPI loopback audio,
computes a 16-band log-spaced FFT spectrum, and streams bar levels (0-100
each) to the Arduino as a plain CSV line, e.g. "3,5,12,20,...\n".

This is a standalone entrypoint (not template.py's media-info flow) -- see
CLAUDE.md for the TempoCore/tempo_core shared-library split this builds on.
'''
import argparse
import threading
import time

from serial.serialutil import SerialException

import tempo_core.debug as debug
from tempo_core import audio_capture, serial_link
from tempo_core.debug import debugPrint

SEND_INTERVAL_S = 0.05  # ~20 fps


def build_frame(levels):
    return ",".join(str(v) for v in levels) + "\n"


def main(port: str | None):
    global_ser = None
    stop_event = threading.Event()
    audio_capture.start_capture_thread(stop_event)

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
                frame = build_frame(levels)
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
