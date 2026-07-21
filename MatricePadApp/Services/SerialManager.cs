using System.IO.Ports;
using MatricePad.SerialCore;
using MatricePadApp.Services.Interfaces;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;

namespace MatricePadApp.Services;

public class SerialManager : ISerialManager, IDisposable
{
    // Mirrors PROTOCOL_VERSION in matrice_pad_tempo.ino -- bump both together
    // whenever SerialPacket's wire format (field count/order) changes.
    private const int ExpectedProtocolVersion = 1;

    private readonly MatricePadOptions _options;
    private readonly ILogger<SerialManager> _logger;
    private readonly Lock _lock = new();
    private SerialPort? _port;
    private Thread? _readThread;
    private CancellationTokenSource? _readCts;

    public bool IsConnected
    {
        get
        {
            lock (_lock)
            {
                return _port is { IsOpen: true };
            }
        }
    }

    public event EventHandler<string>? LineReceived;

    public SerialManager(IOptions<MatricePadOptions> options, ILogger<SerialManager> logger)
    {
        _options = options.Value;
        _logger = logger;
    }

    public void Send(string data)
    {
        lock (_lock)
        {
            if (_port is not { IsOpen: true } && !TryConnect())
            {
                return;
            }

            try
            {
                var bytes = System.Text.Encoding.ASCII.GetBytes(data);
                _port!.Write(bytes, 0, bytes.Length);
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidOperationException or TimeoutException)
            {
                _logger.LogWarning(ex, "Serial write failed, marking disconnected");
                ClosePort();
            }
        }
    }

    // Caller must hold _lock.
    private bool TryConnect()
    {
        for (var attempt = 1; attempt <= 5; attempt++)
        {
            var portName = _options.ComPort ?? FindArduinoPort();
            if (portName is null)
            {
                _logger.LogWarning("Arduino port not found (attempt {Attempt}/5)", attempt);
            }
            else
            {
                try
                {
                    var port = ArduinoSerial.Open(portName, _options.BaudRate);
                    _port = port;
                    _logger.LogInformation("Serial connection established on {Port}", portName);
                    // Opening the port resets the board, and its own setup() has a
                    // ~2s startup delay before it's reading serial at all -- wait
                    // that out before attempting the handshake below, or the
                    // VERSION? request just gets missed while it's still booting.
                    Thread.Sleep(2000);
                    PerformVersionHandshake(port);
                    StartReadLoop();
                    return true;
                }
                catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException)
                {
                    _logger.LogWarning(ex, "Failed to open {Port} (attempt {Attempt}/5)", portName, attempt);
                }
            }

            if (attempt < 5)
            {
                Thread.Sleep(_options.ReconnectDelayMs);
            }
        }

        return false;
    }

    // Caller must hold _lock and have just opened `port`. Best-effort: any
    // failure here just means we don't get a version log line, it never
    // blocks the connection.
    private void PerformVersionHandshake(SerialPort port)
    {
        var result = FirmwareHandshake.Perform(port);
        switch (result.Outcome)
        {
            case HandshakeOutcome.Success when result.ProtocolVersion == ExpectedProtocolVersion:
                _logger.LogInformation(
                    "Firmware handshake OK: protocol {Protocol}, firmware {Firmware}", result.ProtocolVersion, result.FirmwareVersion);
                break;
            case HandshakeOutcome.Success:
                _logger.LogWarning(
                    "Firmware protocol version mismatch: companion expects {Expected}, board reports {Actual} (firmware {Firmware})",
                    ExpectedProtocolVersion, result.ProtocolVersion, result.FirmwareVersion);
                break;
            case HandshakeOutcome.NoResponse:
                _logger.LogInformation("No handshake response from firmware (older firmware without version support?)");
                break;
            case HandshakeOutcome.UnrecognizedResponse:
                _logger.LogWarning("Unrecognized firmware handshake response: {Line}", result.RawResponse);
                break;
            case HandshakeOutcome.Error:
                _logger.LogWarning(result.Error, "Firmware handshake failed");
                break;
        }
    }

    private string? FindArduinoPort() =>
        ArduinoPortFinder.FindCandidates(_options.ArduinoVidPidList).FirstOrDefault()?.PortName;

    private void StartReadLoop()
    {
        _readCts = new CancellationTokenSource();
        var token = _readCts.Token;
        var port = _port!;
        _readThread = new Thread(() =>
        {
            while (!token.IsCancellationRequested && port.IsOpen)
            {
                try
                {
                    var line = port.ReadLine();
                    if (!string.IsNullOrEmpty(line))
                    {
                        _logger.LogDebug("[Panel] {Line}", line);
                        LineReceived?.Invoke(this, line);
                    }
                }
                catch (TimeoutException)
                {
                    // No data within ReadTimeout — expected, keep polling.
                }
                catch (Exception ex)
                {
                    // Catch everything, not just the expected IO/UnauthorizedAccess
                    // types -- a concurrent Send() failure can Close() this exact
                    // port out from under a blocked ReadLine() call on another
                    // thread, which throws ObjectDisposedException. Any exception
                    // escaping this background thread crashes the whole process,
                    // so nothing here can be allowed to go uncaught.
                    _logger.LogWarning(ex, "Serial read failed, closing port so reconnect can open a fresh handle");
                    lock (_lock)
                    {
                        // Only close if we're still the active port -- a concurrent
                        // Send() failure may have already closed and reconnected.
                        if (_port == port)
                        {
                            ClosePort();
                        }
                    }
                    break;
                }
            }
        })
        { IsBackground = true };
        _readThread.Start();
    }

    // Caller must hold _lock.
    private void ClosePort()
    {
        _readCts?.Cancel();
        try
        {
            _port?.Close();
        }
        catch
        {
            // Best-effort close.
        }

        _port = null;
    }

    public void Dispose()
    {
        lock (_lock)
        {
            ClosePort();
        }

        GC.SuppressFinalize(this);
    }
}
