using HEngine;

namespace HEngine.WebAssembly;

public sealed class GameSessionBridge<TSession> where TSession : class, IGameSession
{
    private readonly Func<string, string, TSession> mCreate;
    private TSession? mSession;

    public GameSessionBridge(Func<string, string, TSession> create)
    {
        mCreate = create;
    }

    public string Initialize(string catalog, string save)
    {
        var session = mCreate(catalog, save);
        var snapshot = session.Snapshot();
        mSession = session;
        return snapshot;
    }

    public string Command(string name, string value) => Session.Command(name, value);
    public string Step(double seconds, double x, double z) => Session.Step(seconds, x, z);
    public string Save() => Session.Save();

    private TSession Session => mSession ?? throw new InvalidOperationException("Game not initialized.");
}
