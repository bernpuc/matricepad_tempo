'''
This version uses threading to isolate the WinRT fetching to a separate thread with a
different rate.
WinRT library returns song and artist info when playing audio from a streaming service
within a browser window.
Audio players that display song and artist info in the window title bar are supported
using the pycaw library.

Also captures WASAPI loopback audio for the frequency bar graph shown on the Arduino's
BARS view (toggled by the encoder button) -- see CLAUDE.md for the serial protocol.
'''
import argparse
import threading
import time

from pycaw.pycaw import AudioUtilities
from serial.serialutil import SerialException

import tempo_core.debug as debug
from tempo_core import audio_capture, audio_state, media_sources, serial_link
from tempo_core.debug import debugPrint


def handle_serial_input(ser):
    while ser.in_waiting:
        line = ser.readline().decode('utf-8').strip()
        if line:
            print(f"[Arduino] {line}")


def get_serial_packet(window_title, media_info, last_playing):
    """Returns sanitized (song, artist, paused) for the given media source state."""
    song = ""
    artist = ""
    paused = 0
    if window_title == "":
        # Browser is the active audio source — use WinRT metadata
        if media_info and len(media_info["title"]) > 0:
            winrt_artist = media_info.get("artist", "")
            artist, song = media_sources.parse_youtube_title(media_info["title"], winrt_artist)
            debugPrint(f"[parse] title='{media_info['title']}' winrt_artist='{winrt_artist}'"
                       f" -> artist='{artist}' song='{song}'")
        if media_info and media_info.get("playback_status") == media_sources.PLAYBACK_STATUS_PAUSED:
            paused = 1
    elif window_title == "No media playing":
        # No active audio session — show the most recently playing track if available
        if last_playing and last_playing["title"]:
            winrt_artist = last_playing.get("artist", "")
            artist, song = media_sources.parse_youtube_title(last_playing["title"], winrt_artist)
            debugPrint(f"[paused] title='{last_playing['title']}' winrt_artist='{winrt_artist}'"
                       f" -> artist='{artist}' song='{song}'")
            paused = 1
    else:
        # Non-browser app is playing — use its window title
        debugPrint(f"window: {window_title}")
        song, artist = media_sources.get_title_song(window_title)
        if artist == "": artist = window_title

    return media_sources.to_ascii(song.strip()), media_sources.to_ascii(artist.strip()), paused


def build_frame(song, artist, volume, is_muted, paused, bar_levels, elapsed_sec, duration_sec):
    bars_csv = ",".join(str(v) for v in bar_levels)
    return f"{song}||{artist}||{volume}||{1 if is_muted else 0}||{paused}||{bars_csv}||{elapsed_sec}||{duration_sec}\n"


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
    # Start background threads for media info and audio capture. Staggered on
    # purpose: each does its own first-time COM initialization (WinRT here,
    # soundcard/WASAPI in audio_capture), and starting them at the same
    # instant is a real crash (observed SIGSEGV) -- letting one finish its
    # init before the other starts avoids the race.
    stop_event = threading.Event()
    media_sources.start_media_info_thread(stop_event)
    time.sleep(1)
    audio_capture.start_capture_thread(stop_event)

    try:
        while True:
            if global_ser is None or not global_ser.is_open:
                print("Serial port not connected or closed. Attempting to reconnect...")
                global_ser = serial_link.establish_serial_connection(port, 115200)
                if global_ser is None:
                    print("Waiting before next reconnection attempt...")
                    time.sleep(5)  # Wait longer before retrying connection
                    continue # Skip the rest of the loop and try to reconnect again
                last_packet    = ""   # force resend after reconnect
                last_send_time = 0.0
            try:
                # Check incoming serial
                handle_serial_input(global_ser)
                # Get audio settings
                current_volume, is_muted, _app_volume_unused = audio_state.get_audio_settings(volume_i)
                # Get window_title
                window_title = media_sources.get_window_title()
                # Get media_info
                media_info = media_sources.get_shared_media_info()
                last_playing = media_sources.get_last_playing_media_info()
                # Get bar levels and elapsed/duration for the BARS view
                bar_levels = audio_capture.get_bar_levels()
                elapsed_sec  = media_sources.get_smoothed_elapsed(media_info)
                duration_sec = media_info.get("duration_sec", 0) if media_info else 0
                # Assemble serial packet
                song, artist, paused = get_serial_packet(window_title, media_info, last_playing)
                serial_packet = build_frame(song, artist, current_volume, is_muted, paused,
                                             bar_levels, elapsed_sec, duration_sec)
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

            time.sleep(0.05)

    except KeyboardInterrupt:
        debugPrint("\nShutting down gracefully.")
        stop_event.set()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="A script that runs forever.")
    parser.add_argument("--port", type=str, help="COM port (auto-detected if omitted)", default=None)
    parser.add_argument("--debug", action="store_true", help="Enable debug output")
    args = parser.parse_args()
    debug.DEBUG = args.debug

    main(serial_link.find_arduino_port(args.port))
