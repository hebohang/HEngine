using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using HEngine.Scripting.Interop;

namespace HEngine.EditorScripting;

// Editor-only stable hostfxr entry. No gameplay types or delegates escape the
// collectible context except this explicitly named primitive ABI dispatcher.
public static unsafe class NativeEntry
{
    private delegate int Entry(nint request);
    private sealed class Context(string directory) : AssemblyLoadContext("HEngine scripts: " + directory, isCollectible: true)
    {
        protected override Assembly? Load(AssemblyName name)
        {
            string path = Path.Combine(directory, name.Name + ".dll");
            if (File.Exists(path)) return LoadFromAssemblyPath(path);
            // Only framework assemblies can fall back to the host. Never resolve
            // gameplay from a previous generation or a developer search path.
            if (name.Name is "System" or "netstandard" or "mscorlib" || name.Name!.StartsWith("System.", StringComparison.Ordinal)) return null;
            throw new FileNotFoundException("Undeclared script dependency: " + name.FullName, path);
        }
        protected override nint LoadUnmanagedDll(string name) => throw new NotSupportedException("Native script dependencies require an engine binding: " + name);
    }

    private static Context? sContext;
    private static Entry? sEntry;
    private static string sDirectory = Path.GetDirectoryName(typeof(NativeEntry).Assembly.Location)!;
    private static WeakReference? sRetired;
    private static int sThread;
    private static bool sBlocked;

    public static void Validate(string directory)
    {
        directory = Path.GetFullPath(directory);
        using var receipt = JsonDocument.Parse(File.ReadAllText(Path.Combine(directory, "build.json")));
        var build = receipt.RootElement;
        if (build.GetProperty("schema").GetInt32() != 1 || build.GetProperty("sdk").GetString() != "10.0.302" ||
            build.GetProperty("runtime").GetString() != "10.0.10" || build.GetProperty("shipping").GetBoolean() ||
            build.GetProperty("apiSchemaSha256").GetString() != ScriptApiIdentity.Schema)
            throw new InvalidDataException("Unsupported editor script build identity.");
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var file in build.GetProperty("managedFiles").EnumerateArray())
        {
            string name = file.GetProperty("path").GetString()!;
            if (Path.GetFileName(name) != name || name.Contains(':') || !names.Add(name))
                throw new InvalidDataException("Invalid or duplicate script manifest path: " + name);
            string path = Path.Combine(directory, name);
            if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0 ||
                Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(path))) != file.GetProperty("sha256").GetString())
                throw new InvalidDataException("Script manifest identity mismatch: " + name);
            if (name.EndsWith(".dll", StringComparison.OrdinalIgnoreCase))
            {
                var identity = AssemblyName.GetAssemblyName(path);
                if (identity.Name + ".dll" != name) throw new InvalidDataException("Assembly name differs from its manifest path: " + name);
            }
        }
        foreach (string name in new[] { "GameScripts.dll", "GameScripts.metadata.json", "HEngine.ScriptCore.dll", "HEngine.ScriptRuntime.dll" })
            if (!names.Contains(name)) throw new InvalidDataException("Missing script manifest entry: " + name);
        foreach (string path in Directory.EnumerateFiles(directory, "*.dll"))
            if (!names.Contains(Path.GetFileName(path))) throw new InvalidDataException("Unregistered dependency: " + path);
        using var metadata = JsonDocument.Parse(File.ReadAllText(Path.Combine(directory, "GameScripts.metadata.json")));
        if (metadata.RootElement.GetProperty("version").GetInt32() != 1 ||
            metadata.RootElement.GetProperty("identity").GetString() != build.GetProperty("gameScriptsSha256").GetString() ||
            metadata.RootElement.GetProperty("gameplaySourceIdentity").GetString() != build.GetProperty("gameplaySourceIdentity").GetString())
            throw new InvalidDataException("Script registration/source identity mismatch.");
        if (Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(Path.Combine(directory, "GameScripts.dll")))) != build.GetProperty("gameScriptsSha256").GetString())
            throw new InvalidDataException("Gameplay assembly differs from its generated registration identity.");
        if (build.TryGetProperty("modules", out var modules))
        {
            var registered = metadata.RootElement.GetProperty("modules");
            if (modules.GetArrayLength() != registered.GetArrayLength()) throw new InvalidDataException("Module registration closure differs.");
            for (int index = 0; index < modules.GetArrayLength(); ++index)
                foreach (string key in new[] { "id", "assembly", "identity", "sha256", "category" })
                    if (modules[index].GetProperty(key).GetString() != registered[index].GetProperty(key).GetString())
                        throw new InvalidDataException("Module registration identity mismatch: " + key);
        }
    }

    private static void Load()
    {
        if (sBlocked) throw new InvalidOperationException("Script reload is blocked: the retired context is still alive. Restart the editor. Known host roots: 0 dispatch delegates, 0 active contexts; check unmanaged Thread/Task, timers and external event subscriptions.");
        if (sEntry is not null) return;
        Validate(sDirectory);
        var context = new Context(sDirectory);
        try
        {
            var assembly = context.LoadFromAssemblyPath(Path.Combine(sDirectory, "HEngine.ScriptRuntime.dll"));
            var method = assembly.GetType("HEngine.Scripting.NativeEntry", throwOnError: true)!
                .GetMethod("DispatchManaged", BindingFlags.Public | BindingFlags.Static) ?? throw new MissingMethodException("Script dispatcher ABI missing");
            sEntry = method.CreateDelegate<Entry>();
            sContext = context;
        }
        catch { context.Unload(); throw; }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static WeakReference ProbeGeneration(string directory)
    {
        var context = new Context(directory);
        var weak = new WeakReference(context);
        try
        {
            var assembly = context.LoadFromAssemblyPath(Path.Combine(directory, "HEngine.ScriptRuntime.dll"));
            var entry = assembly.GetType("HEngine.Scripting.NativeEntry", throwOnError: true)!
                .GetMethod("DispatchManaged", BindingFlags.Public | BindingFlags.Static)!.CreateDelegate<Entry>();
            byte[] output = new byte[4 * 1024 * 1024];
            fixed (byte* buffer = output)
            {
                Request request = new() { Version = 1, Operation = (uint)ManagedOperation.Describe,
                    Output = (nint)buffer, OutputCapacity = output.Length };
                int status = entry((nint)(&request));
                string description = Encoding.UTF8.GetString(output, 0, request.OutputLength);
                if (status != 0) throw new InvalidDataException("Candidate registration cannot load: " + description);
                if (!JsonNode.DeepEquals(JsonNode.Parse(description), JsonNode.Parse(File.ReadAllText(Path.Combine(directory, "GameScripts.metadata.json")))))
                    throw new InvalidDataException("Embedded registration differs from the candidate metadata.");
            }
        }
        finally { context.Unload(); }
        return weak;
    }

    private static void Preflight(string directory)
    {
        var weak = ProbeGeneration(directory);
        for (int attempt = 0; attempt < 10 && weak.IsAlive; ++attempt)
        {
            GC.Collect(); GC.WaitForPendingFinalizers(); GC.Collect();
        }
        if (weak.IsAlive) throw new InvalidOperationException("Candidate context cannot be reclaimed after registration preflight; active generation retained.");
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static WeakReference Retire()
    {
        var context = sContext ?? throw new InvalidOperationException("No script context to retire.");
        var weak = new WeakReference(context, trackResurrection: false);
        sEntry = null;
        sContext = null;
        context.Unload();
        return weak;
    }

    private static void ClearJsonReflectionCaches()
    {
        // JSON's shared default-options and member-accessor caches retain types
        // from collectible script generations. Use its registered hot-reload
        // cache invalidation contract before verifying unload (Editor host only).
        bool cleared = false;
        foreach (var handler in typeof(JsonSerializer).Assembly.GetCustomAttributes<System.Reflection.Metadata.MetadataUpdateHandlerAttribute>())
        {
            var clear = handler.HandlerType.GetMethod("ClearCache", BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic,
                binder: null, types: [typeof(Type[])], modifiers: null);
            if (clear is null) continue;
            clear.Invoke(null, [null]);
            cleared = true;
        }
        if (!cleared) throw new NotSupportedException("The pinned JSON runtime has no hot-reload cache invalidation handler.");
    }

    private static void CollectRetired()
    {
        for (int attempt = 0; attempt < 10 && sRetired?.IsAlive == true; ++attempt)
        {
            GC.Collect(); GC.WaitForPendingFinalizers(); GC.Collect();
        }
        sBlocked = sRetired?.IsAlive == true;
        if (sBlocked) Load(); // Emits a stable diagnostic and never calls old code.
        sRetired = null;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int StopGeneration(nint address)
    {
        var request = (Request*)address;
        uint operation = request->Operation;
        request->Operation = (uint)ManagedOperation.Shutdown;
        try { return sEntry!(address); }
        finally { request->Operation = operation; }
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
            if (sizeof(Request) != 160 || request->Version != 1) throw new InvalidOperationException("Script host ABI mismatch.");
            if (sThread == 0) sThread = Environment.CurrentManagedThreadId;
            if (sThread != Environment.CurrentManagedThreadId) throw new InvalidOperationException("Script host requires its engine thread.");
            if ((ManagedOperation)request->Operation == ManagedOperation.Reload)
            {
                if (request->InputLength < 0 || request->InputLength > 32768 || (request->InputLength > 0 && request->Input == 0))
                    throw new ArgumentException("Invalid reload path buffer.");
                string directory = request->InputLength == 0 ? sDirectory : new UTF8Encoding(false, true).GetString(new ReadOnlySpan<byte>((void*)request->Input, request->InputLength));
                directory = Path.GetFullPath(directory);
                Validate(directory); // Failure must leave the active generation intact.
                Preflight(directory);
                if (sEntry is not null)
                {
                    int status = StopGeneration(address);
                    if (status != 0) return status; // Live roots: retain the current dispatcher.
                    sRetired = Retire();
                }
                ClearJsonReflectionCaches();
                CollectRetired();
                sDirectory = directory;
                Load();
                return 0;
            }
            Load();
            return sEntry!(address);
        }
        catch (Exception error)
        {
            byte[] text = JsonSerializer.SerializeToUtf8Bytes(new { scriptError = 1, message = error.Message, stack = error.ToString() });
            if (request->Output != 0 && request->OutputCapacity >= text.Length)
            {
                text.CopyTo(new Span<byte>((void*)request->Output, request->OutputCapacity));
                request->OutputLength = text.Length;
            }
            return -1;
        }
    }
}
