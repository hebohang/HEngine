using System.Numerics;

namespace HEngine;

/// <summary>One segment of travel, bounded by the next corner so a hitch cannot cut through a wall.</summary>
/// <param name="Displacement">Requested movement; the caller must resolve body collision.</param>
/// <param name="HasPath">A complete route exists.</param>
/// <param name="Arrived">The last corner has been reached.</param>
/// <param name="Replanned">A query was issued this step.</param>
public readonly record struct NavigationFollowStep(Vector3 Displacement, bool HasPath, bool Arrived, bool Replanned);

/// <summary>Per-agent path following with throttled target updates and failed-path retries.
/// Positions are ground anchors in world space. Movement and collision remain caller-owned.</summary>
public sealed class NavigationPathFollower
{
    private readonly Func<Vector3, Vector3, NavigationPath> mQuery;
    private readonly float mReplanSeconds, mTargetTolerance, mCornerTolerance;
    private Vector3[] mPoints = [];
    private Vector3 mGoal;
    private int mCorner;
    private float mCooldown;
    private bool mQueried;
    /// <summary>Diagnostic from the most recent path query.</summary>
    public string Diagnostic { get; private set; } = "";

    /// <summary>Constructs a follower; an optional query supports other navigation providers.</summary>
    public NavigationPathFollower(Func<Vector3, Vector3, NavigationPath>? query = null,
        float replanSeconds = .5f, float targetTolerance = .4f, float cornerTolerance = .05f)
    {
        if (!float.IsFinite(replanSeconds) || replanSeconds <= 0 || !float.IsFinite(targetTolerance) ||
            targetTolerance <= 0 || !float.IsFinite(cornerTolerance) || cornerTolerance <= 0)
            throw new ArgumentOutOfRangeException(nameof(replanSeconds));
        mQuery = query ?? Navigation.FindPath;
        mReplanSeconds = replanSeconds; mTargetTolerance = targetTolerance; mCornerTolerance = cornerTolerance;
    }

    /// <summary>Clears old scene, teleport or respawn state. The next positive step queries immediately.</summary>
    public void Reset()
    {
        mPoints = []; mCorner = 0; mCooldown = 0; mQueried = false; Diagnostic = "";
    }

    /// <summary>Stops a route obstructed by dynamic collision; retries retain the query cooldown.</summary>
    public void Invalidate() { mPoints = []; mCorner = 0; }

    /// <summary>Requests at most speed * seconds of movement toward one corner. Zero time is inert.</summary>
    public NavigationFollowStep Step(float seconds, Vector3 position, Vector3 goal, float speed)
    {
        if (!float.IsFinite(seconds) || seconds < 0 || !float.IsFinite(speed) || speed < 0 ||
            !Finite(position) || !Finite(goal)) throw new ArgumentException("Invalid navigation follower input.");
        if (seconds == 0 || speed == 0) return new(Vector3.Zero, mPoints.Length > 0, false, false);
        mCooldown = MathF.Max(0, mCooldown - seconds);
        bool replanned = false;
        bool changed = Vector3.DistanceSquared(goal, mGoal) > mTargetTolerance * mTargetTolerance;
        if (!mQueried || (mCooldown == 0 && (changed || mPoints.Length == 0)))
        {
            var path = mQuery(position, goal);
            mPoints = path.Success && path.Points.Length > 0 && path.Points.All(Finite) ? (Vector3[])path.Points.Clone() : [];
            mGoal = goal; mCorner = 0; mCooldown = mReplanSeconds; mQueried = true; replanned = true;
            Diagnostic = path.Diagnostic;
        }
        if (mPoints.Length == 0) return new(Vector3.Zero, false, false, replanned);
        // Skip only corners reached by the actual collision-resolved position.
        while (mCorner < mPoints.Length && Vector3.DistanceSquared(position, mPoints[mCorner]) <= mCornerTolerance * mCornerTolerance)
            ++mCorner;
        if (mCorner == mPoints.Length) return new(Vector3.Zero, true, true, replanned);
        var delta = mPoints[mCorner] - position;
        float distance = delta.Length();
        return new(delta / distance * MathF.Min(distance, speed * seconds), true, false, replanned);
    }

    private static bool Finite(Vector3 value) => float.IsFinite(value.X) && float.IsFinite(value.Y) && float.IsFinite(value.Z);
}
