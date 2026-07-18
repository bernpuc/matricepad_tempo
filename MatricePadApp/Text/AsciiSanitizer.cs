using System.Text;

namespace MatricePadApp.Text;

public static class AsciiSanitizer
{
    private static readonly Dictionary<char, string> UnicodeReplacements = new()
    {
        ['‘'] = "'", ['’'] = "'", ['‚'] = "'", ['‛'] = "'", // single quotes
        ['“'] = "\"", ['”'] = "\"", ['„'] = "\"", ['‟'] = "\"", // double quotes
        ['–'] = "-", ['—'] = "-", ['―'] = "-", // dashes
        ['…'] = "...", ['•'] = "*", ['·'] = ".", // ellipsis, bullets
    };

    public static string Sanitize(string text)
    {
        var replaced = new StringBuilder(text.Length);
        foreach (var c in text)
        {
            replaced.Append(UnicodeReplacements.TryGetValue(c, out var repl) ? repl : c.ToString());
        }

        var normalized = replaced.ToString().Normalize(System.Text.NormalizationForm.FormKD);

        var ascii = new StringBuilder(normalized.Length);
        foreach (var c in normalized)
        {
            if (c <= 0x7F)
            {
                ascii.Append(c);
            }
        }

        return ascii.ToString();
    }
}
