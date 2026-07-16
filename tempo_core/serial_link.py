import time

import serial
from serial.tools import list_ports
from serial.serialutil import SerialException

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
