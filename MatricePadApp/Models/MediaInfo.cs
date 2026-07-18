namespace MatricePadApp.Models;

public record MediaInfo(
    string Title,
    string Artist,
    string AlbumArtist,
    PlaybackStatus Status,
    int PositionSec,
    int DurationSec
);

public enum PlaybackStatus
{
    Unknown = -1,
    Playing = 4,
    Paused = 5,
}
