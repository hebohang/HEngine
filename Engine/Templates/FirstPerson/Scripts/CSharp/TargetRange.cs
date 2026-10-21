using System.Numerics;
using HEngine;

namespace Starter.FirstPerson;

// Deterministic rules for the authored axis-aligned Box mesh (local extent +/-1).
// These training-range collisions do not require native physics.
internal sealed class TargetRange
{
    private readonly Entity[] mTargets = new Entity[3];
    private readonly EntityState[] mInitial = new EntityState[3];
    private readonly float[] mRespawn = new float[3];
    private Entity mCover;
    public int Hits { get; private set; }
    public int Shots { get; private set; }

    public void Initialize()
    {
        for (int index = 0; index < mTargets.Length; ++index)
        {
            mTargets[index] = SceneServices.FindTag($"Target{index + 1}");
            if (!mTargets[index].IsValid)
                throw new InvalidOperationException($"Starter range requires Target{index + 1}.");
            mInitial[index] = mTargets[index].State;
        }
        mCover = SceneServices.FindTag("Cover");
        if (!mCover.IsValid) throw new InvalidOperationException("Starter range requires Cover.");
    }

    public void Reset()
    {
        Hits = Shots = 0;
        for (int index = 0; index < mTargets.Length; ++index)
        {
            mRespawn[index] = 0;
            if (mTargets[index].IsValid) mTargets[index].Apply(mInitial[index]);
        }
    }

    public void Update(float dt)
    {
        for (int index = 0; index < mTargets.Length; ++index)
        {
            if (mRespawn[index] <= 0) continue;
            mRespawn[index] -= dt;
            if (mRespawn[index] <= 0 && mTargets[index].IsValid)
                mTargets[index].Apply(mInitial[index]);
        }
    }

    public Vector3 Move(Vector3 position, Vector3 displacement, float radius)
    {
        // Axis-separated motion slides along cover and permanent target stands.
        var candidate = position;
        candidate.X = Math.Clamp(position.X + displacement.X, -9 + radius, 9 - radius);
        if (!Blocked(candidate, radius)) position.X = candidate.X;
        candidate = position;
        candidate.Z = Math.Clamp(position.Z + displacement.Z, -9 + radius, 9 - radius);
        if (!Blocked(candidate, radius)) position.Z = candidate.Z;
        return position;
    }

    private bool Blocked(Vector3 position, float radius)
    {
        if (mCover.IsValid && Overlaps(position, radius, mCover.State)) return true;
        foreach (var target in mInitial)
            if (Overlaps(position, radius, target)) return true;
        return false;
    }

    private static bool Overlaps(Vector3 position, float radius, EntityState box) =>
        MathF.Abs(position.X - box.Position.X) < MathF.Abs(box.Scale.X) + radius &&
        MathF.Abs(position.Z - box.Position.Z) < MathF.Abs(box.Scale.Z) + radius;

    public void Fire(Vector3 origin, Vector3 direction)
    {
        ++Shots;
        float nearest = 50;
        int hit = -1;
        if (mCover.IsValid && RayBox(origin, direction, mCover.State, out float coverDistance))
            nearest = MathF.Min(nearest, coverDistance);
        for (int index = 0; index < mTargets.Length; ++index)
        {
            if (mRespawn[index] > 0 || !mTargets[index].IsValid) continue;
            if (RayBox(origin, direction, mTargets[index].State, out float distance) && distance < nearest)
            { nearest = distance; hit = index; }
        }
        if (hit < 0) return;
        ++Hits;
        mRespawn[hit] = 2;
        var state = mTargets[hit].State;
        state.Position.Y = -10; // Keep scale and stable entity identity intact.
        mTargets[hit].Apply(state);
    }

    // Slabs handle parallel axes and starts inside a box. Direction is unit length.
    private static bool RayBox(Vector3 origin, Vector3 direction, EntityState box, out float distance)
    {
        var minimum = box.Position - Vector3.Abs(box.Scale);
        var maximum = box.Position + Vector3.Abs(box.Scale);
        float near = 0, far = 50;
        for (int axis = 0; axis < 3; ++axis)
        {
            if (MathF.Abs(direction[axis]) < .00001f)
            {
                if (origin[axis] < minimum[axis] || origin[axis] > maximum[axis])
                { distance = 0; return false; }
                continue;
            }
            float first = (minimum[axis] - origin[axis]) / direction[axis];
            float second = (maximum[axis] - origin[axis]) / direction[axis];
            near = MathF.Max(near, MathF.Min(first, second));
            far = MathF.Min(far, MathF.Max(first, second));
            if (near > far) { distance = 0; return false; }
        }
        distance = near;
        return true;
    }
}
