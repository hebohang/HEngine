using System;
using System.Runtime.InteropServices.JavaScript;

public static partial class Probe
{
    private static int sCount;

    public static void Main() { Console.WriteLine("[he-csharp-managed-main] " + Identity()); }

    [JSExport]
    public static string Identity() => "he-csharp-probe-v1:" + Environment.Version;

    [JSExport]
    public static int Step(int delta)
    {
        checked { sCount += delta; }
        return sCount;
    }

    [JSExport]
    public static string ExerciseRuntime()
    {
        var root = new object();
        var weak = new WeakReference(root);
        for (var i = 0; i < 64; ++i)
            _ = new byte[16 * 1024];
        GC.Collect();
        GC.KeepAlive(root);
        try { throw new InvalidOperationException("controlled-csharp-exception"); }
        catch (Exception error)
        {
            return $"root={weak.IsAlive}; count={sCount}; heap={GC.GetTotalMemory(false)}; {error}";
        }
    }
}
