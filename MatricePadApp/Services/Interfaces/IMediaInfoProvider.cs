using MatricePadApp.Models;

namespace MatricePadApp.Services.Interfaces;

public interface IMediaInfoProvider
{
    MediaInfo? GetCurrent();
}
