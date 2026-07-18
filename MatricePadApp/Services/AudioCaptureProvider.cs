using MatricePadApp.Services.Interfaces;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using NAudio.CoreAudioApi;
using NAudio.Dsp;
using NAudio.Wave;

namespace MatricePadApp.Services;

public class AudioCaptureProvider : IAudioCaptureProvider, IDisposable
{
    private readonly AudioCaptureOptions _options;
    private readonly ILogger<AudioCaptureProvider> _logger;
    private readonly Lock _lock = new();
    private readonly int[] _levels;
    private readonly double[] _bandEdges;
    private readonly double[] _hannWindow;
    private readonly double _hannWindowSum;
    private readonly List<float> _sampleBuffer = [];
    private WasapiLoopbackCapture? _capture;

    public AudioCaptureProvider(IOptions<MatricePadOptions> options, ILogger<AudioCaptureProvider> logger)
    {
        _options = options.Value.AudioCapture;
        _logger = logger;
        _levels = new int[_options.NumBars];
        _bandEdges = LogSpace(_options.BandMinHz, _options.BandMaxHz, _options.NumBars + 1);
        _hannWindow = HannWindow(_options.ChunkSize);
        _hannWindowSum = _hannWindow.Sum();
    }

    public void Start()
    {
        try
        {
            _capture = new WasapiLoopbackCapture();
            _capture.DataAvailable += OnDataAvailable;
            _capture.RecordingStopped += (_, e) =>
            {
                if (e.Exception is not null)
                {
                    _logger.LogWarning(e.Exception, "Audio capture stopped unexpectedly");
                }
            };
            _capture.StartRecording();
            _logger.LogInformation("Audio capture started: {Format}", _capture.WaveFormat);
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex, "Could not open loopback device");
        }
    }

    public int[] GetBarLevels()
    {
        lock (_lock)
        {
            return (int[])_levels.Clone();
        }
    }

    private void OnDataAvailable(object? sender, WaveInEventArgs e)
    {
        try
        {
            var format = _capture!.WaveFormat;
            var channels = format.Channels;
            var bytesPerSample = format.BitsPerSample / 8;
            var isFloat = format.Encoding == WaveFormatEncoding.IeeeFloat;
            var frameSize = bytesPerSample * channels;
            var frames = e.BytesRecorded / frameSize;

            for (var i = 0; i < frames; i++)
            {
                float sum = 0;
                var frameOffset = i * frameSize;
                for (var c = 0; c < channels; c++)
                {
                    var idx = frameOffset + c * bytesPerSample;
                    var sample = isFloat
                        ? BitConverter.ToSingle(e.Buffer, idx)
                        : BitConverter.ToInt16(e.Buffer, idx) / 32768f;
                    sum += sample;
                }

                _sampleBuffer.Add(sum / channels);
            }

            var chunkSize = _options.ChunkSize;
            while (_sampleBuffer.Count >= chunkSize)
            {
                ProcessChunk(_sampleBuffer.GetRange(0, chunkSize), format.SampleRate);
                _sampleBuffer.RemoveRange(0, chunkSize);
            }
        }
        catch (Exception ex)
        {
            _logger.LogDebug(ex, "Audio capture frame error");
        }
    }

    private void ProcessChunk(List<float> chunk, int sampleRate)
    {
        var n = chunk.Count;
        var m = (int)Math.Log2(n);
        var fftBuffer = new Complex[n];
        for (var i = 0; i < n; i++)
        {
            fftBuffer[i].X = (float)(chunk[i] * _hannWindow[i]);
            fftBuffer[i].Y = 0;
        }

        FastFourierTransform.FFT(true, m, fftBuffer);

        // NAudio's FastFourierTransform divides by N on the forward transform (unlike
        // numpy's rfft, which the dB-floor/ceiling constants and this normalization were
        // tuned against) -- multiply back by n to undo that before applying the window
        // energy normalization, so magnitude scale matches the Python original.
        var halfN = n / 2 + 1;
        var magnitude = new double[halfN];
        for (var i = 0; i < halfN; i++)
        {
            magnitude[i] = Math.Sqrt(fftBuffer[i].X * fftBuffer[i].X + fftBuffer[i].Y * fftBuffer[i].Y) * n * 2.0 / _hannWindowSum;
        }

        var instantLevels = BinToLevels(magnitude, sampleRate, n);

        lock (_lock)
        {
            for (var i = 0; i < _levels.Length; i++)
            {
                _levels[i] = Math.Max(instantLevels[i], _levels[i] - _options.DecayStepPerFrame);
            }
        }
    }

    private int[] BinToLevels(double[] magnitude, int sampleRate, int fftSize)
    {
        var numBars = _options.NumBars;
        var levels = new int[numBars];
        for (var bar = 0; bar < numBars; bar++)
        {
            var lo = _bandEdges[bar];
            var hi = _bandEdges[bar + 1];
            var isLastBar = bar == numBars - 1;
            var bandMag = 0.0;
            var any = false;
            for (var k = 0; k < magnitude.Length; k++)
            {
                var freq = k * (double)sampleRate / fftSize;
                var inBand = isLastBar ? (freq >= lo && freq <= hi) : (freq >= lo && freq < hi);
                if (!inBand)
                {
                    continue;
                }

                any = true;
                if (magnitude[k] > bandMag)
                {
                    bandMag = magnitude[k];
                }
            }

            var db = 20.0 * Math.Log10((any ? bandMag : 0.0) + 1e-9);
            db = Math.Clamp(db, _options.DbFloor, _options.DbCeiling);
            levels[bar] = (int)Math.Round((db - _options.DbFloor) / (_options.DbCeiling - _options.DbFloor) * 100);
        }

        return levels;
    }

    private static double[] LogSpace(double min, double max, int count)
    {
        var result = new double[count];
        var logMin = Math.Log10(min);
        var logMax = Math.Log10(max);
        for (var i = 0; i < count; i++)
        {
            result[i] = Math.Pow(10, logMin + (logMax - logMin) * i / (count - 1));
        }

        return result;
    }

    private static double[] HannWindow(int size)
    {
        var window = new double[size];
        for (var i = 0; i < size; i++)
        {
            window[i] = 0.5 - 0.5 * Math.Cos(2 * Math.PI * i / (size - 1));
        }

        return window;
    }

    public void Dispose()
    {
        if (_capture is not null)
        {
            _capture.DataAvailable -= OnDataAvailable;
            _capture.StopRecording();
            _capture.Dispose();
        }

        GC.SuppressFinalize(this);
    }
}
