using System.Diagnostics;
using System.IO;
using System.IO.Ports;
using System.Windows;
using MatricePad.SerialCore;

namespace MatricePadApp.FirmwareUpdater;

public partial class MainWindow : Window
{
    // Mirrors MatricePadApp's appsettings.json default ArduinoVidPidList.
    private static readonly string[] VidPidList = ["1B4F:9206", "1B4F:9205", "2341:8036"];
    private const int BaudRate = 115200;

    // Bootloader mode enumerates under a distinct VID:PID from application
    // mode. boards.txt's leonardo.upload_port.0 (2341:0036) is the official
    // Arduino Leonardo bootloader identity, but this project's actual board is
    // a SparkFun Pro Micro compiled with the Leonardo FQBN (same chip/
    // protocol) -- its bootloader keeps SparkFun's own identity (1B4F:9205)
    // even though the application-mode sketch reports as Arduino Leonardo
    // (2341:8036). Observed directly: assuming only the official Arduino
    // bootloader PID made the discovery loop never find the real board.
    // Check both so this also works on an actual Arduino-brand Leonardo.
    private static readonly string[] BootloaderVidPidList = ["1B4F:9205", "2341:0036"];

    // TODO: read from a bundled manifest once there's more than one firmware
    // build to choose between. For now these mirror matrice_pad_tempo.ino's
    // current PROTOCOL_VERSION/FIRMWARE_VERSION and the bundled .hex's name.
    private const int ExpectedProtocolVersion = 1;
    private const string ExpectedFirmwareVersion = "1.1.0";
    private const string BundledHexFileName = "matrice_pad_tempo-1.1.0.hex";

    private const string CompanionProcessName = "MatricePadApp.exe";
    private const string CompanionTaskName = "MatricePadApp";

    // How long to wait for the bootloader's COM port to reappear after the
    // 1200bps touch, before falling back to the manual double-tap-reset
    // prompt. Matches arduino/build.ps1's own -DiscoveryTimeout default.
    private const int BootloaderDiscoveryTimeoutMs = 20_000;

    private bool _companionStopped;
    private ArduinoCandidate? _lastCandidate;

    public MainWindow()
    {
        InitializeComponent();
    }

    private async void CheckButton_Click(object sender, RoutedEventArgs e)
    {
        CheckButton.IsEnabled = false;
        CandidatePanel.Visibility = Visibility.Collapsed;
        FlashButton.Visibility = Visibility.Collapsed;
        OutputLog.Clear();

        StopCompanion();

        var candidates = await Task.Run(() => ArduinoPortFinder.FindCandidates(VidPidList));

        if (candidates.Count == 0)
        {
            AppendLog("No Matrice Pad board found -- plug it in and try again.");
            RestartCompanion();
            CheckButton.IsEnabled = true;
            return;
        }

        if (candidates.Count == 1)
        {
            var offerUpdate = await CheckCandidateAsync(candidates[0]);
            if (!offerUpdate)
            {
                RestartCompanion();
            }

            CheckButton.IsEnabled = true;
            return;
        }

        AppendLog($"Found {candidates.Count} matching boards -- pick the one to check.");
        CandidateList.ItemsSource = candidates.Select(c => c.FriendlyName).ToList();
        CandidateList.Tag = candidates;
        CandidateList.SelectedIndex = 0;
        CandidatePanel.Visibility = Visibility.Visible;
        // Companion stays stopped until CheckSelectedButton_Click restarts it --
        // the user still needs to pick a board.
        CheckButton.IsEnabled = true;
    }

    private async void CheckSelectedButton_Click(object sender, RoutedEventArgs e)
    {
        if (CandidateList.Tag is not IReadOnlyList<ArduinoCandidate> candidates || CandidateList.SelectedIndex < 0)
        {
            return;
        }

        CheckSelectedButton.IsEnabled = false;
        var candidate = candidates[CandidateList.SelectedIndex];
        var offerUpdate = await CheckCandidateAsync(candidate);
        if (!offerUpdate)
        {
            RestartCompanion();
        }

        CheckSelectedButton.IsEnabled = true;
    }

    /// <summary>Returns true if an update is being offered (FlashButton shown), meaning the
    /// companion should stay stopped until the flash flow finishes rather than being
    /// restarted immediately by the caller.</summary>
    private async Task<bool> CheckCandidateAsync(ArduinoCandidate candidate)
    {
        AppendLog($"Checking {candidate.FriendlyName} on {candidate.PortName}...");

        var result = await Task.Run(() =>
        {
            try
            {
                using var port = ArduinoSerial.Open(candidate.PortName, BaudRate);
                // Opening the port resets the board, and its own setup() has a
                // ~2s startup delay before it's reading serial at all -- same
                // reasoning as SerialManager.TryConnect().
                Thread.Sleep(2000);
                return FirmwareHandshake.Perform(port);
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException)
            {
                return new FirmwareHandshakeResult(HandshakeOutcome.Error, Error: ex);
            }
        });

        switch (result.Outcome)
        {
            case HandshakeOutcome.Success when result.ProtocolVersion == ExpectedProtocolVersion
                                                && result.FirmwareVersion == ExpectedFirmwareVersion:
                AppendLog($"Firmware is already up to date (protocol {result.ProtocolVersion}, firmware {result.FirmwareVersion}).");
                return false;

            case HandshakeOutcome.Success:
                AppendLog($"Update available: protocol {result.ProtocolVersion}, firmware {result.FirmwareVersion} "
                          + $"-> protocol {ExpectedProtocolVersion}, firmware {ExpectedFirmwareVersion}.");
                ShowFlashButton(candidate);
                return true;

            case HandshakeOutcome.NoResponse:
                AppendLog("Couldn't detect a firmware version -- this may be older firmware, or the board isn't fully connected.");
                AppendLog("You can still update to the bundled firmware version below.");
                ShowFlashButton(candidate);
                return true;

            case HandshakeOutcome.UnrecognizedResponse:
                AppendLog($"Unrecognized response from the board: {result.RawResponse}");
                return false;

            case HandshakeOutcome.Error:
                AppendLog($"Couldn't talk to {candidate.PortName}: {result.Error?.Message}");
                return false;

            default:
                return false;
        }
    }

    private void ShowFlashButton(ArduinoCandidate candidate)
    {
        _lastCandidate = candidate;
        FlashButton.Visibility = Visibility.Visible;
        FlashButton.IsEnabled = true;
    }

    private async void FlashButton_Click(object sender, RoutedEventArgs e)
    {
        if (_lastCandidate is null)
        {
            return;
        }

        FlashButton.IsEnabled = false;
        CheckButton.IsEnabled = false;

        var keepCompanionStopped = await FlashAsync(_lastCandidate);

        if (keepCompanionStopped)
        {
            // Either a manual-reset retry is pending, or the post-flash recheck
            // still wants an update -- the port needs to stay free, and the
            // button needs to stay clickable, for either case.
            FlashButton.IsEnabled = true;
        }
        else
        {
            RestartCompanion();
            FlashButton.Visibility = Visibility.Collapsed;
        }

        CheckButton.IsEnabled = true;
    }

    /// <summary>Returns true if the companion should stay stopped -- either a manual-reset
    /// retry is pending, or the post-flash recheck still wants an update -- and false
    /// once it's safe to restart the companion and hide the Update button.</summary>
    private async Task<bool> FlashAsync(ArduinoCandidate candidate)
    {
        var firmwareDir = Path.Combine(AppContext.BaseDirectory, "Firmware");
        var hexPath = Path.Combine(firmwareDir, BundledHexFileName);
        var avrdudePath = Path.Combine(firmwareDir, "avrdude", "avrdude.exe");
        var avrdudeConfPath = Path.Combine(firmwareDir, "avrdude", "avrdude.conf");

        if (!File.Exists(hexPath) || !File.Exists(avrdudePath) || !File.Exists(avrdudeConfPath))
        {
            AppendLog("Bundled firmware/avrdude files are missing -- can't flash.");
            return false;
        }

        AppendLog($"Entering bootloader mode on {candidate.PortName} (1200bps touch)...");
        var bootloaderPort = await Task.Run(() => TriggerBootloaderReset(candidate.PortName));

        if (bootloaderPort is null)
        {
            AppendLog("The bootloader port didn't appear on its own.");
            AppendLog("Double-tap the reset button on the board now, then click \"Update Firmware Now\" again.");
            return true;
        }

        AppendLog($"Bootloader detected on {bootloaderPort}. Flashing firmware...");
        var args = $"-C\"{avrdudeConfPath}\" -v -patmega32u4 -cavr109 -P{bootloaderPort} -b57600 -D -Uflash:w:\"{hexPath}\":i";
        var success = await Task.Run(() => RunAvrdude(avrdudePath, args));

        if (!success)
        {
            AppendLog("Update failed, but your board is safe -- the bootloader is protected and can't be damaged by this. Just try again.");
            return false;
        }

        AppendLog("Flash succeeded. Re-checking firmware version...");
        // avrdude resets the board out of the bootloader at the end of a normal
        // invocation -- give its setup() the same ~2s boot delay before re-querying.
        // Re-discover rather than trust the pre-flash candidate's port name: this
        // session already found the app-mode port isn't always stable across a
        // reset, same reasoning as the bootloader port below.
        await Task.Delay(2000);
        var recheckCandidates = await Task.Run(() => ArduinoPortFinder.FindCandidates(VidPidList));
        return await CheckCandidateAsync(recheckCandidates.Count == 1 ? recheckCandidates[0] : candidate);
    }

    /// <summary>
    /// The 1200bps touch: open then close the port at 1200 baud to trigger the
    /// Leonardo/Pro Micro's self-reset into the Caterina bootloader. The
    /// bootloader re-enumerates under its own VID:PID (2341:0036 -- distinct
    /// from the application's 2341:8036), and NOT necessarily the same COM
    /// port name as the application mode -- observed directly: assuming the
    /// same port name here caused avrdude to target the wrong (stale) port
    /// and fail. Returns the discovered bootloader port name, or null if it
    /// never appeared within the timeout.
    /// </summary>
    private static string? TriggerBootloaderReset(string portName)
    {
        try
        {
            using (var touchPort = new SerialPort(portName, 1200))
            {
                touchPort.Open();
                touchPort.Close();
            }
        }
        catch (Exception)
        {
            // Some boards drop the port immediately on touch, before Close()
            // returns cleanly -- not itself a failure signal, keep polling below.
        }

        var deadline = DateTime.UtcNow.AddMilliseconds(BootloaderDiscoveryTimeoutMs);
        while (DateTime.UtcNow < deadline)
        {
            Thread.Sleep(500);
            var bootloaderCandidates = ArduinoPortFinder.FindCandidates(BootloaderVidPidList);
            if (bootloaderCandidates.Count > 0)
            {
                // Give the bootloader a moment to finish settling before avrdude
                // tries to open it.
                Thread.Sleep(500);
                return bootloaderCandidates[0].PortName;
            }
        }

        return null;
    }

    private bool RunAvrdude(string exePath, string exeArgs)
    {
        try
        {
            using var process = new Process
            {
                StartInfo = new ProcessStartInfo(exePath, exeArgs)
                {
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                },
            };

            process.OutputDataReceived += (_, args) => LogFromWorkerThread(args.Data);
            process.ErrorDataReceived += (_, args) => LogFromWorkerThread(args.Data);

            process.Start();
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            process.WaitForExit();

            return process.ExitCode == 0;
        }
        catch (Exception ex)
        {
            LogFromWorkerThread($"Failed to run avrdude: {ex.Message}");
            return false;
        }
    }

    private void LogFromWorkerThread(string? line)
    {
        if (string.IsNullOrEmpty(line))
        {
            return;
        }

        Dispatcher.Invoke(() => AppendLog(line));
    }

    private void StopCompanion()
    {
        AppendLog("Stopping the MatricePad companion so this can use the port...");
        RunProcess("taskkill.exe", $"/F /IM {CompanionProcessName}");
        _companionStopped = true;
    }

    private void RestartCompanion()
    {
        if (!_companionStopped)
        {
            return;
        }

        AppendLog("Restarting the MatricePad companion...");
        RunProcess("schtasks.exe", $"/Run /TN \"{CompanionTaskName}\"");
        _companionStopped = false;
    }

    private static void RunProcess(string exe, string args)
    {
        try
        {
            using var process = Process.Start(new ProcessStartInfo(exe, args)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
            });
            process?.WaitForExit(5000);
        }
        catch
        {
            // Best-effort -- e.g. nothing to kill/nothing registered yet.
        }
    }

    private void AppendLog(string line)
    {
        OutputLog.AppendText(line + Environment.NewLine);
        OutputLog.ScrollToEnd();
    }
}
