using System.Text.RegularExpressions;

namespace MatricePadApp.Text;

/// <summary>
/// Parses artist/song from local player window titles (Zune, VLC, Media Player).
/// </summary>
public static partial class WindowTitleParser
{
    [GeneratedRegex(@"^\d+\s(.*)\s-\s(.*)\s-\s.*$")]
    private static partial Regex TrackArtistSongApp();

    [GeneratedRegex(@"^\d+\s(.*)\s-\s(.*)$")]
    private static partial Regex TrackArtistSong();

    [GeneratedRegex(@"^(.*)\s-\s(.*)$")]
    private static partial Regex ArtistSong();

    /// <returns>(song, artist), both empty if no pattern matches.</returns>
    public static (string Song, string Artist) Parse(string title)
    {
        var m = TrackArtistSongApp().Match(title);
        if (m.Success)
        {
            return (m.Groups[2].Value, m.Groups[1].Value);
        }

        m = TrackArtistSong().Match(title);
        if (m.Success)
        {
            return (m.Groups[2].Value, m.Groups[1].Value);
        }

        m = ArtistSong().Match(title);
        if (m.Success)
        {
            return (m.Groups[2].Value, m.Groups[1].Value);
        }

        return ("", "");
    }
}
