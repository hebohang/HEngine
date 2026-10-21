using System.Numerics;
using HEngine;

namespace Starter.FirstPerson;

[ScriptType("starter.first-person")]
[ScriptDependency("Assets/UI/StarterHud.rml")]
public sealed class FirstPersonController : ScriptBehaviour
{
    [ScriptField("move-speed", HasRange = true, Min = 1, Max = 12)]
    public float MoveSpeed = 4.5f;

    private readonly TargetRange mRange = new();
    private EntityState mSpawn;
    private UiDocument? mHud;
    private float mCooldown;
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
            mRange.Reset();
            mCooldown = 0;
            RefreshHud();
            return;
        }

        mRange.Update(dt);
        mCooldown = MathF.Max(0, mCooldown - dt);
        var state = Entity.State;
        var turn = InputActions.Read("Turn").Value * (2 * dt);
        // Pointer actions contain a scaled frame delta: do not multiply by dt.
        if (InputActions.Read("LookHeld").Held)
            turn += InputActions.Read("Look").Value * .3f;
        state.Rotation.Y -= turn.X;
        state.Rotation.X = Math.Clamp(state.Rotation.X + turn.Y, -1.3f, 1.3f);
        var move = InputActions.Read("Move").Value;
        if (move.LengthSquared() > 1) move = Vector2.Normalize(move);
        float yaw = state.Rotation.Y;
        var forward = new Vector3(-MathF.Sin(yaw), 0, -MathF.Cos(yaw));
        var right = new Vector3(MathF.Cos(yaw), 0, -MathF.Sin(yaw));
        state.Position = mRange.Move(state.Position, (right * move.X + forward * move.Y) * (MoveSpeed * dt), .35f);
        Entity.Apply(state);

        var fire = InputActions.Read("Fire");
        if ((fire.Held || fire.Pressed) && mCooldown <= 0)
        {
            float pitch = state.Rotation.X;
            var direction = forward * MathF.Cos(pitch) + Vector3.UnitY * MathF.Sin(pitch);
            mRange.Fire(state.Position, direction);
            mCooldown = .2f;
            RefreshHud();
        }
    }

    private void RefreshHud() => mHud?.SetText("score", $"Hits {Hits}  /  Shots {Shots}  |  R: reset");
    public override void Destroy() { mHud?.Dispose(); mHud = null; }
}
