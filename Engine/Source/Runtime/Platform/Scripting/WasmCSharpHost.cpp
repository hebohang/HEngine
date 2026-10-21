#include "WasmCSharpHost.h"

#include <emscripten.h>

#include <cmath>

namespace HEngine
{
    // These pointers refer only to the engine module. Managed exports receive
    // copied text/numbers; no pointer is ever interpreted in .NET's memory.
    EM_JS(int, InvokeWasmManaged,
        (int textMode, int operation, double instance, double entity, int callback, double seconds,
         const char* name, int nameLength, const char* input, int inputLength, char* output, int capacity, double* result), {
        try {
            if (!Module.heManaged) throw new Error('The real .NET Wasm host was not attached before engine startup.');
            const decoder = Module.heScriptDecoder || (Module.heScriptDecoder = new TextDecoder('utf-8', {fatal:true}));
            const read = (p, n) => n ? decoder.decode(HEAPU8.subarray(p, p + n)) : "";
            const managed = Module.heManaged;
            let text = "";
            if (textMode) text = managed.Text(operation, instance, read(name, nameLength), read(input, inputLength));
            else HEAPF64[result / 8] = managed.Execute(operation, instance, entity, callback, seconds, read(name, nameLength));
            const failure = managed.LastError();
            if (failure) {
                stringToUTF8(failure, output, capacity);
                return -1;
            }
            const encoder = Module.heScriptEncoder || (Module.heScriptEncoder = new TextEncoder());
            const bytes = encoder.encode(text);
            if (bytes.length >= capacity) throw new Error('Managed Wasm output exceeds native capacity.');
            HEAPU8.set(bytes, output); HEAPU8[output + bytes.length] = 0;
            return bytes.length;
        } catch (error) {
            stringToUTF8(String(error.stack || error), output, capacity);
            return -1;
        }
    });

    int CallWasmCSharp(ScriptAbi::Request* pointer) noexcept
    {
        if (!pointer) return -1;
        auto& r = *pointer;
        if (r.Version != ScriptAbi::Version || r.NameLength < 0 || r.InputLength < 0 || r.OutputCapacity < 1 || !r.Output ||
            r.Instance > 9007199254740991ULL || r.Entity > 9007199254740991ULL) return -1;
        const auto op = static_cast<ScriptAbi::ManagedOperation>(r.Operation);
        const bool text = op == ScriptAbi::ManagedOperation::ReadFields || op == ScriptAbi::ManagedOperation::Describe || op == ScriptAbi::ManagedOperation::ReadField || op == ScriptAbi::ManagedOperation::WriteField;
        double result = 0;
        const auto length = InvokeWasmManaged(text, r.Operation, static_cast<double>(r.Instance), static_cast<double>(r.Entity), r.Callback, r.Seconds,
            reinterpret_cast<const char*>(r.Name), r.NameLength, reinterpret_cast<const char*>(r.Input), r.InputLength,
            reinterpret_cast<char*>(r.Output), r.OutputCapacity, &result);
        if (length < 0)
        {
            const auto buffer = reinterpret_cast<const char*>(r.Output);
            r.OutputLength = 0;
            while (r.OutputLength < r.OutputCapacity && buffer[r.OutputLength]) ++r.OutputLength;
            return -1;
        }
        if (!std::isfinite(result) || result < 0 || result > 9007199254740991.0 || std::trunc(result) != result) return -1;
        r.OutputLength = length; r.Result = static_cast<std::uint64_t>(result);
        return 0;
    }
}
