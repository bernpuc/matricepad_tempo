namespace MatricePadApp;

public class MatricePadOptions
{
    public const string SectionName = "MatricePad";

    public string? ComPort { get; set; }
    public int BaudRate { get; set; } = 115200;
    public int KeepaliveIntervalMs { get; set; } = 2500;
    public int ConnectionTimeoutMs { get; set; } = 5000;
    public int ReconnectDelayMs { get; set; } = 2000;
    public int MainLoopIntervalMs { get; set; } = 50;
    public int SystemVolumePollIntervalMs { get; set; } = 150;
    public int WindowTitlePollIntervalMs { get; set; } = 1000;
    public int WinRtPollIntervalMs { get; set; } = 3000;
    public int PausedSessionClearCountdown { get; set; } = 3;
    public string[] ArduinoVidPidList { get; set; } = [];
    public string[] BrowserProcessNames { get; set; } = [];
    public string[] NoisyWindowTitles { get; set; } = [];
    public AudioCaptureOptions AudioCapture { get; set; } = new();
}

public class AudioCaptureOptions
{
    public int SampleRate { get; set; } = 44100;
    public int ChunkSize { get; set; } = 1024;
    public int NumBars { get; set; } = 16;
    public double BandMinHz { get; set; } = 60;
    public double BandMaxHz { get; set; } = 16000;
    public double DbFloor { get; set; } = -60;
    public double DbCeiling { get; set; } = 0;
    public int DecayStepPerFrame { get; set; } = 8;
}
