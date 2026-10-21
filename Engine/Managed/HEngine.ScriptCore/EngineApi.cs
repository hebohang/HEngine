using System.Numerics;
using HEngine.Scripting.Interop;

namespace HEngine;

/// <summary>Shared typed ABI semantics; platform transports copy requests across their own boundary.</summary>
public abstract unsafe class EngineApi : IEngineApi
{
    /// <summary>Creates a versioned request; entity zero is reserved for context-free services.</summary>
    protected static Request New(EngineOperation operation, ulong entity = 0) => new() { Version = 1, Operation = (uint)operation, Entity = entity };
    /// <summary>Synchronous borrowed-buffer transport. Failures throw; no pointers may escape the call.</summary>
    protected abstract string Call(ref Request request, string name = "", string input = "", int outputCapacity = 4096);

    // Probe only on failure: successful snapshots keep one boundary call and no
    // delegate allocation. Preserve unrelated transport/thread/validation errors.
    private string CallEntity(ref Request request, string name = "", int outputCapacity = 4096, ulong referencedEntity = 0)
    {
        try { return Call(ref request, name, outputCapacity: outputCapacity); }
        catch (Exception error) when (error is not InvalidEngineObjectException)
        {
            ulong invalid = 0;
            bool invalidFound = false;
            try
            {
                if (!IsValid(request.Entity)) { invalid = request.Entity; invalidFound = true; }
                else if (referencedEntity != 0 && !IsValid(referencedEntity)) { invalid = referencedEntity; invalidFound = true; }
            }
            catch { /* An unavailable host/thread must retain its original error. */ }
            if (invalidFound)
                throw new InvalidEngineObjectException(nameof(Entity), invalid, ((EngineOperation)request.Operation).ToString(), error);
            throw;
        }
    }

    /// <inheritdoc/>
    public bool IsValid(ulong entity) { var r = New(EngineOperation.EntityValid, entity); Call(ref r); return r.Result != 0; }
    /// <inheritdoc/>
    public EntityState ReadEntity(ulong entity)
    {
        var r = New(EngineOperation.ReadEntity, entity); CallEntity(ref r);
        return new() {
            Position = new(r.Data[0], r.Data[1], r.Data[2]), Rotation = new(r.Data[3], r.Data[4], r.Data[5]),
            Scale = new(r.Data[6], r.Data[7], r.Data[8]), Color = new(r.Data[9], r.Data[10], r.Data[11], r.Data[12]), HasSprite = (r.Flags & 2) != 0 };
    }
    /// <inheritdoc/>
    public void WriteEntity(ulong entity, in EntityState state, bool transform, bool sprite)
    {
        var r = New(EngineOperation.WriteEntity, entity); r.Flags = (transform ? 1u : 0u) | (sprite ? 2u : 0u);
        r.Data[0] = state.Position.X; r.Data[1] = state.Position.Y; r.Data[2] = state.Position.Z;
        r.Data[3] = state.Rotation.X; r.Data[4] = state.Rotation.Y; r.Data[5] = state.Rotation.Z;
        r.Data[6] = state.Scale.X; r.Data[7] = state.Scale.Y; r.Data[8] = state.Scale.Z;
        r.Data[9] = state.Color.X; r.Data[10] = state.Color.Y; r.Data[11] = state.Color.Z; r.Data[12] = state.Color.W;
        CallEntity(ref r);
    }
    /// <inheritdoc/>
    public InputState ReadInput()
    {
        var r = New(EngineOperation.ReadInput, Engine.CurrentEntity); Call(ref r);
        return new(new Vector2(r.Data[0], r.Data[1]), new Vector2(r.Data[2], r.Data[3]), (r.Flags & 1) != 0, r.Result);
    }
    /// <inheritdoc/>
    public ActionState ReadAction(string name)
    {
        var r = New(EngineOperation.InputAction, Engine.CurrentEntity); Call(ref r, name);
        return new(new(r.Data[0], r.Data[1]), (r.Result & 1) != 0, (r.Result & 2) != 0,
            (r.Result & 4) != 0, (r.Result & 8) != 0, (r.Result & 16) != 0,
            (int)r.Data[2]);
    }
    /// <inheritdoc/>
    public void SetInputContext(string context)
    {
        var r = New(EngineOperation.InputAction, Engine.CurrentEntity); r.Flags = 1; Call(ref r, context);
    }
    /// <inheritdoc/>
    public void RebindInput(string action, int index, ActionBinding binding)
    {
        if (index < 0 || index > 32 || binding.Code < 0 || binding.Code > 348 || binding.Axis < 0 || binding.Axis > 1 ||
            !Enum.IsDefined(binding.Device) || !float.IsFinite(binding.Scale)) throw new ArgumentOutOfRangeException(nameof(binding));
        var r = New(EngineOperation.InputAction, Engine.CurrentEntity); r.Flags = 2; r.Instance = (ulong)index;
        r.Data[0] = binding.Code; r.Data[1] = binding.Axis; r.Data[2] = binding.Scale;
        Call(ref r, action, binding.Device.ToString());
    }
    /// <inheritdoc/>
    public string ReadInputMap()
    {
        var r = New(EngineOperation.InputAction, Engine.CurrentEntity); r.Flags = 3;
        return Call(ref r, outputCapacity: 1024 * 1024);
    }
    /// <inheritdoc/>
    public void WriteInputMap(string json)
    {
        var r = New(EngineOperation.InputAction, Engine.CurrentEntity); r.Flags = 4; Call(ref r, input: json);
    }
    /// <inheritdoc/>
    public void DestroyEntity(ulong entity) { var r = New(EngineOperation.DestroyEntity, entity); CallEntity(ref r); }
    /// <inheritdoc/>
    public Entity FindEntity(string stableId) { var r = New(EngineOperation.FindEntity, Engine.CurrentEntity); Call(ref r, stableId); return new(r.Result); }
    /// <inheritdoc/>
    public void SetEnabled(ulong entity, bool enabled) { var r = New(EngineOperation.SetEnabled, entity); r.Flags = enabled ? 1u : 0u; CallEntity(ref r); }
    /// <inheritdoc/>
    public string ReadTexture(ulong entity) { var r = New(EngineOperation.ReadTexture, entity); return CallEntity(ref r); }
    /// <inheritdoc/>
    public void WriteTexture(ulong entity, string path) { var r = New(EngineOperation.WriteTexture, entity); CallEntity(ref r, path); }
    /// <inheritdoc/>
    public void Log(string message) { var r = New(EngineOperation.Log); Call(ref r, input: message); }
    /// <inheritdoc/>
    public string ReadSave(string key) { var r = New(EngineOperation.ReadSave); return Call(ref r, key, outputCapacity: 1024 * 1024); }
    /// <inheritdoc/>
    public void WriteSave(string key, string value) { var r = New(EngineOperation.WriteSave); Call(ref r, key, value); }
    /// <inheritdoc/>
    public void EmitEvent(string name, string payload) { var r = New(EngineOperation.EmitEvent, Engine.CurrentEntity); Call(ref r, name, payload); }
    /// <inheritdoc/>
    public string PlatformService(string name, string request)
    {
        var r = New(EngineOperation.PlatformService, Engine.CurrentEntity);
        return Call(ref r, name, request, 1024 * 1024);
    }
    /// <inheritdoc/>
    public string ReadEvent(string name, ulong afterSequence, out ulong sequence)
    {
        var r = New(EngineOperation.ReadEvent, Engine.CurrentEntity); r.Result = afterSequence;
        string payload = Call(ref r, name, outputCapacity: 65536); sequence = r.Result; return payload;
    }
    /// <inheritdoc/>
    public ulong ComponentAccess(ulong entity, uint kind, uint operation)
    {
        var r = New(EngineOperation.ComponentAccess, entity); r.Flags = kind | (operation << 8); CallEntity(ref r); return r.Result;
    }
    /// <inheritdoc/>
    public Entity CreateEntity(string name, Entity template)
    {
        var r = New(EngineOperation.CreateEntity, Engine.CurrentEntity); r.Result = template.Handle;
        CallEntity(ref r, name, referencedEntity: template.Handle); return new(r.Result);
    }
    /// <inheritdoc/>
    public Entity Parent(ulong entity, Entity parent, bool write)
    {
        var r = New(EngineOperation.Hierarchy, entity); r.Flags = write ? 1u : 0u; r.Result = parent.Handle;
        CallEntity(ref r, referencedEntity: parent.Handle); return new(r.Result);
    }
    /// <inheritdoc/>
    public Entity[] Children(ulong entity)
    {
        var r = New(EngineOperation.Hierarchy, entity); r.Flags = 2;
        using var json = System.Text.Json.JsonDocument.Parse(CallEntity(ref r, outputCapacity: 65536));
        if (json.RootElement.GetArrayLength() > 4096) throw new InvalidDataException("Hierarchy query exceeds 4096 children.");
        return json.RootElement.EnumerateArray().Select(value => new Entity(value.GetUInt64())).ToArray();
    }
    /// <inheritdoc/>
    public ulong PrefabRequest(ulong owner, ulong request, uint operation, string path)
    {
        var r = New(EngineOperation.PrefabRequest, owner); r.Instance = request; r.Flags = operation;
        Call(ref r, path); return r.Result;
    }
}
