using System.IO.Ports;

namespace MatricePad.SerialCore;

public enum HandshakeOutcome
{
    /// <summary>Board responded with a well-formed PONG||protocol||firmware line.</summary>
    Success,

    /// <summary>No response within the timeout -- either no board, or firmware predating this feature.</summary>
    NoResponse,

    /// <summary>The port itself failed (IO/UnauthorizedAccess/InvalidOperation) while attempting the exchange.</summary>
    Error,

    /// <summary>Something responded, but not in the expected "PONG||<int>||<string>" shape.</summary>
    UnrecognizedResponse,
}

public sealed record FirmwareHandshakeResult(
    HandshakeOutcome Outcome,
    int? ProtocolVersion = null,
    string? FirmwareVersion = null,
    string? RawResponse = null,
    Exception? Error = null);

/// <summary>
/// The VERSION?/PONG exchange described in CLAUDE.md's Serial Protocol
/// section: send a bare "VERSION?" line, expect "PONG||&lt;protocolVersion&gt;||
/// &lt;firmwareVersion&gt;" back. Firmware predating this feature just ignores
/// "VERSION?" like any other malformed line (it has no "||"), so a
/// NoResponse outcome is an expected, non-fatal case, not just an error.
/// </summary>
public static class FirmwareHandshake
{
    /// <summary>
    /// Caller must have just opened <paramref name="port"/> and waited out the
    /// board's own boot delay (opening the port resets it) -- otherwise the
    /// request goes out while the board is still mid-boot and is missed.
    /// Temporarily overrides and restores the port's ReadTimeout.
    /// </summary>
    public static FirmwareHandshakeResult Perform(SerialPort port, int timeoutMs = 1500)
    {
        var originalTimeout = port.ReadTimeout;
        try
        {
            port.DiscardInBuffer();
            port.ReadTimeout = timeoutMs;

            var bytes = System.Text.Encoding.ASCII.GetBytes("VERSION?\n");
            port.Write(bytes, 0, bytes.Length);

            var line = port.ReadLine();
            var parts = line.Split("||");
            if (parts.Length == 3 && parts[0] == "PONG" && int.TryParse(parts[1], out var protocolVersion))
            {
                return new FirmwareHandshakeResult(HandshakeOutcome.Success, protocolVersion, parts[2]);
            }

            return new FirmwareHandshakeResult(HandshakeOutcome.UnrecognizedResponse, RawResponse: line);
        }
        catch (TimeoutException)
        {
            return new FirmwareHandshakeResult(HandshakeOutcome.NoResponse);
        }
        catch (Exception ex) when (ex is IOException or InvalidOperationException or UnauthorizedAccessException)
        {
            return new FirmwareHandshakeResult(HandshakeOutcome.Error, Error: ex);
        }
        finally
        {
            port.ReadTimeout = originalTimeout;
        }
    }
}
