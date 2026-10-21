namespace HEngine;

/// <summary>One-step action markers, emitted exactly once even when a step crosses both boundaries.</summary>
/// <param name="Committed">The gameplay marker was crossed this step.</param>
/// <param name="Completed">The end of the action was crossed this step.</param>
public readonly record struct ActionTimelineTick(bool Committed, bool Completed);

/// <summary>Shared time source for normalized animation playback and cancellable gameplay actions.</summary>
public sealed class ActionTimeline
{
    private float mCommitTime;
    /// <summary>Whether the action is running.</summary>
    public bool IsActive { get; private set; }
    /// <summary>Whether its gameplay marker has already fired; retained after cancellation.</summary>
    public bool HasCommitted { get; private set; }
    /// <summary>Current elapsed seconds, bounded by Duration.</summary>
    public float Elapsed { get; private set; }
    /// <summary>Configured action duration.</summary>
    public float Duration { get; private set; }
    /// <summary>Normalized time for a sampled animation layer.</summary>
    public float Progress => Duration > 0 ? Elapsed / Duration : 0;

    /// <summary>Starts a new action. Cancel an existing action explicitly before replacing it.</summary>
    public void Start(float duration, float commitProgress = 1)
    {
        if (IsActive) throw new InvalidOperationException("Cancel the running action before starting another.");
        if (!float.IsFinite(duration) || duration <= 0 || !float.IsFinite(commitProgress) || commitProgress < 0 || commitProgress > 1)
            throw new ArgumentOutOfRangeException(nameof(duration));
        Duration = duration; mCommitTime = duration * commitProgress;
        Elapsed = 0; HasCommitted = false; IsActive = true;
    }

    /// <summary>Stops future markers. Previously committed gameplay is not undone.</summary>
    public void Cancel() => IsActive = false;

    /// <summary>Advances animation and gameplay on the same simulation clock.</summary>
    public ActionTimelineTick Advance(float seconds)
    {
        if (!float.IsFinite(seconds) || seconds < 0) throw new ArgumentOutOfRangeException(nameof(seconds));
        if (!IsActive || seconds == 0) return default;
        Elapsed = MathF.Min(Duration, Elapsed + seconds);
        bool commit = !HasCommitted && Elapsed >= mCommitTime;
        HasCommitted |= commit;
        bool completed = Elapsed >= Duration;
        if (completed) IsActive = false;
        return new(commit, completed);
    }
}
