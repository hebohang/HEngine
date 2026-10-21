using System.Numerics;
using System.Runtime.CompilerServices;
using System.Text;
using HEngine.Scripting.Interop;

namespace HEngine;

// PC transport. Wasm supplies IEngineApi using copies across separate memories.
// Every pointer below is borrowed for one synchronous call on the owning thread.
/// <summary>PC host transport. Captures its engine thread and rejects calls after generation detach.</summary>
/// <param name="entry">Borrowed native ABI entry valid for the attached host lifetime.</param>
public unsafe sealed class NativeEngineApi(nint entry) : EngineApi
{
    private readonly delegate* unmanaged[Cdecl]<Request*, int> mEntry = (delegate* unmanaged[Cdecl]<Request*, int>)entry;
    private readonly int mThread = Environment.CurrentManagedThreadId;

    /// <inheritdoc/>
    protected override string Call(ref Request request, string name = "", string input = "", int outputCapacity = 4096)
    {
        if (!Engine.IsAttached(this)) throw new ObjectDisposedException(nameof(NativeEngineApi), "The script host generation has detached.");
        if (Engine.IsValidating) throw new InvalidOperationException("Runtime engine APIs are unavailable during Reset/OnValidate.");
        if (mEntry == null || Environment.CurrentManagedThreadId != mThread)
            throw new InvalidOperationException("Engine API calls must use the attached engine thread.");
        byte[] nameBytes = Encoding.UTF8.GetBytes(name), inputBytes = Encoding.UTF8.GetBytes(input);
        byte[] output = System.Buffers.ArrayPool<byte>.Shared.Rent(outputCapacity);
        try
        {
            fixed (byte* namePtr = nameBytes, inputPtr = inputBytes, outputPtr = output)
            fixed (Request* pointer = &request)
            {
                request.Name = (nint)namePtr; request.NameLength = nameBytes.Length;
                request.Input = (nint)inputPtr; request.InputLength = inputBytes.Length;
                request.Output = (nint)outputPtr; request.OutputCapacity = outputCapacity;
                int status = mEntry(pointer);
                if (request.OutputLength < 0 || request.OutputLength > outputCapacity)
                    throw new InvalidOperationException("Engine API returned an invalid UTF-8 buffer length.");
                string text = Encoding.UTF8.GetString(output, 0, request.OutputLength);
                if (status != 0) throw new InvalidOperationException($"Engine API {(EngineOperation)request.Operation} failed ({status}): {text}");
                return text;
            }
        }
        finally { System.Buffers.ArrayPool<byte>.Shared.Return(output); }
    }

}
