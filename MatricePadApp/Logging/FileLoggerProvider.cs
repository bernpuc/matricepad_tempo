namespace MatricePadApp.Logging;

/// <summary>
/// Minimal rolling-by-date file sink. Per-user app (%APPDATA%), not a service,
/// so there's deliberately no Windows Event Log provider here.
///
/// Level filtering is delegated entirely to the standard "Logging:File:LogLevel"
/// config section (via this ProviderAlias) rather than a constructor argument --
/// a level baked in here would be silently overridden anyway, since the generic
/// host's filtering pipeline gates every provider on "Logging:LogLevel:Default"
/// before this provider's Log() is ever called unless a provider-specific rule exists.
/// </summary>
[ProviderAlias("File")]
public sealed class FileLoggerProvider(string logDirectory) : ILoggerProvider
{
    private readonly Lock _writeLock = new();

    public ILogger CreateLogger(string categoryName) => new FileLogger(this, categoryName);

    internal void Write(string line)
    {
        Directory.CreateDirectory(logDirectory);
        var path = Path.Combine(logDirectory, $"matricepad-{DateTime.Now:yyyy-MM-dd}.log");
        lock (_writeLock)
        {
            File.AppendAllText(path, line + Environment.NewLine);
        }
    }

    public void Dispose()
    {
    }
}

internal sealed class FileLogger(FileLoggerProvider provider, string categoryName) : ILogger
{
    public IDisposable? BeginScope<TState>(TState state) where TState : notnull => null;

    // The host's logging pipeline already applies "Logging:File:LogLevel" (via the
    // FileLoggerProvider's ProviderAlias) before Log() is ever called, so this is
    // always true -- there's no independent level to gate here.
    public bool IsEnabled(LogLevel logLevel) => true;

    public void Log<TState>(LogLevel logLevel, EventId eventId, TState state, Exception? exception, Func<TState, Exception?, string> formatter)
    {
        var line = $"{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff} [{logLevel}] {categoryName}: {formatter(state, exception)}";
        if (exception is not null)
        {
            line += Environment.NewLine + exception;
        }

        provider.Write(line);
    }
}
