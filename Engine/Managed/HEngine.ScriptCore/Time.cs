namespace HEngine;

/// <summary>Engine-owned gameplay frame clock. Single Step advances one game frame while IsPaused remains true.</summary>
public static class Time
{
    /// <summary>Game seconds in the current ordinary frame; zero while paused between steps.</summary>
    public static double DeltaTime { get; private set; }
    /// <summary>Fixed simulation interval in seconds.</summary>
    public const double FixedDeltaTime = 1.0 / 60.0;
    /// <summary>Unscaled seconds supplied by the host for this gameplay frame.</summary>
    public static double UnscaledDeltaTime { get; private set; }
    /// <summary>Number of gameplay frames since this managed session began.</summary>
    public static ulong Frame { get; private set; }
    /// <summary>Whether the editor's gameplay session is paused.</summary>
    public static bool IsPaused { get; private set; }
    /// <summary>Runtime binding only. Called once before the frame's lifecycle phases.</summary>
    public static void BeginFrame(double seconds, bool paused)
    {
        if (!double.IsFinite(seconds) || seconds < 0) throw new ArgumentOutOfRangeException(nameof(seconds));
        DeltaTime = UnscaledDeltaTime = seconds; IsPaused = paused; Frame = checked(Frame + 1);
    }
    /// <summary>Runtime binding only. Pause does not advance gameplay time.</summary>
    public static void SetPaused(bool paused) { IsPaused = paused; if (paused) DeltaTime = 0; }
}
