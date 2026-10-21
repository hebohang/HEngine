using System.Buffers;
using System.Runtime.CompilerServices;
using System.Text;
using System.Text.Json;
using HEngine.Generated;
using HEngine.Scripting.Interop;

namespace HEngine.Scripting;

public static class ScriptRuntime
{
    private sealed record Instance(ScriptBehaviour Script, ScriptRegistration Registration);
    private static readonly Dictionary<string, ScriptRegistration> sTypes = GameScriptRegistry.Create().ToDictionary(t => t.Id, StringComparer.Ordinal);
    private static readonly Dictionary<ulong, Instance> sInstances = new();
    private static ulong sNext = 1;
    private static int sThread;
    private static readonly List<ScriptBehaviour> sRetiring = [];
    public static string Metadata => GameScriptRegistry.Metadata;
    public static int InstanceCount => sInstances.Count;

    public static void Initialize(IEngineApi api)
    {
        if (sInstances.Count != 0) throw new InvalidOperationException("Cannot reattach the scripting host with live instances.");
        sThread = Environment.CurrentManagedThreadId;
        Engine.Attach(api);
    }
    private static void CheckThread()
    {
        if (sThread == 0 || sThread != Environment.CurrentManagedThreadId)
            throw new InvalidOperationException("Script operations require the attached engine thread.");
    }
    public static ulong Create(string type, ulong entity)
    {
        CheckThread();
        if (sInstances.Count >= 100_000 || sNext >= 9_007_199_254_740_991UL) throw new InvalidOperationException("Script instance limit reached.");
        if (!Engine.Api.IsValid(entity)) throw new InvalidOperationException("Cannot bind a script to a stale entity.");
        if (!sTypes.TryGetValue(type, out var registration)) throw new TypeLoadException($"Script type not registered: {type}");
        ScriptBehaviour script = registration.Create();
        // Compile the virtual per-frame callbacks while the scene is starting.
        // They are not invoked here, so gameplay state and scheduled work stay
        // untouched. This avoids a first-input frame paying for the script JIT.
        if (RuntimeFeature.IsDynamicCodeSupported)
        {
            var scriptType = script.GetType();
            var start = scriptType.GetMethod(nameof(ScriptBehaviour.Start), Type.EmptyTypes);
            if (start?.DeclaringType == scriptType)
                RuntimeHelpers.PrepareMethod(start.MethodHandle);
            foreach (var callback in new[] {
                nameof(ScriptBehaviour.Update),
                nameof(ScriptBehaviour.FixedUpdate),
                nameof(ScriptBehaviour.LateUpdate) })
            {
                var method = scriptType.GetMethod(callback, [typeof(double)]);
                if (method?.DeclaringType == scriptType)
                    RuntimeHelpers.PrepareMethod(method.MethodHandle);
            }
            RuntimeHelpers.PrepareMethod(typeof(ScriptRuntime).GetMethod(nameof(BeginFrame))!.MethodHandle);
            RuntimeHelpers.PrepareMethod(typeof(Time).GetMethod(nameof(Time.BeginFrame))!.MethodHandle);
            RuntimeHelpers.PrepareMethod(typeof(ScriptBehaviour).GetMethod(nameof(ScriptBehaviour.AdvanceScheduled))!.MethodHandle);
        }
        script.Bind(new Entity(entity));
        ulong handle = sNext++;
        sInstances.Add(handle, new(script, registration));
        return handle;
    }
    public static void BeginFrame(double seconds, bool paused) { CheckThread(); Time.BeginFrame(seconds, paused); }
    public static void WriteField(ulong handle, string field, string json)
    {
        CheckThread();
        var instance = sInstances[handle];
        var registration = instance.Registration.Fields.FirstOrDefault(f => f.Id == field) ?? throw new MissingFieldException(instance.Registration.Id, field);
        if (json.Length > 1024 * 1024) throw new InvalidDataException("Script field payload exceeds 1 MiB.");
        using var document = JsonDocument.Parse(json, new JsonDocumentOptions { MaxDepth = 32 });
        registration.Write(instance.Script, document.RootElement);
    }
    public static string ReadField(ulong handle, string field)
    {
        CheckThread();
        var instance = sInstances[handle];
        return (instance.Registration.Fields.FirstOrDefault(f => f.Id == field) ?? throw new MissingFieldException(instance.Registration.Id, field)).Read(instance.Script);
    }
    public static string ReadFields(ulong handle)
    {
        CheckThread();
        return Snapshot(sInstances[handle]);
    }
    private static string Snapshot(Instance instance)
    {
        var buffer = new ArrayBufferWriter<byte>();
        using (var writer = new Utf8JsonWriter(buffer))
        {
            writer.WriteStartObject();
            writer.WriteNumber("schema", 2);
            writer.WriteStartObject("fields");
            foreach (var field in instance.Registration.Fields)
            {
                writer.WritePropertyName(field.Id);
                writer.WriteRawValue(field.Read(instance.Script));
                if (writer.BytesCommitted + writer.BytesPending > 1024 * 1024)
                    throw new InvalidDataException("Script field snapshot exceeds 1 MiB.");
            }
            writer.WriteEndObject();
            writer.WriteEndObject();
        }
        if (buffer.WrittenCount > 1024 * 1024)
            throw new InvalidDataException("Script field snapshot exceeds 1 MiB.");
        return Encoding.UTF8.GetString(buffer.WrittenSpan);
    }
#if HE_SCRIPT_EDITOR
    public static string EditFields(string type, string fields, uint callback)
    {
        CheckThread();
        if (sInstances.Count != 0) throw new InvalidOperationException("Edit validation requires a stopped scene.");
        if (fields.Length > 1024 * 1024) throw new InvalidDataException("Editor fields exceed 1 MiB.");
        if (callback != (uint)ScriptLifecycle.Reset && callback != (uint)ScriptLifecycle.OnValidate)
            throw new ArgumentOutOfRangeException(nameof(callback));
        using var scope = Engine.EnterEditValidation();
        var registration = sTypes[type];
        var script = registration.Create();
        try
        {
            using var document = JsonDocument.Parse(fields, new JsonDocumentOptions { MaxDepth = 32 });
            if (document.RootElement.GetProperty("schema").GetInt32() is not (1 or 2)) throw new InvalidDataException("Unknown editor field schema.");
            var values = document.RootElement.GetProperty("fields");
            foreach (var field in registration.Fields)
                if (values.TryGetProperty(field.Id, out var value)) field.Write(script, value);
            if (callback == (uint)ScriptLifecycle.Reset) script.Reset();
            script.OnValidate();
            return Snapshot(new Instance(script, registration));
        }
        finally
        {
            var diagnostics = script.ReleaseScheduled();
            if (diagnostics.Length != 0) { sRetiring.Add(script); throw new InvalidOperationException(string.Join("; ", diagnostics)); }
        }
    }
#endif
    public static void Contact(ulong handle, in ScriptContact contact, bool trigger)
    {
        CheckThread();
        var script = sInstances[handle].Script;
        ulong previous = Engine.ExchangeEntity(script.Entity.Handle);
        try { ScriptPhysics.Dispatch(script, contact, trigger); }
        finally { Engine.ExchangeEntity(previous); }
    }

    public static void Invoke(ulong handle, uint callback, double seconds)
    {
        CheckThread();
        var script = sInstances[handle].Script;
        ulong previous = Engine.ExchangeEntity(script.Entity.Handle);
        try
        {
            switch ((ScriptLifecycle)callback)
            {
                case ScriptLifecycle.Initialize: script.Initialize(); break;
                case ScriptLifecycle.Enable: script.Enable(); break;
                case ScriptLifecycle.Update: script.Update(seconds); script.AdvanceScheduled(false, seconds); break;
                case ScriptLifecycle.Disable: script.Disable(); break;
                case ScriptLifecycle.Destroy: script.Destroy(); break;
                case ScriptLifecycle.Start: script.Start(); break;
                case ScriptLifecycle.FixedUpdate: script.FixedUpdate(seconds); script.AdvanceScheduled(true, seconds); break;
                case ScriptLifecycle.LateUpdate: script.LateUpdate(seconds); break;
                case ScriptLifecycle.Reset: script.Reset(); break;
                case ScriptLifecycle.OnValidate: script.OnValidate(); break;
                case ScriptLifecycle.Pause: Time.SetPaused(true); script.Pause(); break;
                case ScriptLifecycle.Resume: Time.SetPaused(false); script.Resume(); break;
                case ScriptLifecycle.ApplicationQuit: script.ApplicationQuit(); break;
                case ScriptLifecycle.SceneLoaded: script.SceneLoaded(); break;
                case ScriptLifecycle.SceneUnloading: script.SceneUnloading(); break;
                default: throw new ArgumentOutOfRangeException(nameof(callback));
            }
        }
        finally { Engine.ExchangeEntity(previous); }
    }
    public static void Release(ulong handle)
    {
        CheckThread();
        if (!sInstances.Remove(handle, out var instance)) return;
        var diagnostics = instance.Script.ReleaseScheduled();
        if (diagnostics.Length != 0)
        {
            sRetiring.Add(instance.Script);
            // Releasing a native root must succeed even when the log sink is unavailable.
            // Residual task diagnostics remain retained for the unload gate below.
            foreach (string diagnostic in diagnostics)
                try { Engine.Log(diagnostic); } catch { }
        }
    }
    public static void Shutdown()
    {
        CheckThread();
        if (sInstances.Count != 0) throw new InvalidOperationException($"Cannot stop C# plugin: {sInstances.Count} script roots remain.");
        var pending = sRetiring.SelectMany(script => script.PendingTasksForShutdown()).Distinct().ToArray();
        if (pending.Length != 0)
        {
            // One budget for the whole generation, not a delay multiplied by instance count.
            // Tasks requiring the engine thread cannot deadlock this bounded wait.
            try { Task.WaitAll(pending, TimeSpan.FromMilliseconds(250)); }
            catch (AggregateException) { } // ReleaseScheduled below retains structured origin diagnostics.
        }
        var roots = new List<string>();
        foreach (var script in sRetiring.ToArray())
        {
            var diagnostics = script.ReleaseScheduled();
            if (diagnostics.Length == 0) sRetiring.Remove(script);
            else roots.AddRange(diagnostics);
        }
        if (roots.Count != 0) throw new InvalidOperationException("Cannot unload C#: task cancellation timeout (250 ms); " + string.Join("; ", roots));
        Engine.Detach(); sThread = 0;
    }
}
