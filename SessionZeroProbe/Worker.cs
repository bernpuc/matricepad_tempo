using System.Diagnostics;
using System.Text;
using NAudio.CoreAudioApi;
using Windows.Media.Control;

namespace SessionZeroProbe;

// Throwaway probe: does a LocalSystem Windows Service have access to WinRT
// media-session info and the default audio endpoint? Both are normally
// interactive-session (desktop) concepts, and services run in Session 0,
// isolated from the desktop. Result decides whether the .NET rewrite of the
// Python host script can be a true Windows Service, or needs to be a
// per-user startup app instead. See docs/spec-windows-service.md.
public class Worker(ILogger<Worker> logger) : BackgroundService
{
    private static readonly string LogPath = @"C:\ProgramData\SessionZeroProbe\probe.log";

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(LogPath)!);
        await RunProbeAsync();

        // Keep the service "running" so the SCM doesn't treat an immediate
        // exit as a crash -- the actual result is in the log file.
        while (!stoppingToken.IsCancellationRequested)
        {
            await Task.Delay(TimeSpan.FromMinutes(5), stoppingToken);
        }
    }

    private async Task RunProbeAsync()
    {
        var sb = new StringBuilder();
        sb.AppendLine($"=== SessionZeroProbe run at {DateTimeOffset.Now} ===");
        sb.AppendLine($"Session ID: {Process.GetCurrentProcess().SessionId}");
        sb.AppendLine($"User: {Environment.UserName}");
        sb.AppendLine($"Is interactive (Environment.UserInteractive): {Environment.UserInteractive}");

        try
        {
            var manager = await GlobalSystemMediaTransportControlsSessionManager.RequestAsync();
            var sessions = manager.GetSessions();
            sb.AppendLine($"[WinRT] SUCCESS - RequestAsync returned, {sessions.Count} session(s)");
            var current = manager.GetCurrentSession();
            sb.AppendLine($"[WinRT] Current session: {current?.SourceAppUserModelId ?? "none"}");
        }
        catch (Exception ex)
        {
            sb.AppendLine($"[WinRT] FAILED: {ex.GetType().Name}: {ex.Message}");
        }

        try
        {
            var enumerator = new MMDeviceEnumerator();
            var device = enumerator.GetDefaultAudioEndpoint(DataFlow.Render, Role.Multimedia);
            sb.AppendLine($"[NAudio] SUCCESS - default device: {device.FriendlyName}, "
                         + $"volume={device.AudioEndpointVolume.MasterVolumeLevelScalar}");
        }
        catch (Exception ex)
        {
            sb.AppendLine($"[NAudio] FAILED: {ex.GetType().Name}: {ex.Message}");
        }

        sb.AppendLine();
        await File.AppendAllTextAsync(LogPath, sb.ToString());
        logger.LogInformation("Probe complete, see {LogPath}", LogPath);
    }
}
