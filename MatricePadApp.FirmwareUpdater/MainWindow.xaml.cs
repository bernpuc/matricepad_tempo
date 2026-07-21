using System.Diagnostics;
using System.IO;
using System.Windows;
using MatricePad.SerialCore;

namespace MatricePadApp.FirmwareUpdater;

public partial class MainWindow : Window
{
    // Mirrors MatricePadApp's appsettings.json default ArduinoVidPidList.
    private static readonly string[] VidPidList = ["1B4F:9206", "1B4F:9205", "2341:8036"];
    private const int BaudRate = 115200;

    // TODO: read from the bundled .hex/manifest once firmware packaging
    // (docs/spec-firmwareUpdater.md §3) is wired up. For now these mirror
    // matrice_pad_tempo.ino's current PROTOCOL_VERSION/FIRMWARE_VERSION,
    // representing "what this build of the updater knows the latest
    // firmware to be."
    private const int ExpectedProtocolVersion = 1;
    private const string ExpectedFirmwareVersion = "1.1.0";

    private const string CompanionProcessName = "MatricePadApp.exe";
    private const string CompanionTaskName = "MatricePadApp";

    private bool _companionStopped;

    public MainWindow()
    {
        InitializeComponent();
    }

    private async void CheckButton_Click(object sender, RoutedEventArgs e)
    {
        CheckButton.IsEnabled = false;
        CandidatePanel.Visibility = Visibility.Collapsed;
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
            await CheckCandidateAsync(candidates[0]);
            RestartCompanion();
            CheckButton.IsEnabled = true;
            return;
        }

        AppendLog($"Found {candidates.Count} matching boards -- pick the one to check.");
        CandidateList.ItemsSource = candidates.Select(c => $"{c.FriendlyName}").ToList();
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
        await CheckCandidateAsync(candidate);
        RestartCompanion();
        CheckSelectedButton.IsEnabled = true;
    }

    private async Task CheckCandidateAsync(ArduinoCandidate candidate)
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
                break;
            case HandshakeOutcome.Success:
                AppendLog($"Update available: protocol {result.ProtocolVersion}, firmware {result.FirmwareVersion} "
                          + $"-> protocol {ExpectedProtocolVersion}, firmware {ExpectedFirmwareVersion}.");
                AppendLog("(Flashing isn't implemented yet -- this check-only build just reports what it found.)");
                break;
            case HandshakeOutcome.NoResponse:
                AppendLog("Couldn't detect a firmware version -- this may be older firmware, or the board isn't fully connected.");
                break;
            case HandshakeOutcome.UnrecognizedResponse:
                AppendLog($"Unrecognized response from the board: {result.RawResponse}");
                break;
            case HandshakeOutcome.Error:
                AppendLog($"Couldn't talk to {candidate.PortName}: {result.Error?.Message}");
                break;
        }
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
