using System.IO.Ports;

namespace MatricePad.SerialCore;

/// <summary>
/// Opens a SerialPort the way an Arduino-class native-USB board (Leonardo,
/// Pro Micro) actually needs. DtrEnable/RtsEnable default to false in .NET's
/// SerialPort, but the board's CDC stack can leave incoming data
/// unacknowledged until DTR is actively asserted -- observed directly:
/// the port opens without error, yet the board never receives a single
/// byte, depending on whatever DTR state happened to be left over from a
/// prior session. Must be set explicitly on every open.
/// </summary>
public static class ArduinoSerial
{
    public static SerialPort Open(string portName, int baudRate, int readTimeoutMs = 500)
    {
        var port = new SerialPort(portName, baudRate) { NewLine = "\n", ReadTimeout = readTimeoutMs };
        port.DtrEnable = true;
        port.RtsEnable = true;
        port.Open();
        return port;
    }
}
