using MatricePadApp.Models;
using MatricePadApp.Services.Interfaces;
using Microsoft.Extensions.Options;
using NAudio.CoreAudioApi;

namespace MatricePadApp.Services;

public class AudioStateProvider : IAudioStateProvider, IDisposable
{
    private readonly MatricePadOptions _options;
    private readonly MMDevice _device;
    private readonly Lock _lock = new();
    private AudioState _cached = new(0, false);
    private DateTime _nextRefreshUtc = DateTime.MinValue;

    public AudioStateProvider(IOptions<MatricePadOptions> options)
    {
        _options = options.Value;
        var enumerator = new MMDeviceEnumerator();
        _device = enumerator.GetDefaultAudioEndpoint(DataFlow.Render, Role.Multimedia);
    }

    public AudioState GetCurrent()
    {
        lock (_lock)
        {
            var now = DateTime.UtcNow;
            if (now >= _nextRefreshUtc)
            {
                var volume = (int)Math.Round(_device.AudioEndpointVolume.MasterVolumeLevelScalar * 100);
                var muted = _device.AudioEndpointVolume.Mute;
                _cached = new AudioState(volume, muted);
                _nextRefreshUtc = now.AddMilliseconds(_options.SystemVolumePollIntervalMs);
            }

            return _cached;
        }
    }

    public void Dispose()
    {
        _device.Dispose();
        GC.SuppressFinalize(this);
    }
}
