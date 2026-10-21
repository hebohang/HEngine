namespace HEngine;

public interface IGameSession
{
    string Snapshot();
    string Command(string name, string value);
    string Step(double seconds, double x, double z);
    string Save();
}
