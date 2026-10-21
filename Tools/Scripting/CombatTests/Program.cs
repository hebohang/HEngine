using System.Numerics;
using HEngine;

int checks = 0;
void Check(bool condition, string reason)
{
    if (!condition) throw new Exception(reason);
    ++checks;
}
void Reject(Action action, string reason)
{
    try { action(); } catch (ArgumentException) { ++checks; return; }
    throw new Exception(reason);
}
var settings = new CombatAgentSettings { SightRange = 20, LoseSightRange = 24, AttackRange = 12,
    FieldOfViewDegrees = 120, DetectionSeconds = .25f, WindupSeconds = .5f,
    RecoverySeconds = 1, MemorySeconds = 2 };
var agent = new CombatAgent(settings);
var sample = new CombatObservation(Vector3.Zero, Vector3.UnitZ, new(0, 0, 10), true, true);
Check(agent.Step(.1f, sample with { ClearLineOfSight = false }).State == CombatAgentState.Patrol, "Occlusion blocks first acquisition");
Check(!agent.Step(.1f, sample with { TargetPosition = new(10, 0, 0) }).TargetVisible, "Cone excludes side targets");
Check(!agent.Step(.1f, sample with { TargetPosition = new(0, 0, 21) }).TargetVisible, "Initial sight range");
Check(!agent.Step(.1f, sample with { Forward = Vector3.Zero }).TargetVisible, "Zero facing is not omniscient");
Check(agent.Step(.1f, sample).State == CombatAgentState.Acquire, "First contact requires reaction");
Check(agent.Step(.2f, sample).State == CombatAgentState.Acquire, "No instant detection");
Check(agent.Step(.1f, sample with { ClearLineOfSight = false }).State == CombatAgentState.Search, "Loss interrupts acquisition");
Check(agent.Step(.1f, sample).State == CombatAgentState.Acquire, "Reacquisition starts fresh");
Check(agent.Step(.25f, sample).State == CombatAgentState.Pursue, "Continuous contact confirms target");
Check(agent.Step(0, sample).State == CombatAgentState.Pursue, "Zero time cannot start warning");
Check(agent.Step(10, sample).State == CombatAgentState.Windup, "Hitch cannot skip a new warning");
var moved = sample with { TargetPosition = new(1, 0, 10) };
Check(agent.Step(.2f, moved).AimPoint == sample.TargetPosition, "Warning locks attack point for dodging");
var shot = agent.Step(.3f, moved);
Check(shot.AttackReleased && shot.State == CombatAgentState.Recover && shot.AimPoint == sample.TargetPosition, "One attack on warning end");
Check(!agent.Step(.5f, moved).AttackReleased, "Cooldown suppresses repeated attack");
Check(!agent.Step(20, moved).AttackReleased && agent.State == CombatAgentState.Pursue, "Hitch cannot burst through recovery");
agent.Step(.01f, moved);
var lost = agent.Step(.1f, sample with { ClearLineOfSight = false, TargetPosition = new(100, 0, 0) });
Check(lost.State == CombatAgentState.Search && !lost.AttackReleased && lost.Destination == moved.TargetPosition, "Occlusion cancels warning and remembers last sight only");
Check(agent.Step(2, sample with { ClearLineOfSight = false }).State == CombatAgentState.Patrol, "Memory expires");
agent.Investigate(new(4, 0, 0));
var heard = agent.Step(.1f, sample with { ClearLineOfSight = false });
Check(heard.State == CombatAgentState.Search && heard.Destination == new Vector3(4, 0, 0) && !heard.AttackReleased, "Sound directs search without sight or blind shot");
agent.Kill(); agent.Investigate(Vector3.One);
Check(agent.Step(30, sample).State == CombatAgentState.Dead && !agent.Step(.1f, sample).HasDestination, "Dead agents stay inert");
agent.Reset(3);
agent.Step(.1f, sample); agent.Step(.3f, sample);
Check(agent.Step(.1f, sample).State == CombatAgentState.Pursue, "Spawn grace prevents early windup");
Check(agent.Step(.1f, sample with { TargetPosition = new(0, 0, 23) }).TargetVisible, "Range hysteresis retains observed target");
Check(!agent.Step(.1f, sample with { TargetAlive = false }).HasDestination, "Dead target clears memory");
Reject(() => agent.Step(float.NaN, sample), "NaN time rejected");
Reject(() => agent.Step(.1f, sample with { Position = new(float.PositiveInfinity) }), "Nonfinite perception rejected");
Reject(() => new CombatAgent(settings with { AttackRange = 25 }), "Invalid tuning rejected");

agent.Reset();
agent.Step(.1f, sample); agent.Step(.25f, sample); agent.Step(.1f, sample);
Check(agent.State == CombatAgentState.Windup, "Lifecycle fixture entered warning");
var pausedWarning = agent.Step(0, sample);
Check(agent.Step(0, sample) == pausedWarning && !pausedWarning.AttackReleased,
    "Paused warning does not advance or release");
agent.Kill();
Check(!agent.Step(100, sample).AttackReleased && agent.State == CombatAgentState.Dead,
    "Death during warning cancels its pending attack");
agent.Reset(2);
var reset = agent.Step(0, sample with { ClearLineOfSight = false });
Check(reset.State == CombatAgentState.Patrol && !reset.HasDestination && reset.AimPoint == Vector3.Zero,
    "A new wave clears old target memory and locked aim");
agent.Step(.1f, sample); agent.Step(.25f, sample);
Check(agent.Step(.5f, sample).State == CombatAgentState.Pursue, "New wave retains spawn grace");
agent.Step(2, sample);
var deadTarget = agent.Step(.5f, sample with { TargetAlive = false });
Check(!deadTarget.AttackReleased && !deadTarget.HasDestination && deadTarget.State == CombatAgentState.Patrol,
    "Target death during warning cancels its pending release");

var timeline = new ActionTimeline();
timeline.Start(2, .75f);
Check(timeline.Advance(0) == default && timeline.Progress == 0, "Pause does not commit");
Check(!timeline.Advance(1).Committed && timeline.Progress == .5f, "Animation and action share normalized clock");
timeline.Cancel();
Check(timeline.Advance(10) == default && !timeline.HasCommitted, "Cancel before marker prevents gameplay effect");
timeline.Start(2, .75f);
Check(timeline.Advance(1.5f).Committed && timeline.HasCommitted, "Marker commits once");
Check(!timeline.Advance(.1f).Committed, "Marker is not repeated");
timeline.Cancel();
Check(timeline.HasCommitted && timeline.Advance(5) == default, "Cancel after marker keeps committed effect");
timeline.Start(2);
Check(timeline.Advance(5) == new ActionTimelineTick(true, true) && timeline.Progress == 1 && !timeline.IsActive, "Large interval crosses marker and end exactly once");
Check(timeline.Advance(1) == default, "Completed action emits no further markers");
Reject(() => timeline.Start(float.NaN), "Invalid duration rejected");
Reject(() => timeline.Start(1, 2), "Invalid marker rejected");
Reject(() => timeline.Advance(-1), "Negative interval rejected");
int queries = 0;
bool reachable = true;
var follower = new NavigationPathFollower((start, end) => {
    ++queries;
    return reachable ? new(true, [start, new(0, 0, 2), new(4, 0, 2), end], "") : new(false, [], "disconnected");
});
var target = new Vector3(4, 0, 0);
Check(follower.Step(0, Vector3.Zero, target, 2).Displacement == Vector3.Zero && queries == 0, "Paused follower performs no query");
var follow = follower.Step(.25f, Vector3.Zero, target, 2);
Check(follow.Replanned && follow.HasPath && follow.Displacement == new Vector3(0, 0, .5f), "Follower steers to the obstacle corner, not straight to goal");
follow = follower.Step(10, Vector3.Zero, target, 2);
Check(follow.Displacement == new Vector3(0, 0, 2) && queries == 1, "A hitch cannot skip a corner or overshoot");
follow = follower.Step(.25f, new(0, 0, 2), target, 2);
Check(follow.Displacement == new Vector3(.5f, 0, 0), "Only actual arrival advances the corner");
follower.Step(2, new(4, 0, 2), target, 2);
Check(follower.Step(.1f, target, target, 2).Arrived, "Complete route reports arrival");
follower.Step(.1f, target, target + new Vector3(.1f, 0, 0), 2);
Check(queries == 1, "Small goal motion does not churn path queries");
follower.Step(.1f, target, target + Vector3.UnitX, 2);
Check(queries == 2, "Significant goal motion replans");
follower.Invalidate(); reachable = false;
follow = follower.Step(.1f, target, Vector3.Zero, 2);
Check(!follow.HasPath && !follow.Replanned && follow.Displacement == Vector3.Zero, "Collision invalidation stops movement without retry storm");
follow = follower.Step(.5f, target, Vector3.Zero, 2);
Check(!follow.HasPath && follow.Replanned && queries == 3 && follower.Diagnostic == "disconnected", "Unreachable destination stops with diagnostic");
follower.Step(.1f, target, Vector3.Zero, 2);
Check(queries == 3, "Unreachable queries are throttled");
reachable = true; follower.Reset();
Check(follower.Step(.01f, Vector3.Zero, target, 2).Replanned && queries == 4, "Respawn reset discards cooldown and old route");
Reject(() => follower.Step(float.NaN, Vector3.Zero, target, 2), "Follower rejects nonfinite time");
Reject(() => follower.Step(.1f, new(float.NaN), target, 2), "Follower rejects nonfinite position");
Reject(() => new NavigationPathFollower(replanSeconds: 0), "Follower rejects zero query interval");
var invalidFollower = new NavigationPathFollower((a, b) => new(true, [new(float.NaN)], "bad provider"));
Check(!invalidFollower.Step(.1f, Vector3.Zero, target, 2).HasPath, "Malformed provider output never moves an agent");
Console.WriteLine($"COMBAT_ACTION_TESTS_PASS checks={checks}");
