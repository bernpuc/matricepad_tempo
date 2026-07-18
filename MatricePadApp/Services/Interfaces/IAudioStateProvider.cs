using MatricePadApp.Models;

namespace MatricePadApp.Services.Interfaces;

public interface IAudioStateProvider
{
    AudioState GetCurrent();
}
