namespace MatricePadApp.Services.Interfaces;

public interface IAudioCaptureProvider
{
    /// <returns>Always 16 elements, each 0-100.</returns>
    int[] GetBarLevels();
}
