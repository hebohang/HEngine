using System.Collections.Generic;
using System.Diagnostics;

namespace HEngine;

/// <summary>A bounded, engine-scheduled wait. Conditions run on the engine thread.</summary>
public readonly struct ScriptWait
{
    internal enum Kind { Frame, Fixed, GameTime, RealTime, Condition }
    internal readonly Kind Mode;
    internal readonly double Seconds;
    internal readonly Func<bool>? Predicate;
    private ScriptWait(Kind mode, double seconds = 0, Func<bool>? predicate = null)
    {
        if (!double.IsFinite(seconds) || seconds < 0) throw new ArgumentOutOfRangeException(nameof(seconds));
        Mode = mode; Seconds = seconds; Predicate = predicate;
    }
    /// <summary>Continues on the next ordinary update.</summary>
    public static ScriptWait NextFrame => new(Kind.Frame);
    /// <summary>Continues at the next fixed update.</summary>
    public static ScriptWait FixedUpdate => new(Kind.Fixed);
    /// <summary>Waits for enabled script game time; pause does not advance it.</summary>
    public static ScriptWait GameSeconds(double seconds) => new(Kind.GameTime, seconds);
    /// <summary>Waits for elapsed wall time; resumes at the next enabled update.</summary>
    public static ScriptWait RealSeconds(double seconds) => new(Kind.RealTime, seconds);
    /// <summary>Evaluates a condition at most once per enabled ordinary update.</summary>
    public static ScriptWait Until(Func<bool> predicate) => new(Kind.Condition, predicate: predicate ?? throw new ArgumentNullException(nameof(predicate)));
}

public abstract partial class ScriptBehaviour
{
    private sealed class Routine(IEnumerator<ScriptWait> iterator)
    {
        public readonly IEnumerator<ScriptWait> Iterator = iterator;
        public ScriptWait Wait;
        public double Remaining;
        public long Since;
    }
    private readonly List<Routine> mRoutines = [];
    private readonly List<(Task Task, string Origin)> mTasks = [];
    private readonly List<IDisposable> mOwnedResources = [];
    private readonly CancellationTokenSource mLifetime = new();
    private bool mReleased;
    private bool mTicking;

    /// <summary>Cancelled before final engine release. Pass it to every background operation.</summary>
    public CancellationToken Lifetime => mLifetime.Token;

    internal void OwnResource(IDisposable resource)
    {
        if (mReleased) throw new ObjectDisposedException(GetType().FullName);
        if (mOwnedResources.Count >= 1024) throw new InvalidOperationException("Owned resource limit (1024) reached.");
        mOwnedResources.Add(resource);
    }
    internal void DisownResource(IDisposable resource) => mOwnedResources.Remove(resource);

    /// <summary>Runtime shutdown snapshot, engine thread only. Used for one bounded cancellation grace period across all retired scripts.</summary>
    public Task[] PendingTasksForShutdown() => mTasks.Where(item => !item.Task.IsCompleted).Select(item => item.Task).ToArray();

    /// <summary>Starts a typed coroutine. The engine owns and disposes its iterator on release.</summary>
    public void StartCoroutine(IEnumerator<ScriptWait> iterator)
    {
        if (Engine.IsValidating) throw new InvalidOperationException("Coroutines cannot be scheduled during Reset/OnValidate.");
        ArgumentNullException.ThrowIfNull(iterator);
        if (mReleased) throw new ObjectDisposedException(GetType().FullName);
        if (mRoutines.Count >= 1024) throw new InvalidOperationException("Coroutine limit (1024) reached.");
        var routine = new Routine(iterator);
        try
        {
            if (!iterator.MoveNext()) { iterator.Dispose(); return; }
            Arm(routine);
            mRoutines.Add(routine);
        }
        catch { iterator.Dispose(); throw; }
    }

    /// <summary>Registers a task that uses Lifetime. Engine APIs remain restricted to the engine thread.</summary>
    public void TrackTask(Task task, string origin)
    {
        if (Engine.IsValidating) throw new InvalidOperationException("Tasks cannot be scheduled during Reset/OnValidate.");
        ArgumentNullException.ThrowIfNull(task);
        if (mReleased) throw new ObjectDisposedException(GetType().FullName);
        mTasks.RemoveAll(item => item.Task.IsCompletedSuccessfully || item.Task.IsCanceled);
        if (mTasks.Count >= 1024) throw new InvalidOperationException("Tracked task limit (1024) reached.");
        if (string.IsNullOrWhiteSpace(origin) || origin.Length > 256) throw new ArgumentException("Task origin must contain 1–256 characters.", nameof(origin));
        mTasks.Add((task, origin));
    }

    private static void Arm(Routine routine)
    {
        routine.Wait = routine.Iterator.Current;
        routine.Remaining = routine.Wait.Seconds;
        routine.Since = Stopwatch.GetTimestamp();
    }

    /// <summary>Runtime dispatch only; advances a snapshot so newly queued routines wait until the next phase.</summary>
    public void AdvanceScheduled(bool fixedUpdate, double seconds)
    {
        if (mReleased) return;
        if (mTicking) throw new InvalidOperationException("Recursive coroutine scheduling.");
        if (!double.IsFinite(seconds) || seconds < 0) throw new ArgumentOutOfRangeException(nameof(seconds));
        mTicking = true;
        try
        {
            foreach (var routine in mRoutines.ToArray())
            {
                if (mReleased) break;
                var wait = routine.Wait;
                if ((wait.Mode == ScriptWait.Kind.Fixed) != fixedUpdate) continue;
                bool ready = wait.Mode switch
                {
                    ScriptWait.Kind.GameTime => (routine.Remaining -= seconds) <= 0,
                    ScriptWait.Kind.RealTime => Stopwatch.GetElapsedTime(routine.Since).TotalSeconds >= wait.Seconds,
                    ScriptWait.Kind.Condition => wait.Predicate!(),
                    _ => true
                };
                if (!ready) continue;
                if (routine.Iterator.MoveNext()) Arm(routine);
                else { mRoutines.Remove(routine); routine.Iterator.Dispose(); }
            }
        }
        finally { mTicking = false; }
    }

    /// <summary>Cancels work and disposes every iterator even when cleanup throws. Returns diagnostic strings only.</summary>
    public string[] ReleaseScheduled()
    {
        var errors = new List<string>();
        if (!mReleased)
        {
            mReleased = true;
            try { mLifetime.Cancel(); } catch (Exception error) { errors.Add("Cancellation: " + error.Message); }
            foreach (var routine in mRoutines)
                try { routine.Iterator.Dispose(); } catch (Exception error) { errors.Add("Coroutine disposal: " + error.Message); }
            mRoutines.Clear();
            foreach (var resource in mOwnedResources.ToArray())
                try { resource.Dispose(); } catch (Exception error) { errors.Add("Resource release: " + error.Message); }
            mOwnedResources.Clear();
        }
        foreach (var item in mTasks)
        {
            if (!item.Task.IsCompleted) errors.Add($"Task {item.Task.Id} still running after cancellation: {item.Origin}");
            else if (item.Task.IsFaulted) errors.Add($"Task {item.Task.Id} failed: {item.Origin}: {item.Task.Exception?.GetBaseException().Message}");
        }
        mTasks.RemoveAll(item => item.Task.IsCompleted);
        return errors.ToArray();
    }
}
