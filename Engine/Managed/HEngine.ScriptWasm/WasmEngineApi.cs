using System.Runtime.InteropServices.JavaScript;
using HEngine.Scripting.Interop;

namespace HEngine.Scripting;

// The borrowed MemoryView belongs to .NET's Wasm, not the C++ engine Wasm.
// JS copies its 16 numeric slots and flags/result into the engine's own memory.
internal unsafe partial class WasmEngineApi : EngineApi
{
    [JSImport("call", "HEngine.ScriptTransport")]
    private static partial string InvokeEngine(int operation, double entity, int flags, double sequence,
        string name, string input, [JSMarshalAs<JSType.MemoryView>] Span<double> data);

    protected override string Call(ref Request request, string name = "", string input = "", int outputCapacity = 4096)
    {
        if (!Engine.IsAttached(this)) throw new ObjectDisposedException(nameof(WasmEngineApi), "The script host generation has detached.");
        if (Engine.IsValidating) throw new InvalidOperationException("Runtime engine APIs are unavailable during Reset/OnValidate.");
        if (request.Version != 1 || request.Entity > 9_007_199_254_740_991UL || request.Result > 9_007_199_254_740_991UL)
            throw new InvalidOperationException("Wasm engine ABI version or exact-number handle range differs.");
        Span<double> data = stackalloc double[18];
        for (int i = 0; i < 16; i++) data[i] = request.Data[i];
        string text = InvokeEngine((int)request.Operation, request.Entity, checked((int)request.Flags), request.Result, name, input, data);
        for (int i = 0; i < 16; i++) request.Data[i] = (float)data[i];
        request.Flags = checked((uint)data[16]); request.Result = checked((ulong)data[17]);
        if (System.Text.Encoding.UTF8.GetByteCount(text) > outputCapacity)
            throw new InvalidOperationException("Wasm engine API output exceeded its caller-owned capacity.");
        return text;
    }
}
