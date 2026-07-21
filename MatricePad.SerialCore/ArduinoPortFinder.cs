using System.Management;
using System.Text.RegularExpressions;

namespace MatricePad.SerialCore;

/// <summary>A board matched by VID:PID, before anything has opened it.</summary>
public sealed record ArduinoCandidate(string PortName, string FriendlyName);

/// <summary>
/// Finds Arduino-class boards attached over USB by VID:PID, via WMI. Shared
/// between MatricePadApp (which just wants the first match) and the firmware
/// updater (which must never guess when more than one board is present --
/// flashing the wrong one is a much worse mistake than a display client
/// picking the wrong port).
/// </summary>
public static partial class ArduinoPortFinder
{
    /// <summary>
    /// Returns every currently-attached COM port whose PNP device ID matches
    /// one of the "VID:PID" strings in <paramref name="vidPidList"/> (hex,
    /// case-insensitive, e.g. "2341:8036").
    /// </summary>
    public static IReadOnlyList<ArduinoCandidate> FindCandidates(IEnumerable<string> vidPidList)
    {
        var candidates = new List<ArduinoCandidate>();
        var vidPids = vidPidList as ICollection<string> ?? vidPidList.ToList();

        try
        {
            using var searcher = new ManagementObjectSearcher(
                "SELECT * FROM Win32_PnPEntity WHERE Caption LIKE '%(COM%'");
            foreach (ManagementBaseObject device in searcher.Get())
            {
                var pnpId = device["PNPDeviceID"]?.ToString() ?? "";
                var caption = device["Caption"]?.ToString() ?? "";

                foreach (var vidPid in vidPids)
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
                        candidates.Add(new ArduinoCandidate(m.Groups[1].Value, caption));
                    }

                    break;
                }
            }
        }
        catch (Exception)
        {
            // WMI query failed for any reason -- treat as "nothing found" rather
            // than throwing; callers already handle an empty candidate list.
        }

        return candidates;
    }

    [GeneratedRegex(@"\((COM\d+)\)")]
    private static partial Regex ComPortInCaption();
}
