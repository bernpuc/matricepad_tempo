using SessionZeroProbe;

var builder = Host.CreateApplicationBuilder(args);
builder.Services.AddWindowsService(options =>
{
    options.ServiceName = "SessionZeroProbe";
});
builder.Services.AddHostedService<Worker>();

var host = builder.Build();
host.Run();
