using System.Numerics;
using HEngine;

namespace Starter.TopDown;

[ScriptType("starter.top-down")]
[ScriptDependency("Assets/UI/StarterHud.rml")]
public sealed class TopDownController : ScriptBehaviour
{
    [ScriptField("move-speed", HasRange = true, Min = 1, Max = 12)]
    public float MoveSpeed = 4.5f;

    private readonly TargetRange mRange = new();
    private EntityState mSpawn;
    private UiDocument? mHud;
    private float mCooldown;
    private Vector3 mAim = -Vector3.UnitZ;
    public int Hits => mRange.Hits;
    public int Shots => mRange.Shots;

    public override void Initialize()
    {
        mSpawn = Entity.State;
        mRange.Initialize();
        mHud = UiDocument.Load(this, new("Assets/UI/StarterHud.rml"));
        RefreshHud();
    }

    public override void Update(double seconds)
    {
        float dt = (float)Math.Clamp(seconds, 0, .1);
        if (InputActions.Read("Reset").Pressed)
        {
            Entity.Apply(mSpawn);
            mAim = -Vector3.UnitZ;
            mRange.Reset();
            mCooldown = 0;
            RefreshHud();
            return;
        }

        mRange.Update(dt);
        mCooldown = MathF.Max(0, mCooldown - dt);
        var state = Entity.State;
        var move = InputActions.Read("Move").Value;
        if (move.LengthSquared() > 1) move = Vector2.Normalize(move);
        state.Position = mRange.Move(state.Position, new Vector3(move.X, 0, -move.Y) * (MoveSpeed * dt), .45f);
        var aim = InputActions.Read("Aim").Value;
        if (aim.LengthSquared() > .001f)
            mAim = Vector3.Normalize(new Vector3(aim.X, 0, -aim.Y));
        state.Rotation.Y = MathF.Atan2(-mAim.X, -mAim.Z);
        Entity.Apply(state);

        var fire = InputActions.Read("Fire");
        if ((fire.Held || fire.Pressed) && mCooldown <= 0)
        {
            mRange.Fire(state.Position, mAim);
            mCooldown = .2f;
            RefreshHud();
        }
    }

    private void RefreshHud() => mHud?.SetText("score", $"Hits {Hits}  /  Shots {Shots}  |  R: reset");
    public override void Destroy() { mHud?.Dispose(); mHud = null; }
}
