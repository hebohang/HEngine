using System.Numerics;

namespace HEngine;

/// <summary>A native object or component view whose lifetime has ended. No fake-null conversion is performed.</summary>
public sealed class InvalidEngineObjectException(string objectType, ulong entity, string origin, Exception? innerException = null)
    : InvalidOperationException($"{objectType} on entity {entity} is invalid at {origin}.", innerException)
{
    /// <summary>Object/view type whose lifetime ended.</summary>
    public string ObjectType { get; } = objectType;
    /// <summary>Stale runtime entity handle, retained for diagnostics only.</summary>
    public ulong EntityHandle { get; } = entity;
    /// <summary>API operation that detected the invalid lifetime.</summary>
    public string Origin { get; } = origin;
}

/// <summary>Explicit component binding; implementors supply a fixed binding ID, never a reflected type name.</summary>
public interface IComponentView<TSelf> where TSelf : struct, IComponentView<TSelf>
{
    /// <summary>Fixed native binding ID; unsupported IDs are rejected.</summary>
    static abstract uint Kind { get; }
    /// <summary>Constructs a view of a native component generation; host binding only.</summary>
    static abstract TSelf Bind(Entity entity, ulong generation);
}

public readonly partial record struct Entity
{
    /// <summary>Returns whether the native entity has a supported component. A stale entity throws.</summary>
    public bool HasComponent<T>() where T : struct, IComponentView<T> => Engine.Api.ComponentAccess(Handle, T.Kind, 0) != 0;
    /// <summary>Gets a generation-checked component view; missing or destroyed components throw.</summary>
    public T GetComponent<T>() where T : struct, IComponentView<T>
        => TryGetComponent<T>(out var view) ? view : throw new InvalidEngineObjectException(typeof(T).Name, Handle, "GetComponent");
    /// <summary>Returns false for a missing component; stale entities and API failures throw.</summary>
    public bool TryGetComponent<T>(out T view) where T : struct, IComponentView<T>
    {
        ulong generation = Engine.Api.ComponentAccess(Handle, T.Kind, 0);
        view = generation == 0 ? default : T.Bind(this, generation);
        return generation != 0;
    }
    /// <summary>Adds a supported component or returns its existing view.</summary>
    public T AddComponent<T>() where T : struct, IComponentView<T> => T.Bind(this, Engine.Api.ComponentAccess(Handle, T.Kind, 1));
    /// <summary>Removes a supported optional component. Required Transform removal throws.</summary>
    public bool RemoveComponent<T>() where T : struct, IComponentView<T> => Engine.Api.ComponentAccess(Handle, T.Kind, 2) != 0;
    /// <summary>Gets or sets the same-scene parent; the default Entity detaches. Cycles throw.</summary>
    public Entity Parent { get => Engine.Api.Parent(Handle, default, false); set => Engine.Api.Parent(Handle, value, true); }
    /// <summary>Returns a bounded children snapshot. Avoid allocating it on every frame.</summary>
    public Entity[] GetChildrenSnapshot() => Engine.Api.Children(Handle);
    /// <summary>Creates an entity in the currently executing script's scene.</summary>
    public static Entity Create(string name = "Entity") => Engine.Api.CreateEntity(name, default);
    /// <summary>Duplicates an existing same-scene entity hierarchy with new stable identities.</summary>
    public static Entity Instantiate(Entity template) => Engine.Api.CreateEntity("", template);
}

/// <summary>A generation-checked Transform view. Snapshot accesses batch position, rotation and scale.</summary>
public readonly record struct Transform(Entity Entity, ulong Generation) : IComponentView<Transform>
{
    /// <inheritdoc/>
    public static uint Kind => 1;
    /// <inheritdoc/>
    public static Transform Bind(Entity entity, ulong generation) => new(entity, generation);
    private void Check()
    {
        if (Generation == 0 || !Entity.IsValid || Engine.Api.ComponentAccess(Entity.Handle, Kind, 0) != Generation)
            throw new InvalidEngineObjectException(nameof(Transform), Entity.Handle, "Transform.Snapshot");
    }
    /// <summary>Batched transform state; invalid views throw InvalidEngineObjectException.</summary>
    public EntityState Snapshot { get { Check(); return Entity.State; } set { Check(); Engine.Api.WriteEntity(Entity.Handle, value, true, false); } }
    /// <summary>Translation relative to the parent. Use WorldPosition for a world-space query.</summary>
    public Vector3 LocalPosition { get => Snapshot.Position; set { var state = Snapshot; state.Position = value; Snapshot = state; } }
    /// <summary>Local Euler rotation in radians, using the engine's transform convention.</summary>
    public Vector3 LocalRotation { get => Snapshot.Rotation; set { var state = Snapshot; state.Rotation = value; Snapshot = state; } }
    /// <summary>Scale relative to the parent.</summary>
    public Vector3 LocalScale { get => Snapshot.Scale; set { var state = Snapshot; state.Scale = value; Snapshot = state; } }
    /// <summary>Evaluated world position including hierarchy and bone attachments.</summary>
    public Vector3 WorldPosition { get { Check(); return SceneServices.WorldPosition(Entity); } }
}

/// <summary>A generation-checked Sprite view. Removing/replacing the component permanently invalidates old views.</summary>
public readonly record struct Sprite(Entity Entity, ulong Generation) : IComponentView<Sprite>
{
    /// <inheritdoc/>
    public static uint Kind => 2;
    /// <inheritdoc/>
    public static Sprite Bind(Entity entity, ulong generation) => new(entity, generation);
    private void Check()
    {
        if (Generation == 0 || !Entity.IsValid || Engine.Api.ComponentAccess(Entity.Handle, Kind, 0) != Generation)
            throw new InvalidEngineObjectException(nameof(Sprite), Entity.Handle, "Sprite");
    }
    /// <summary>RGBA tint; stale component generations throw InvalidEngineObjectException.</summary>
    public Vector4 Color
    {
        get { Check(); return Entity.State.Color; }
        set { Check(); var state = Entity.State; state.Color = value; Engine.Api.WriteEntity(Entity.Handle, state, false, true); }
    }
    /// <summary>Stable texture identity; invalid assets or retired views throw.</summary>
    public ResourceReference Texture { get { Check(); return Entity.Texture; } set { Check(); Engine.Api.WriteTexture(Entity.Handle, value.Id); } }
}
