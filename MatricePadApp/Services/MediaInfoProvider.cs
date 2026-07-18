using System.Diagnostics;
using System.Text;
using MatricePadApp.Models;
using MatricePadApp.Native;
using MatricePadApp.Services.Interfaces;
using MatricePadApp.Text;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using NAudio.CoreAudioApi;
using NAudio.CoreAudioApi.Interfaces;
using Windows.Media.Control;

namespace MatricePadApp.Services;

public class MediaInfoProvider : IMediaInfoProvider, IDisposable
{
    private readonly MatricePadOptions _options;
    private readonly ILogger<MediaInfoProvider> _logger;
    private readonly HashSet<string> _browserProcessNames;
    private readonly HashSet<string> _noisyWindowTitles;

    private readonly Lock _winRtLock = new();
    private MediaInfo? _winRtInfo;
    private MediaInfo? _lastPlaying;

    private readonly Lock _titleLock = new();
    private string _windowTitleCache = "No media playing";
    private DateTime _windowTitleNextRefreshUtc = DateTime.MinValue;

    private Thread? _pollThread;
    private CancellationTokenSource? _pollCts;

    public MediaInfoProvider(IOptions<MatricePadOptions> options, ILogger<MediaInfoProvider> logger)
    {
        _options = options.Value;
        _logger = logger;
        _browserProcessNames = new HashSet<string>(_options.BrowserProcessNames, StringComparer.OrdinalIgnoreCase);
        _noisyWindowTitles = new HashSet<string>(_options.NoisyWindowTitles, StringComparer.OrdinalIgnoreCase);
    }

    public void Start()
    {
        _pollCts = new CancellationTokenSource();
        _pollThread = new Thread(() => WinRtPollLoop(_pollCts.Token)) { IsBackground = true };
        _pollThread.Start();
    }

    public MediaInfo? GetCurrent()
    {
        var windowTitle = GetWindowTitleCached();

        MediaInfo? winRtInfo;
        MediaInfo? lastPlaying;
        lock (_winRtLock)
        {
            winRtInfo = _winRtInfo;
            lastPlaying = _lastPlaying;
        }

        if (windowTitle == "")
        {
            // Browser is active — use WinRT metadata. Song/artist parsing only happens
            // when a title is present, but Status (used for Paused persistence by the
            // Worker) is preserved either way, matching the Python original.
            if (winRtInfo is null)
            {
                return null;
            }

            var title = "";
            var artist = "";
            if (winRtInfo.Title.Length > 0)
            {
                (artist, title) = WinRtTitleParser.Parse(winRtInfo.Title, winRtInfo.Artist);
            }

            return winRtInfo with { Title = title, Artist = artist };
        }

        if (windowTitle == "No media playing")
        {
            // No active audio session — retain last known playing track as a paused
            // display, but only if it actually had a title (matches Python: the
            // paused-persistence flag only fires alongside a non-empty last title).
            if (lastPlaying is null || lastPlaying.Title.Length == 0)
            {
                return null;
            }

            var (lastArtist, lastTitle) = WinRtTitleParser.Parse(lastPlaying.Title, lastPlaying.Artist);
            return lastPlaying with { Status = PlaybackStatus.Paused, Title = lastTitle, Artist = lastArtist };
        }

        // Non-browser app is playing — its window title is the source
        var (song, parsedArtist) = WindowTitleParser.Parse(windowTitle);
        if (parsedArtist == "")
        {
            parsedArtist = windowTitle;
        }

        return new MediaInfo(song, parsedArtist, "", PlaybackStatus.Unknown, 0, 0);
    }

    // --- WinRT polling ---

    private void WinRtPollLoop(CancellationToken token)
    {
        var noneCount = 0;
        while (!token.IsCancellationRequested)
        {
            try
            {
                var info = PollMediaInfoAsync().GetAwaiter().GetResult();
                if (info is not null)
                {
                    noneCount = 0;
                    lock (_winRtLock)
                    {
                        _winRtInfo = info;
                        if (info.Status == PlaybackStatus.Playing)
                        {
                            _lastPlaying = info;
                        }
                    }

                    _logger.LogDebug("WinRT metadata: '{Title}' by '{Artist}' status={Status}", info.Title, info.Artist, info.Status);
                }
                else
                {
                    noneCount++;
                    if (noneCount >= _options.PausedSessionClearCountdown)
                    {
                        noneCount = 0;
                        lock (_winRtLock)
                        {
                            _winRtInfo = null;
                            _lastPlaying = null;
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                _logger.LogWarning(ex, "WinRT session poll error");
            }

            token.WaitHandle.WaitOne(_options.WinRtPollIntervalMs);
        }
    }

    private async Task<MediaInfo?> PollMediaInfoAsync()
    {
        var manager = await GlobalSystemMediaTransportControlsSessionManager.RequestAsync();

        GlobalSystemMediaTransportControlsSession? chosen = null;
        foreach (var session in manager.GetSessions())
        {
            try
            {
                var playbackInfo = session.GetPlaybackInfo();
                if (playbackInfo?.PlaybackStatus == GlobalSystemMediaTransportControlsSessionPlaybackStatus.Playing)
                {
                    chosen = session;
                    break;
                }
            }
            catch (Exception ex)
            {
                _logger.LogDebug(ex, "WinRT session inspect error");
            }
        }

        chosen ??= manager.GetCurrentSession();
        if (chosen is null)
        {
            return null;
        }

        var chosenPlaybackInfo = chosen.GetPlaybackInfo();
        var status = chosenPlaybackInfo?.PlaybackStatus ?? GlobalSystemMediaTransportControlsSessionPlaybackStatus.Closed;
        var props = await chosen.TryGetMediaPropertiesAsync();

        var positionSec = 0;
        var durationSec = 0;
        try
        {
            var timeline = chosen.GetTimelineProperties();
            var position = timeline.Position;
            if (status == GlobalSystemMediaTransportControlsSessionPlaybackStatus.Playing)
            {
                var elapsedSinceUpdate = DateTimeOffset.UtcNow - timeline.LastUpdatedTime;
                if (elapsedSinceUpdate > TimeSpan.Zero)
                {
                    position += elapsedSinceUpdate;
                }
            }

            positionSec = (int)position.TotalSeconds;
            durationSec = (int)timeline.EndTime.TotalSeconds;
        }
        catch (Exception ex)
        {
            _logger.LogDebug(ex, "WinRT timeline read error");
        }

        var mappedStatus = status switch
        {
            GlobalSystemMediaTransportControlsSessionPlaybackStatus.Playing => PlaybackStatus.Playing,
            GlobalSystemMediaTransportControlsSessionPlaybackStatus.Paused => PlaybackStatus.Paused,
            _ => PlaybackStatus.Unknown,
        };

        return new MediaInfo(
            props.Title ?? "",
            props.Artist ?? "",
            props.AlbumArtist ?? "",
            mappedStatus,
            positionSec,
            durationSec);
    }

    // --- Window title polling ---

    private string GetWindowTitleCached()
    {
        lock (_titleLock)
        {
            var now = DateTime.UtcNow;
            if (now >= _windowTitleNextRefreshUtc)
            {
                _windowTitleCache = ComputeAudioPlayingWindowTitle();
                _windowTitleNextRefreshUtc = now.AddMilliseconds(_options.WindowTitlePollIntervalMs);
            }

            return _windowTitleCache;
        }
    }

    private string ComputeAudioPlayingWindowTitle()
    {
        try
        {
            using var enumerator = new MMDeviceEnumerator();
            using var device = enumerator.GetDefaultAudioEndpoint(DataFlow.Render, Role.Multimedia);
            var sessions = device.AudioSessionManager.Sessions;
            for (var i = 0; i < sessions.Count; i++)
            {
                var session = sessions[i];
                if (session.State != AudioSessionState.AudioSessionStateActive)
                {
                    continue;
                }

                uint pid;
                try
                {
                    pid = session.GetProcessID;
                }
                catch (Exception ex)
                {
                    _logger.LogDebug(ex, "Failed to read audio session process id");
                    continue;
                }

                if (pid == 0)
                {
                    continue;
                }

                string processName;
                try
                {
                    processName = Process.GetProcessById((int)pid).ProcessName;
                }
                catch (Exception)
                {
                    continue;
                }

                if (_browserProcessNames.Contains(processName))
                {
                    return "";
                }

                var title = FindLongestVisibleWindowTitle(pid);
                if (title is not null)
                {
                    return title;
                }
            }
        }
        catch (Exception ex)
        {
            _logger.LogDebug(ex, "Window title poll error");
        }

        return "No media playing";
    }

    private string? FindLongestVisibleWindowTitle(uint targetPid)
    {
        var titles = new List<string>();
        var buffer = new StringBuilder(512);

        bool Callback(nint hWnd, nint lParam)
        {
            if (!Win32.IsWindowVisible(hWnd))
            {
                return true;
            }

            Win32.GetWindowThreadProcessId(hWnd, out var windowPid);
            if (windowPid != targetPid)
            {
                return true;
            }

            buffer.Clear();
            var len = Win32.GetWindowText(hWnd, buffer, buffer.Capacity);
            if (len == 0)
            {
                return true;
            }

            var title = buffer.ToString();
            if (!string.IsNullOrWhiteSpace(title) && !_noisyWindowTitles.Contains(title))
            {
                titles.Add(title);
            }

            return true;
        }

        Win32.EnumWindows(Callback, 0);

        return titles.Count > 0 ? titles.OrderByDescending(t => t.Length).First() : null;
    }

    public void Dispose()
    {
        _pollCts?.Cancel();
        GC.SuppressFinalize(this);
    }
}
