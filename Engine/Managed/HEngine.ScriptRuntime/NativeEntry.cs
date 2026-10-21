using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using HEngine.Scripting.Interop;

namespace HEngine.Scripting;

public static unsafe class NativeEntry
{
    private static readonly UTF8Encoding sUtf8 = new(false, true);
    private static string Read(nint data, int size)
    {
        if (size < 0 || size > 1024 * 1024 || (size > 0 && data == 0)) throw new ArgumentException("Invalid borrowed UTF-8 input buffer.");
        return sUtf8.GetString(new ReadOnlySpan<byte>((void*)data, size));
    }
    private static void Write(Request* request, string text)
    {
        int size = sUtf8.GetByteCount(text);
        if (request->OutputCapacity < size || request->Output == 0) throw new ArgumentException($"Script output buffer requires {size} bytes.");
        request->OutputLength = sUtf8.GetBytes(text, new Span<byte>((void*)request->Output, request->OutputCapacity));
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static int Dispatch(Request* request) => DispatchManaged((nint)request);

    public static int DispatchManaged(nint address)
    {
        var request = (Request*)address;
        if (request == null) return -1;
        request->OutputLength = 0;
        try
        {
            if (sizeof(Request) != 160 || request->Version != 1) throw new InvalidOperationException("C# native ABI version or layout mismatch.");
            switch ((ManagedOperation)request->Operation)
            {
                case ManagedOperation.Initialize: ScriptRuntime.Initialize(new NativeEngineApi(request->HostEntry)); break;
                case ManagedOperation.Describe: Write(request, ScriptRuntime.Metadata); break;
                case ManagedOperation.Create: request->Result = ScriptRuntime.Create(Read(request->Name, request->NameLength), request->Entity); break;
                case ManagedOperation.WriteField: ScriptRuntime.WriteField(request->Instance, Read(request->Name, request->NameLength), Read(request->Input, request->InputLength)); break;
                case ManagedOperation.ReadField: Write(request, ScriptRuntime.ReadField(request->Instance, Read(request->Name, request->NameLength))); break;
                case ManagedOperation.Invoke: ScriptRuntime.Invoke(request->Instance, request->Callback, request->Seconds); break;
                case ManagedOperation.PhysicsContact:
                    ScriptRuntime.Contact(request->Instance, new(new Entity(request->Entity), (ScriptContactPhase)request->Callback,
                        new(request->Data[0], request->Data[1], request->Data[2]),
                        new(request->Data[3], request->Data[4], request->Data[5])), request->Flags != 0);
                    break;
                case ManagedOperation.Release: ScriptRuntime.Release(request->Instance); break;
                case ManagedOperation.Shutdown: ScriptRuntime.Shutdown(); break;
                case ManagedOperation.ReadFields: Write(request, ScriptRuntime.ReadFields(request->Instance)); break;
                case ManagedOperation.BeginFrame: ScriptRuntime.BeginFrame(request->Seconds, request->Flags != 0); break;
#if HE_SCRIPT_EDITOR
                case ManagedOperation.EditFields: Write(request, ScriptRuntime.EditFields(Read(request->Name, request->NameLength), Read(request->Input, request->InputLength), request->Callback)); break;
#endif
                case ManagedOperation.Collect:
                    GC.Collect(); GC.WaitForPendingFinalizers(); GC.Collect();
                    request->Result = (ulong)GC.GetTotalMemory(false); break;
                default: throw new InvalidOperationException("Unknown managed operation.");
            }
            return 0;
        }
        catch (Exception error)
        {
            // Never let a CLR exception unwind through hostfxr's native ABI.
            try { Write(request, ScriptDiagnostic.Encode(error)); }
            catch { request->OutputLength = 0; }
            return -1;
        }
    }
}
