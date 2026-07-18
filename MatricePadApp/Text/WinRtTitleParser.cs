using System.Text.RegularExpressions;

namespace MatricePadApp.Text;

/// <summary>
/// Parses artist/song from a YouTube (or similar) WinRT title + artist field.
/// </summary>
public static partial class WinRtTitleParser
{
    private static readonly Regex[] QualifierPatterns =
    [
        new Regex(@"\s*\(Official(?:\s+Music)?\s+Video\)", RegexOptions.IgnoreCase),
        new Regex(@"\s*\[Official(?:\s+(?:Music|HD)\s+)?Video\]", RegexOptions.IgnoreCase),
        new Regex(@"\s*\(Official(?:\s+Music)?\s+Audio\)", RegexOptions.IgnoreCase),
        new Regex(@"\s*\(Official\s+Lyric\s+Video\)", RegexOptions.IgnoreCase),
        new Regex(@"\s*\(Official\s+Visualizer\)", RegexOptions.IgnoreCase),
        new Regex(@"\s*\(Official\)", RegexOptions.IgnoreCase),
        new Regex(@"\s*\(Lyric\s+Video\)", RegexOptions.IgnoreCase),
        new Regex(@"\s*\[4K[^\]]*\]", RegexOptions.IgnoreCase),
        new Regex(@"\s*\[HD\]", RegexOptions.IgnoreCase),
        new Regex(@"\s*\(Full\s+Album\)", RegexOptions.IgnoreCase),
        new Regex(@"\s*\(feat\.[^)]*\)", RegexOptions.IgnoreCase),
        new Regex(@"\s*\(ft\.[^)]*\)", RegexOptions.IgnoreCase),
        new Regex(@"\s+ft\.\s+[^()\[\]]+$", RegexOptions.IgnoreCase),
        new Regex(@"\s*[-–]\s*YouTube(?:\s+Music)?\s*$", RegexOptions.IgnoreCase),
    ];

    [GeneratedRegex(
        @"\b(lofi|lo-fi|beats\s+to\s+(relax|chill|study)|playlist|compilation|mix\b|full\s+album|live\s+stream|24/7|vol\.?\s*\d)\b",
        RegexOptions.IgnoreCase)]
    private static partial Regex NonMusicPattern();

    [GeneratedRegex(@"\b(&|\+|and|feat\.|ft\.)\b", RegexOptions.IgnoreCase)]
    private static partial Regex ArtistConnectorPattern();

    [GeneratedRegex(@"^The\s+[A-Z]")]
    private static partial Regex TheBandPattern();

    [GeneratedRegex(
        @"^(In\s+The|Somebody|Something|Nothing|Everything|Anyone|Everyone|Never|Always|Sometimes|Forever|Tonight|Yesterday|Tomorrow|Beautiful|Wonderful|Crazy|Falling|Running|Standing|What\s+|How\s+)",
        RegexOptions.IgnoreCase)]
    private static partial Regex SongPhraseStarterPattern();

    private static string StripQualifiers(string text)
    {
        foreach (var pattern in QualifierPatterns)
        {
            text = pattern.Replace(text, "");
        }

        return text.Trim();
    }

    private static bool IsLikelyArtist(string s)
    {
        var words = s.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        if (ArtistConnectorPattern().IsMatch(s))
        {
            return true;
        }

        if (TheBandPattern().IsMatch(s) && words.Length <= 4)
        {
            return true;
        }

        if (words.Length == 1 && s == s.ToUpperInvariant() && s.Length >= 3 && s.Any(char.IsLetter))
        {
            return true;
        }

        if (words.Length <= 3 && words.Length > 0 &&
            words.All(w => w.Length == 0 || char.IsUpper(w[0]) || !char.IsLetter(w[0])))
        {
            return true;
        }

        return false;
    }

    private static bool IsLikelySongPhrase(string s)
    {
        var wordCount = s.Split(' ', StringSplitOptions.RemoveEmptyEntries).Length;
        if (SongPhraseStarterPattern().IsMatch(s) && wordCount >= 3)
        {
            return true;
        }

        return wordCount >= 5;
    }

    /// <returns>(artist, song).</returns>
    public static (string Artist, string Song) Parse(string rawTitle, string winRtArtist)
    {
        if (NonMusicPattern().IsMatch(rawTitle))
        {
            return (winRtArtist, StripQualifiers(rawTitle));
        }

        var t = StripQualifiers(rawTitle);
        var isVevo = winRtArtist.ToUpperInvariant().EndsWith("VEVO");

        // Topic channel / YouTube Music: no ' - ' in title, artist field is clean
        if (!t.Contains(" - ") && !string.IsNullOrEmpty(winRtArtist) && !isVevo)
        {
            return (winRtArtist, t);
        }

        // No delimiter — fall back to WinRT artist
        if (!t.Contains(" - "))
        {
            return (winRtArtist, t);
        }

        var idx = t.IndexOf(" - ", StringComparison.Ordinal);
        var left = t[..idx].Trim();
        var right = StripQualifiers(t[(idx + 3)..]).Trim();

        // Inversion: "Song Title - Artist Name"
        var rightWordCount = right.Split(' ', StringSplitOptions.RemoveEmptyEntries).Length;
        if (!IsLikelyArtist(left) && IsLikelySongPhrase(left) &&
            (rightWordCount <= 3 || IsLikelyArtist(right)))
        {
            return (right, left);
        }

        return (left, right);
    }
}
