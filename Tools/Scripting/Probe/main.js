import { dotnet } from './_framework/dotnet.js';

const started = performance.now();
try {
    const runtime = await dotnet.withDiagnosticTracing(false).create();
    const exports = await runtime.getAssemblyExports(runtime.getConfig().mainAssemblyName);
    const probe = exports.Probe;
    const result = {
        identity: probe.Identity(), first: probe.Step(7), second: probe.Step(11),
        runtime: probe.ExerciseRuntime(), startupMs: performance.now() - started
    };
    if (result.first !== 7 || result.second !== 18 || !result.runtime.includes('root=True'))
        throw new Error('C# probe invariant failed: ' + JSON.stringify(result));
    globalThis.heCSharpProbe = { result, step: delta => probe.Step(delta) };
    console.log('[he-csharp-probe]', JSON.stringify(result));
    document.getElementById('result').textContent = JSON.stringify(result, null, 2);
    document.getElementById('step').onclick = () => {
        document.getElementById('count').textContent = String(probe.Step(7));
    };
} catch (error) {
    globalThis.heCSharpProbe = { error: String(error.stack || error) };
    console.error('[he-csharp-probe-failed]', error);
    document.getElementById('result').textContent = String(error.stack || error);
    throw error;
}
