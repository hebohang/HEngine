import { dotnet } from './_framework/dotnet.js';
try {
    const runtime = await dotnet.create();
    const config = runtime.getConfig();
    const exports = await runtime.getAssemblyExports(config.mainAssemblyName);
    const response = await fetch('./python-reference.json');
    if (!response.ok) throw new Error('Missing Python reference fixtures');
    const result = exports.Program.Run(await response.text());
    document.querySelector('#result').textContent = result;
    globalThis.survivorParity = { passed: true, result };
} catch (error) {
    document.querySelector('#result').textContent = String(error);
    globalThis.survivorParity = { passed: false, error: String(error) };
    console.error(error);
}
