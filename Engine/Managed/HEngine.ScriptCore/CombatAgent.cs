using System.Numerics;

namespace HEngine;

/// <summary>Reusable combat phases; locomotion, collision and rendering remain with the caller.</summary>
public enum CombatAgentState
{
    /// <summary>No remembered target.</summary>
    Patrol,
    /// <summary>Confirming sustained visual contact.</summary>
    Acquire,
    /// <summary>Visible target outside attack distance.</summary>
    Pursue,
    /// <summary>Telegraphing an attack at a locked aim point.</summary>
    Windup,
    /// <summary>Attack cooldown.</summary>
    Recover,
    /// <summary>Investigating the last observed position.</summary>
    Search,
    /// <summary>Disabled until explicitly reset.</summary>
    Dead
}

/// <summary>Immutable, engine-independent tuning in world units and seconds.</summary>
public sealed record CombatAgentSettings
{
    /// <summary>Distance at which a previously unseen target can be acquired.</summary>
    public float SightRange { get; init; } = 36;
    /// <summary>Distance at which a tracked target is lost; must be at least SightRange.</summary>
    public float LoseSightRange { get; init; } = 40;
    /// <summary>Full horizontal/vertical sight cone angle, in degrees.</summary>
    public float FieldOfViewDegrees { get; init; } = 160;
    /// <summary>Attack distance; cannot exceed SightRange.</summary>
    public float AttackRange { get; init; } = 28;
    /// <summary>Continuous visible contact required before engaging.</summary>
    public float DetectionSeconds { get; init; } = .3f;
    /// <summary>Visible warning before each attack.</summary>
    public float WindupSeconds { get; init; } = .6f;
    /// <summary>Minimum time from an attack release to the next warning.</summary>
    public float RecoverySeconds { get; init; } = 2;
    /// <summary>Time to remember an occluded target.</summary>
    public float MemorySeconds { get; init; } = 3;
}

/// <summary>One caller-owned perception sample. Line of sight must include world occlusion.</summary>
/// <param name="Position">Observer position.</param>
/// <param name="Forward">Observer facing; zero means no directional visibility.</param>
/// <param name="TargetPosition">Current candidate target position.</param>
/// <param name="TargetAlive">Whether the target remains valid and alive.</param>
/// <param name="ClearLineOfSight">Whether collision queries find an unobstructed view.</param>
public readonly record struct CombatObservation(Vector3 Position, Vector3 Forward,
    Vector3 TargetPosition, bool TargetAlive, bool ClearLineOfSight);

/// <summary>Value-only decision for animation, movement and attacks.</summary>
/// <param name="State">Current phase.</param>
/// <param name="TargetVisible">Visibility including range, cone and occlusion.</param>
/// <param name="HasDestination">Whether there is a remembered movement/look destination.</param>
/// <param name="Destination">Last observed position; never follows an unseen target.</param>
/// <param name="AimPoint">Position locked when the attack warning began.</param>
/// <param name="AttackReleased">One-step pulse; the caller decides whether a weapon can emit.</param>
/// <param name="WindupProgress">Warning progress from zero to one.</param>
public readonly record struct CombatDecision(CombatAgentState State, bool TargetVisible,
    bool HasDestination, Vector3 Destination, Vector3 AimPoint, bool AttackReleased, float WindupProgress);

/// <summary>Deterministic visual perception, target memory and interruptible attack phases.
/// No scene handles or global state; one instance per agent. Each Step emits at most one attack.
/// Large steps do not skip a newly entered warning phase. Zero time never advances phases.</summary>
public sealed class CombatAgent
{
    private readonly CombatAgentSettings mSettings;
    private readonly float mHalfConeCos;
    private float mPhaseTime, mMemory, mRecovery;
    private bool mHasMemory;
    private Vector3 mLastSeen, mAimPoint;

    /// <summary>Current phase.</summary>
    public CombatAgentState State { get; private set; } = CombatAgentState.Patrol;

    /// <summary>Validates tuning once; subsequent decisions allocate no resources.</summary>
    public CombatAgent(CombatAgentSettings settings)
    {
        ArgumentNullException.ThrowIfNull(settings);
        static bool Positive(float value) => float.IsFinite(value) && value > 0;
        if (!Positive(settings.SightRange) || !Positive(settings.LoseSightRange) ||
            settings.LoseSightRange < settings.SightRange || !Positive(settings.AttackRange) ||
            settings.AttackRange > settings.SightRange || !Positive(settings.FieldOfViewDegrees) ||
            settings.FieldOfViewDegrees > 360 || !Positive(settings.DetectionSeconds) ||
            !Positive(settings.WindupSeconds) || !Positive(settings.RecoverySeconds) || !Positive(settings.MemorySeconds))
            throw new ArgumentOutOfRangeException(nameof(settings), "Combat ranges, cone and phase durations must be finite and valid.");
        mSettings = settings;
        mHalfConeCos = MathF.Cos(settings.FieldOfViewDegrees * MathF.PI / 360);
    }

    private static bool Finite(Vector3 value) => float.IsFinite(value.X) && float.IsFinite(value.Y) && float.IsFinite(value.Z);
    private void Enter(CombatAgentState state) { State = state; mPhaseTime = 0; }

    /// <summary>Clears all target/attack state, e.g. on spawn, pooling or a new wave.</summary>
    public void Reset(float initialRecoverySeconds = 0)
    {
        if (!float.IsFinite(initialRecoverySeconds) || initialRecoverySeconds < 0)
            throw new ArgumentOutOfRangeException(nameof(initialRecoverySeconds));
        mHasMemory = false; mMemory = 0; mLastSeen = mAimPoint = Vector3.Zero;
        mRecovery = initialRecoverySeconds; Enter(CombatAgentState.Patrol);
    }

    /// <summary>Stops all attacks and forgets the target until Reset.</summary>
    public void Kill() { Reset(); Enter(CombatAgentState.Dead); }

    /// <summary>Records a heard/damage location without granting visual contact or permitting a blind shot.</summary>
    public void Investigate(Vector3 position)
    {
        if (!Finite(position)) throw new ArgumentOutOfRangeException(nameof(position));
        if (State == CombatAgentState.Dead) return;
        mLastSeen = position; mHasMemory = true; mMemory = mSettings.MemorySeconds;
        if (State is CombatAgentState.Patrol or CombatAgentState.Search) Enter(CombatAgentState.Search);
    }

    /// <summary>Consumes a finite nonnegative simulation interval and one sensor sample.</summary>
    public CombatDecision Step(float seconds, in CombatObservation observation)
    {
        if (!float.IsFinite(seconds) || seconds < 0 || !Finite(observation.Position) ||
            !Finite(observation.Forward) || !Finite(observation.TargetPosition))
            throw new ArgumentOutOfRangeException(nameof(observation), "Combat samples and time must be finite.");
        bool released = false, visible = false;
        if (State == CombatAgentState.Dead) return Decision();
        Vector3 delta = observation.TargetPosition - observation.Position;
        float distance = delta.Length(), forwardLength = observation.Forward.Length();
        float range = mHasMemory ? mSettings.LoseSightRange : mSettings.SightRange;
        visible = observation.TargetAlive && observation.ClearLineOfSight && distance <= range &&
            forwardLength > .0001f && (distance < .0001f ||
                Vector3.Dot(delta / distance, observation.Forward / forwardLength) >= mHalfConeCos);
        mRecovery = MathF.Max(0, mRecovery - seconds);
        if (visible) { mLastSeen = observation.TargetPosition; mHasMemory = true; mMemory = mSettings.MemorySeconds; }
        else { mMemory = MathF.Max(0, mMemory - seconds); if (mMemory == 0 || !observation.TargetAlive) mHasMemory = false; }
        if (!visible)
        {
            var next = mHasMemory ? CombatAgentState.Search : CombatAgentState.Patrol;
            if (State != next) Enter(next);
            return Decision();
        }
        if (State is CombatAgentState.Patrol or CombatAgentState.Search) { Enter(CombatAgentState.Acquire); return Decision(); }
        mPhaseTime += seconds;
        switch (State)
        {
            case CombatAgentState.Acquire:
                if (seconds > 0 && mPhaseTime >= mSettings.DetectionSeconds) Enter(CombatAgentState.Pursue);
                break;
            case CombatAgentState.Pursue:
                if (seconds > 0 && distance <= mSettings.AttackRange && mRecovery == 0)
                { mAimPoint = observation.TargetPosition; Enter(CombatAgentState.Windup); }
                break;
            case CombatAgentState.Windup:
                if (distance > mSettings.AttackRange) Enter(CombatAgentState.Pursue);
                else if (seconds > 0 && mPhaseTime >= mSettings.WindupSeconds)
                { released = true; mRecovery = mSettings.RecoverySeconds; Enter(CombatAgentState.Recover); }
                break;
            case CombatAgentState.Recover:
                if (seconds > 0 && mRecovery == 0) Enter(CombatAgentState.Pursue);
                break;
        }
        return Decision();
        CombatDecision Decision() => new(State, visible, mHasMemory, mLastSeen, mAimPoint,
            released, State == CombatAgentState.Windup ? Math.Clamp(mPhaseTime / mSettings.WindupSeconds, 0, 1) : 0);
    }
}
