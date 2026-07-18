namespace MatricePadApp.Models;

public record SerialPacket(
    string Song,
    string Artist,
    int Volume,
    bool IsMuted,
    bool Paused,
    int[] BarLevels,
    int ElapsedSec,
    int DurationSec)
{
    public string Encode() =>
        $"{Song}||{Artist}||{Volume}||{(IsMuted ? 1 : 0)}||{(Paused ? 1 : 0)}||{string.Join(',', BarLevels)}||{ElapsedSec}||{DurationSec}\n";
}
