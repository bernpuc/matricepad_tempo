using System.IO.Ports;
using System.Management;
using System.Text.RegularExpressions;
using MatricePadApp.Services.Interfaces;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;

namespace MatricePadApp.Services;

public partial class SerialManager : ISerialManager, IDisposable
{
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
                    var port = new SerialPort(portName, _options.BaudRate) { NewLine = "\n", ReadTimeout = 500 };
                    port.Open();
                    _port = port;
                    _logger.LogInformation("Serial connection established on {Port}", portName);
                    Thread.Sleep(2000);
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

    private string? FindArduinoPort()
    {
        try
        {
            using var searcher = new ManagementObjectSearcher(
                "SELECT * FROM Win32_PnPEntity WHERE Caption LIKE '%(COM%'");
            foreach (ManagementBaseObject device in searcher.Get())
            {
                var pnpId = device["PNPDeviceID"]?.ToString() ?? "";
                var caption = device["Caption"]?.ToString() ?? "";
                foreach (var vidPid in _options.ArduinoVidPidList)
                {
                    var parts = vidPid.Split(':');
                    if (parts.Length != 2)
                    {
                        continue;
                    }

                    if (!pnpId.Contains($"VID_{parts[0]}", StringComparison.OrdinalIgnoreCase) ||
                        !pnpId.Contains($"PID_{parts[1]}", StringComparison.OrdinalIgnoreCase))
                    {
                        continue;
                    }

                    var m = ComPortInCaption().Match(caption);
                    if (m.Success)
                    {
                        return m.Groups[1].Value;
                    }
                }
            }
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex, "WMI COM port lookup failed");
        }

        return null;
    }

    [GeneratedRegex(@"\((COM\d+)\)")]
    private static partial Regex ComPortInCaption();

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
                catch (Exception ex) when (ex is IOException or InvalidOperationException or UnauthorizedAccessException)
                {
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
