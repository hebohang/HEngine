using System.Numerics;
using HEngine;

[ScriptType("diagnostics.fsr-portability")]
public sealed class Portability : ScriptBehaviour
{
    private readonly CombatAgent mBrain = new(new CombatAgentSettings
    {
        SightRange = 12, LoseSightRange = 14, AttackRange = 1.1f,
        DetectionSeconds = .2f, WindupSeconds = .3f, RecoverySeconds = .6f
    });
    private readonly NavigationPathFollower mPath = new();
    private readonly ActionTimeline mAction = new();
    private readonly Vector3 mTarget = new(3, .5f, 0);
    private CombatAgentState mPreviousState = CombatAgentState.Dead;
    private int mAttacks, mCommits, mCompletions;
    private bool bReported;

    public override void Initialize()
    {
        var nav = Navigation.Build(new(-6, -1, -6), new(6, 2, 6));
        if (!nav.Success) throw new InvalidOperationException("Portability navigation: " + nav.Diagnostic);
        Engine.Log("PORTABILITY navigation ready");
    }

    public override void Update(double seconds)
    {
        var dt = (float)seconds;
        var state = Entity.State;
        var decision = mBrain.Step(dt, new(state.Position, Vector3.UnitX, mTarget, true, true));
        if (decision.State != mPreviousState)
        {
            Engine.Log("PORTABILITY state " + decision.State);
            mPreviousState = decision.State;
        }
        if (decision.State == CombatAgentState.Pursue)
        {
            var ground = state.Position - new Vector3(0, .5f, 0);
            var route = mPath.Step(dt, ground, mTarget - new Vector3(0, .5f, 0), 2);
            if (!route.HasPath) throw new InvalidOperationException("Portability route: " + mPath.Diagnostic);
            state.Position += route.Displacement;
        }
        if (decision.AttackReleased)
        {
            ++mAttacks;
            mAction.Start(.4f, .5f);
        }
        var tick = mAction.Advance(dt);
        if (tick.Committed) ++mCommits;
        if (tick.Completed) ++mCompletions;
        // A visible action consumes the same clock as the gameplay marker.
        state.Scale = new Vector3(.5f, .5f + (mAction.IsActive ? .2f * MathF.Sin(mAction.Progress * MathF.PI) : 0), .5f);
        Entity.Apply(state);
        if (!bReported && mCompletions >= 2)
        {
            if (mAttacks != mCommits || mCommits != mCompletions || state.Position.X < 1.8f)
                throw new InvalidOperationException("Portability movement/action markers disagree");
            bReported = true;
            Engine.Log($"PORTABILITY PASS attacks={mAttacks} commits={mCommits} completed={mCompletions} x={state.Position.X:F3}");
        }
    }
}
