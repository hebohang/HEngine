using System.Runtime.InteropServices.JavaScript;
using HEngine.Scripting.Interop;

[assembly: System.Runtime.Versioning.SupportedOSPlatform("browser")]

namespace HEngine.Scripting;

public static partial class WasmEntry
{
    private static string sError = "";
    public static void Main() { }

    private static ulong Handle(double value)
    {
        if (!double.IsFinite(value) || value < 0 || value > 9_007_199_254_740_991d || Math.Truncate(value) != value)
            throw new InvalidOperationException("Invalid exact-number Wasm handle.");
        return (ulong)value;
    }

    [JSExport]
    public static string LastError() => sError;

    // Cold metadata/fields travel as typed JSON; update/create/release and engine
    // component operations use numeric IDs. Exceptions never escape this boundary.
    [JSExport]
    public static string Text(int operation, double instance, string name, string input)
    {
        sError = "";
        try
        {
            if (name.Length > 4096 || input.Length > 1024 * 1024) throw new InvalidOperationException("Managed Wasm request exceeds its bound.");
            switch ((ManagedOperation)operation)
            {
                case ManagedOperation.Describe: return ScriptRuntime.Metadata;
                case ManagedOperation.ReadFields: return ScriptRuntime.ReadFields(Handle(instance));
                case ManagedOperation.ReadField: return ScriptRuntime.ReadField(Handle(instance), name);
                case ManagedOperation.WriteField: ScriptRuntime.WriteField(Handle(instance), name, input); return "";
                default: throw new InvalidOperationException("Unknown managed Wasm text operation.");
            }
        }
        catch (Exception error) { sError = ScriptDiagnostic.Encode(error); return ""; }
    }

    [JSExport]
    public static double Execute(int operation, double instance, double entity, int callback, double seconds, string type)
    {
        sError = "";
        try
        {
            switch ((ManagedOperation)operation)
            {
                case ManagedOperation.Initialize: ScriptRuntime.Initialize(new WasmEngineApi()); break;
                case ManagedOperation.Create: return ScriptRuntime.Create(type, Handle(entity));
                case ManagedOperation.Invoke: ScriptRuntime.Invoke(Handle(instance), checked((uint)callback), seconds); break;
                case ManagedOperation.Release: ScriptRuntime.Release(Handle(instance)); break;
                case ManagedOperation.Shutdown: ScriptRuntime.Shutdown(); break;
                case ManagedOperation.BeginFrame: ScriptRuntime.BeginFrame(seconds, callback != 0); break;
                case ManagedOperation.Collect: GC.Collect(); GC.WaitForPendingFinalizers(); GC.Collect(); return GC.GetTotalMemory(false);
                default: throw new InvalidOperationException("Unknown managed Wasm numeric operation.");
            }
            return 0;
        }
        catch (Exception error) { sError = ScriptDiagnostic.Encode(error); return -1; }
    }
}
