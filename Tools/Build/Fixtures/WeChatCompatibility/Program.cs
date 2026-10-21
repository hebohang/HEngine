using System.Runtime.InteropServices.JavaScript;
using System.Text.Json;
[assembly: System.Runtime.Versioning.SupportedOSPlatform("browser")]
public static partial class Program
{
    public static void Main() { }
    [JSExport] public static int ExceptionRoundTrip()
    {
        int result = 0;
        try { throw new InvalidOperationException("expected"); }
        catch (InvalidOperationException) { result = 40; }
        finally { result += 2; }
        try { JsonSerializer.Deserialize<int>("invalid json"); }
        catch (JsonException) { return result; }
        return -1;
    }
    [JSExport] public static void ThrowToHost() => throw new ArgumentException("expected-host-exception");
}
