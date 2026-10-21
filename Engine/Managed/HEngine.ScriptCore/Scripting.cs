using System.Numerics;

namespace HEngine;

/// <summary>Registers a behaviour under a persistent project-wide type ID.</summary>
[AttributeUsage(AttributeTargets.Class, Inherited = false)]
public sealed class ScriptTypeAttribute(string id) : Attribute
{
    /// <summary>Stable scene identity; changing it requires explicit migration.</summary>
    public string Id { get; } = id;
}

/// <summary>Declares a bounded value structure with stable identity and explicit ScriptField members.</summary>
[AttributeUsage(AttributeTargets.Struct, Inherited = false)]
public sealed class ScriptStructAttribute(string id) : Attribute
{
    /// <summary>Stable identity of the bounded value structure.</summary>
    public string Id { get; } = id;
}

/// <summary>Sets a type's lifecycle phase order. Equal values use stable type IDs.</summary>
[AttributeUsage(AttributeTargets.Class, Inherited = false)]
public sealed class ScriptExecutionOrderAttribute(int order) : Attribute
{
    /// <summary>Default priority when dependency constraints do not order two types.</summary>
    public int Order { get; } = order;
    /// <summary>Stable script type IDs that must execute after this type; missing IDs fail the build.</summary>
    public string[] Before { get; set; } = [];
    /// <summary>Stable script type IDs that must execute before this type; cycles fail the build.</summary>
    public string[] After { get; set; } = [];
}

/// <summary>Declares a dynamically selected stable resource so the cooker includes it.</summary>
[AttributeUsage(AttributeTargets.Class, AllowMultiple = true, Inherited = true)]
public sealed class ScriptDependencyAttribute(string path) : Attribute
{
    /// <summary>Project-relative resource identity; absolute paths are forbidden.</summary>
    public string Path { get; } = path;
}

/// <summary>Persists a supported public value field through explicit generated bindings.</summary>
[AttributeUsage(AttributeTargets.Field, Inherited = true)]
public sealed class ScriptFieldAttribute(string id, params string[] formerIds) : Attribute
{
    /// <summary>Stable field identity, independent of the C# member name.</summary>
    public string Id { get; } = id;
    /// <summary>Prior IDs read in declared order when the current ID is absent.</summary>
    public string[] FormerIds { get; } = formerIds;
    /// <summary>Inspector label; empty uses the member name.</summary>
    public string DisplayName { get; set; } = "";
    /// <summary>Help displayed when hovering the label.</summary>
    public string Tooltip { get; set; } = "";
    /// <summary>Section caption preceding this field.</summary>
    public string Header { get; set; } = "";
    /// <summary>Additional DPI-scaled logical spacing.</summary>
    public double Space { get; set; }
    /// <summary>Disables Inspector edits while retaining persistence.</summary>
    public bool ReadOnly { get; set; }
    /// <summary>Hides the field without removing persisted values.</summary>
    public bool HideInInspector { get; set; }
    /// <summary>Uses a multiline string editor.</summary>
    public bool Multiline { get; set; }
    /// <summary>Draws Vector4 as an RGBA color.</summary>
    public bool Color { get; set; }
    /// <summary>Uses a numeric slider when the range is representable.</summary>
    public bool HasRange { get; set; }
    /// <summary>Minimum Inspector numeric value, enforced on commit.</summary>
    public double Min { get; set; } = double.MinValue;
    /// <summary>Maximum Inspector numeric value, enforced on commit.</summary>
    public double Max { get; set; } = double.MaxValue;
    /// <summary>Required asset category such as Texture or Prefab; empty accepts any supported resource.</summary>
    public string ResourceType { get; set; } = "";
    /// <summary>Whether an entity/resource reference may be empty.</summary>
    public bool AllowNull { get; set; } = true;
}

/// <summary>Stable project-relative asset identity, never a loaded object or native pointer.</summary>
public readonly record struct ResourceReference(string Id);
/// <summary>Persisted scene UUID, distinct from a runtime Entity handle.</summary>
public readonly record struct EntityReference(string Id)
{
    /// <summary>Resolves in the executing script's scene; returns an invalid Entity when absent.</summary>
    public Entity Resolve() => Engine.Api.FindEntity(Id);
}

/// <summary>Value snapshot that batches Transform/Sprite reads and writes.</summary>
public struct EntityState
{
    /// <summary>Local translation in engine units.</summary>
    public Vector3 Position;
    /// <summary>Local Euler rotation in radians.</summary>
    public Vector3 Rotation;
    /// <summary>Local nonuniform scale.</summary>
    public Vector3 Scale;
    /// <summary>Sprite RGBA tint when HasSprite is true.</summary>
    public Vector4 Color;
    /// <summary>Whether the snapshot includes a Sprite component.</summary>
    public bool HasSprite;
}

/// <summary>Frame input: movement axes, pointer position, down state and monotonic press sequence.</summary>
public readonly record struct InputState(Vector2 Movement, Vector2 Pointer, bool PointerDown, ulong PressSequence);

/// <summary>Explicit host ABI facade. Prefer Entity/component views and typed data APIs in gameplay.</summary>
public interface IEngineApi
{
    /// <summary>Queries a generation-checked handle; stale handles return false.</summary>
    bool IsValid(ulong entity);
    /// <summary>Reads Transform/Sprite in one call; stale entities throw InvalidEngineObjectException.</summary>
    EntityState ReadEntity(ulong entity);
    /// <summary>Writes selected snapshot components; nonfinite data, missing components and stale handles throw.</summary>
    void WriteEntity(ulong entity, in EntityState state, bool transform, bool sprite);
    /// <summary>Reads input for the current scene and viewport.</summary>
    InputState ReadInput();
    /// <summary>Reads a named action from the desktop input map.</summary>
    ActionState ReadAction(string name) => throw new NotSupportedException("Input actions require a supporting host.");
    /// <summary>Switches the active input context and suppresses held controls until release.</summary>
    void SetInputContext(string context) => throw new NotSupportedException("Input actions require a supporting host.");
    /// <summary>Replaces a binding, or appends when index equals the current binding count.</summary>
    void RebindInput(string action, int index, ActionBinding binding) => throw new NotSupportedException("Input actions require a supporting host.");
    /// <summary>Exports validated bindings for persistence using the save API.</summary>
    string ReadInputMap() => throw new NotSupportedException("Input actions require a supporting host.");
    /// <summary>Validates and atomically replaces bindings; malformed maps leave live bindings unchanged.</summary>
    void WriteInputMap(string json) => throw new NotSupportedException("Input actions require a supporting host.");
    /// <summary>Invalidates immediately and destroys at the scene command barrier.</summary>
    void DestroyEntity(ulong entity);
    /// <summary>Resolves a stable UUID in the current scene; missing entities return an invalid handle.</summary>
    Entity FindEntity(string stableId);
    /// <summary>Changes ScriptComponent enable state; lifecycle synchronization occurs at the next update.</summary>
    void SetEnabled(ulong entity, bool enabled);
    /// <summary>Reads the Sprite's stable texture resource identity.</summary>
    string ReadTexture(ulong entity);
    /// <summary>Sets a validated stable texture resource; invalid/missing resources throw.</summary>
    void WriteTexture(ulong entity, string path);
    /// <summary>Writes an engine log entry on the owning thread.</summary>
    void Log(string message);
    /// <summary>Reads a persistent save slot, returning empty text if absent.</summary>
    string ReadSave(string key);
    /// <summary>Atomically writes a bounded persistent save; validation/I/O failures throw.</summary>
    void WriteSave(string key, string value);
    /// <summary>Publishes the latest scene-local mailbox payload.</summary>
    void EmitEvent(string name, string payload);
    /// <summary>Reads a newer mailbox sequence or returns empty text.</summary>
    string ReadEvent(string name, ulong afterSequence, out ulong sequence);
    /// <summary>Explicit optional platform service; unsupported services throw and must pass export auditing.</summary>
    string PlatformService(string name, string request) => throw new NotSupportedException(name);
    /// <summary>Generated numeric component binding: operation 0=query, 1=add, 2=remove; unknown IDs throw.</summary>
    ulong ComponentAccess(ulong entity, uint kind, uint operation) => throw new NotSupportedException("Component bindings unavailable.");
    /// <summary>Creates or duplicates an entity hierarchy in the executing scene; returns a new stable runtime handle.</summary>
    Entity CreateEntity(string name, Entity template) => throw new NotSupportedException("Entity creation unavailable.");
    /// <summary>Reads or writes parent hierarchy; cross-scene references and cycles throw.</summary>
    Entity Parent(ulong entity, Entity parent, bool write) => throw new NotSupportedException("Hierarchy bindings unavailable.");
    /// <summary>Returns a bounded children snapshot; avoid allocating one every frame.</summary>
    Entity[] Children(ulong entity) => throw new NotSupportedException("Hierarchy bindings unavailable.");
    /// <summary>Lease binding used by PrefabLoad; request IDs are distinct from entity handles.</summary>
    ulong PrefabRequest(ulong owner, ulong request, uint operation, string path) => throw new NotSupportedException("Asynchronous prefab bindings unavailable.");
}

/// <summary>Current engine-thread services. Runtime access is forbidden during Reset/OnValidate.</summary>
public static class Engine
{
    private static IEngineApi? sApi;
    /// <summary>Thread-local host callback context; never retain it across Play sessions. Native APIs still require the engine thread.</summary>
    [field: ThreadStatic]
    public static ulong CurrentEntity { get; private set; }
    /// <summary>Host dispatch only; restore the returned previous context after a callback.</summary>
    public static ulong ExchangeEntity(ulong entity) { ulong previous = CurrentEntity; CurrentEntity = entity; return previous; }
    /// <summary>True while a restricted Reset/OnValidate transaction is executing.</summary>
    public static bool IsValidating { get; private set; }
    /// <summary>Attached host facade; throws when detached or in restricted edit validation.</summary>
    public static IEngineApi Api => IsValidating ? throw new InvalidOperationException("Runtime engine APIs are unavailable during Reset/OnValidate.") :
        sApi ?? throw new InvalidOperationException("The engine scripting host is not attached.");
    /// <summary>Runtime editor binding only. Rejects recursive validation and all engine access until disposed.</summary>
    public static IDisposable EnterEditValidation()
    {
        if (IsValidating) throw new InvalidOperationException("Recursive script validation is forbidden.");
        IsValidating = true;
        return new ValidationScope();
    }
    private sealed class ValidationScope : IDisposable { public void Dispose() => IsValidating = false; }
    /// <summary>Host initialization only; installs the transport for this managed generation.</summary>
    public static void Attach(IEngineApi api) => sApi = api ?? throw new ArgumentNullException(nameof(api));
    /// <summary>Transport lifetime guard. A cached API from a detached generation must never call the native host.</summary>
    public static bool IsAttached(IEngineApi api) => ReferenceEquals(sApi, api);
    /// <summary>Host shutdown only; immediately invalidates access through the old transport.</summary>
    public static void Detach() { sApi = null; CurrentEntity = 0; }
    /// <summary>Current viewport input snapshot.</summary>
    public static InputState Input => Api.ReadInput();
    /// <summary>Logs an engine-thread message; exceptions are reported by the script boundary.</summary>
    public static void Log(string message) => Api.Log(message);
}

/// <summary>Generation-checked scene object identity. Destroyed entities are explicitly invalid, never fake null.</summary>
/// <example><code>var transform = Entity.GetComponent&lt;Transform&gt;(); var value = transform.Snapshot; value.Position.X += 1; transform.Snapshot = value;</code></example>
public readonly partial record struct Entity(ulong Handle)
{
    /// <summary>False for missing, retired or pending-destruction handles; a detached host throws.</summary>
    public bool IsValid => Engine.Api.IsValid(Handle);
    /// <summary>Batched Transform/Sprite value snapshot. Stale handles throw InvalidEngineObjectException.</summary>
    public EntityState State { get => Engine.Api.ReadEntity(Handle); set => Engine.Api.WriteEntity(Handle, value, true, value.HasSprite); }
    /// <summary>Applies one snapshot; unsupported components and invalid values throw.</summary>
    public void Apply(in EntityState state) => Engine.Api.WriteEntity(Handle, state, true, state.HasSprite);
    /// <summary>Invalidates the entity now; native removal occurs at the deferred command barrier.</summary>
    public void Destroy() => Engine.Api.DestroyEntity(Handle);
    /// <summary>Changes this entity's ScriptComponent enable state.</summary>
    public void SetScriptEnabled(bool value) => Engine.Api.SetEnabled(Handle, value);
    /// <summary>Stable Sprite texture identity; missing Sprite or stale entity throws.</summary>
    public ResourceReference Texture { get => new(Engine.Api.ReadTexture(Handle)); set => Engine.Api.WriteTexture(Handle, value.Id); }
}

/// <summary>Explicit lifecycle base. Field restoration precedes Initialize; failures are isolated per instance.</summary>
/// <remarks>Lifecycle ordering, cleanup barriers and edit restrictions are specified in Docs/ScriptingWorkflowUpgrade.md.</remarks>
public abstract partial class ScriptBehaviour
{
    /// <summary>Owning runtime entity, valid only in this scene session.</summary>
    public Entity Entity { get; private set; }
    /// <summary>Host construction only; binds the owner before restoring fields.</summary>
    public void Bind(Entity entity) => Entity = entity;
    /// <summary>Runs once after persisted fields have been restored.</summary>
    public virtual void Initialize() { }
    /// <summary>Runs when an initialized instance gains enabled status.</summary>
    public virtual void Enable() { }
    /// <summary>Ordinary enabled frame update; seconds is game delta time.</summary>
    public virtual void Update(double seconds) { }
    /// <summary>Runs when an enabled instance loses enabled status, including Stop.</summary>
    public virtual void Disable() { }
    /// <summary>Final callback before mandatory host release, even after earlier callback failures.</summary>
    public virtual void Destroy() { }
    /// <summary>Runs once before the first ordinary update of an enabled instance.</summary>
    public virtual void Start() { }
    /// <summary>Runs zero or more times per frame at the fixed simulation interval.</summary>
    public virtual void FixedUpdate(double seconds) { }
    /// <summary>Runs after all ordinary Update callbacks and before the destruction barrier.</summary>
    public virtual void LateUpdate(double seconds) { }
    /// <summary>Sets serialized defaults on initial type selection or explicit Reset; runtime APIs are forbidden.</summary>
    public virtual void Reset() { }
    /// <summary>Validates committed edit values once per transaction; recursive validation and runtime APIs are forbidden.</summary>
    public virtual void OnValidate() { }
    /// <summary>Receives transition into a paused scene.</summary>
    public virtual void Pause() { }
    /// <summary>Receives transition out of a paused scene.</summary>
    public virtual void Resume() { }
    /// <summary>Receives normal application shutdown before scene cleanup.</summary>
    public virtual void ApplicationQuit() { }
    /// <summary>Receives the initialized scene context before its first frame.</summary>
    public virtual void SceneLoaded() { }
    /// <summary>Receives scene shutdown before Disable/Destroy/Release.</summary>
    public virtual void SceneUnloading() { }
}
