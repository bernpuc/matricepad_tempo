using MatricePadApp.Models;
using MatricePadApp.Services;
using MatricePadApp.Services.Interfaces;
using MatricePadApp.Text;
using Microsoft.Extensions.Options;

namespace MatricePadApp;

public class Worker(
    IAudioStateProvider audioStateProvider,
    MediaInfoProvider mediaInfoProvider,
    AudioCaptureProvider audioCaptureProvider,
    ISerialManager serialManager,
    IOptions<MatricePadOptions> options,
    ILogger<Worker> logger) : BackgroundService
{
    private readonly MatricePadOptions _options = options.Value;

    private const double MaxExtrapolationSeconds = 4.0;
    private int? _lastRawPositionSec;
    private DateTime? _lastPositionChangeTimeUtc;

    private string _lastSentPacket = "";
    private DateTime _lastSendTimeUtc = DateTime.MinValue;

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        logger.LogInformation("MatricePadApp starting");

        // Each of these performs its own first-time audio-subsystem COM
        // initialization on its own thread; starting them at the same instant
        // is a known crash risk, so stagger by ~1s (see docs/spec-windows-app.md §9).
        mediaInfoProvider.Start();
        await Task.Delay(1000, stoppingToken);
        audioCaptureProvider.Start();

        using var timer = new PeriodicTimer(TimeSpan.FromMilliseconds(_options.MainLoopIntervalMs));
        while (!stoppingToken.IsCancellationRequested && await timer.WaitForNextTickAsync(stoppingToken))
        {
            try
            {
                Tick();
            }
            catch (Exception ex)
            {
                logger.LogError(ex, "Unhandled exception in main loop");
            }
        }

        logger.LogInformation("MatricePadApp stopping");
    }

    private void Tick()
    {
        var audioState = audioStateProvider.GetCurrent();
        var mediaInfo = mediaInfoProvider.GetCurrent();
        var barLevels = audioCaptureProvider.GetBarLevels();

        var elapsedSec = GetSmoothedElapsed(mediaInfo);
        var durationSec = mediaInfo?.DurationSec ?? 0;

        var song = mediaInfo is null ? "" : AsciiSanitizer.Sanitize(mediaInfo.Title.Trim());
        var artist = mediaInfo is null ? "" : AsciiSanitizer.Sanitize(mediaInfo.Artist.Trim());
        var paused = mediaInfo?.Status == PlaybackStatus.Paused;

        var packet = new SerialPacket(song, artist, audioState.Volume, audioState.IsMuted, paused, barLevels, elapsedSec, durationSec);
        var encoded = packet.Encode();

        var now = DateTime.UtcNow;
        var contentChanged = encoded != _lastSentPacket;
        if (contentChanged || (now - _lastSendTimeUtc).TotalMilliseconds >= _options.KeepaliveIntervalMs)
        {
            serialManager.Send(encoded);
            logger.Log(LogLevel.Debug, contentChanged ? "Packet sent (content change): {Packet}" : "Keepalive sent: {Packet}", encoded.TrimEnd('\n'));
            _lastSentPacket = encoded;
            _lastSendTimeUtc = now;
        }
    }

    /// <summary>
    /// The WinRT polling loop only refreshes PositionSec every WinRtPollIntervalMs, so
    /// reading it every 50ms tick sees a stair-step. Extrapolate forward from the last
    /// observed change using wall-clock time, capped so a source that never updates its
    /// position settles to a small, bounded offset instead of drifting forever.
    /// </summary>
    private int GetSmoothedElapsed(MediaInfo? mediaInfo)
    {
        if (mediaInfo is null)
        {
            _lastRawPositionSec = null;
            _lastPositionChangeTimeUtc = null;
            return 0;
        }

        var rawPosition = mediaInfo.PositionSec;
        var now = DateTime.UtcNow;

        if (rawPosition != _lastRawPositionSec)
        {
            _lastRawPositionSec = rawPosition;
            _lastPositionChangeTimeUtc = now;
        }

        if (mediaInfo.Status == PlaybackStatus.Playing)
        {
            var elapsedSinceChange = (now - _lastPositionChangeTimeUtc!.Value).TotalSeconds;
            var extrapolated = Math.Min(elapsedSinceChange, MaxExtrapolationSeconds);
            return (int)(_lastRawPositionSec!.Value + extrapolated);
        }

        return _lastRawPositionSec!.Value;
    }
}
