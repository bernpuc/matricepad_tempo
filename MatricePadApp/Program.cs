using MatricePadApp;
using MatricePadApp.Logging;
using MatricePadApp.Services;
using MatricePadApp.Services.Interfaces;

var builder = Host.CreateApplicationBuilder(args);

builder.Services.Configure<MatricePadOptions>(builder.Configuration.GetSection(MatricePadOptions.SectionName));

builder.Services.AddSingleton<IAudioStateProvider, AudioStateProvider>();
builder.Services.AddSingleton<ISerialManager, SerialManager>();
builder.Services.AddSingleton<MediaInfoProvider>();
builder.Services.AddSingleton<IMediaInfoProvider>(sp => sp.GetRequiredService<MediaInfoProvider>());
builder.Services.AddSingleton<AudioCaptureProvider>();
builder.Services.AddSingleton<IAudioCaptureProvider>(sp => sp.GetRequiredService<AudioCaptureProvider>());
builder.Services.AddHostedService<Worker>();

var logDirectory = Path.Combine(
    Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "MatricePad", "logs");
builder.Logging.AddProvider(new FileLoggerProvider(logDirectory));

var host = builder.Build();
host.Run();
