using System.Numerics;

namespace HEngine;

/// <summary>Stable contact phases for engine-owned Jolt and Box2D rigid-body events.</summary>
public enum ScriptContactPhase
{
    /// <summary>A new contact pair.</summary>
    Enter = 0,
    /// <summary>An existing contact pair.</summary>
    Stay = 1,
    /// <summary>A contact pair that has ended; the other entity may already be invalid.</summary>
    Exit = 2
}

/// <summary>Value-only contact data. Other is a generation-checked scene handle; no native collider pointer is exposed.</summary>
public readonly record struct ScriptContact
{
    /// <summary>The other scene entity. Resolve validity before using it, particularly for Exit events.</summary>
    public Entity Other { get; }
    /// <summary>Enter, Stay or Exit.</summary>
    public ScriptContactPhase Phase { get; }
    /// <summary>World-space contact point; zero for triggers or unavailable exit manifold data.</summary>
    public Vector3 Point { get; }
    /// <summary>World-space normal facing this receiver; zero when no manifold exists.</summary>
    public Vector3 Normal { get; }
    /// <summary>Constructs bounded finite contact data; invalid phase or coordinates throw.</summary>
    public ScriptContact(Entity other, ScriptContactPhase phase, Vector3 point, Vector3 normal)
    {
        if (phase is < ScriptContactPhase.Enter or > ScriptContactPhase.Exit) throw new ArgumentOutOfRangeException(nameof(phase));
        if (!float.IsFinite(point.X) || !float.IsFinite(point.Y) || !float.IsFinite(point.Z) ||
            !float.IsFinite(normal.X) || !float.IsFinite(normal.Y) || !float.IsFinite(normal.Z))
            throw new ArgumentException("Contact coordinates must be finite.");
        Other = other; Phase = phase; Point = point; Normal = normal;
    }
}

/// <summary>Engine-thread collision/trigger delivery for scripts attached to rigid bodies.</summary>
public interface IScriptPhysicsEvents
{
    /// <summary>Receives a physical contact on the engine thread; exceptions enter the caller's script boundary.</summary>
    void Collision(in ScriptContact contact);
    /// <summary>Receives a trigger overlap on the engine thread; exceptions enter the caller's script boundary.</summary>
    void Trigger(in ScriptContact contact);
}

/// <summary>Type-safe contact dispatcher used by the native physics event channel.</summary>
public static class ScriptPhysics
{
    /// <summary>Invokes an explicitly implemented interface, without method-name scanning. Returns false for non-subscribers.</summary>
    public static bool Dispatch(ScriptBehaviour behaviour, in ScriptContact contact, bool trigger)
    {
        ArgumentNullException.ThrowIfNull(behaviour);
        behaviour.Lifetime.ThrowIfCancellationRequested();
        if (behaviour is not IScriptPhysicsEvents receiver) return false;
        if (trigger) receiver.Trigger(contact); else receiver.Collision(contact);
        return true;
    }
}
