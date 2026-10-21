export function createScriptTransport(engine) {
    const capacity = 1024 * 1024;
    const dataPointer = engine._malloc(18 * 8);
    const namePointer = engine._malloc(4096);
    const inputPointer = engine._malloc(capacity);
    const outputPointer = engine._malloc(capacity);
    if (![dataPointer, namePointer, inputPointer, outputPointer].every(Boolean)) throw new Error('Engine script bridge allocation failed.');
    const values = new Float64Array(18), encoder = new TextEncoder(), decoder = new TextDecoder('utf-8', {fatal: true});
    let active = false, disposed = false;
    const metrics = {calls: 0, milliseconds: 0, maxMilliseconds: 0, operations: {}};
    return {
        metrics,
        call(operation, entity, flags, sequence, name, input, managedMemory) {
            if (active || disposed) throw new Error('Engine script bridge is reentrant or disposed.');
            active = true;
            const started = performance.now();
            try {
                const nameBytes = encoder.encode(name), inputBytes = encoder.encode(input);
                if (nameBytes.length > 4096 || inputBytes.length > capacity) throw new Error('Engine script input exceeded the ABI capacity.');
                // .NET's MemoryView and the engine's typed arrays refer to different
                // Wasm instances. These copies are intentional and synchronous.
                managedMemory.copyTo(values);
                engine.HEAPF64.set(values, dataPointer / 8);
                engine.HEAPU8.set(nameBytes, namePointer); engine.HEAPU8.set(inputBytes, inputPointer);
                const size = engine._he_script_call(operation, entity, flags, sequence,
                    namePointer, nameBytes.length, inputPointer, inputBytes.length, dataPointer, outputPointer, capacity);
                const length = size >= 0 ? size : -size - 1;
                if (length > capacity) throw new Error('Engine returned an invalid script output length.');
                const text = decoder.decode(engine.HEAPU8.subarray(outputPointer, outputPointer + length));
                if (size < 0) throw new Error(text || 'Engine rejected the script ABI request.');
                values.set(engine.HEAPF64.subarray(dataPointer / 8, dataPointer / 8 + 18));
                managedMemory.set(values);
                return text;
            } finally {
                const elapsed = performance.now() - started;
                metrics.calls++; metrics.milliseconds += elapsed; metrics.maxMilliseconds = Math.max(metrics.maxMilliseconds, elapsed);
                metrics.operations[operation] = (metrics.operations[operation] || 0) + 1;
                active = false;
            }
        },
        dispose() {
            if (active) throw new Error('Cannot dispose an active script transport.');
            if (disposed) return;
            disposed = true;
            for (const pointer of [dataPointer, namePointer, inputPointer, outputPointer]) engine._free(pointer);
        }
    };
}
