namespace MatricePadApp.Services.Interfaces;

public interface ISerialManager
{
    bool IsConnected { get; }
    void Send(string data);
    event EventHandler<string>? LineReceived;
}
