using System.Numerics;

namespace HEngine;

/// <summary>Engine mix buses. Gameplay participates in gameplay pause.</summary>
public enum AudioBus
{
    /// <summary>Background music.</summary>
    Music,
    /// <summary>Gameplay sounds.</summary>
    Gameplay,
    /// <summary>Menu and interface feedback.</summary>
    UI
}

/// <summary>Spatial distance attenuation.</summary>
public enum AudioAttenuation
{
    /// <summary>No distance attenuation.</summary>
    Disabled,
    /// <summary>Inverse distance.</summary>
    Inverse,
    /// <summary>Linear distance.</summary>
    Linear,
    /// <summary>Exponential distance.</summary>
    Exponential
}

/// <summary>Options copied into a new voice. Positions are world space; angles are radians.</summary>
public sealed record AudioOptions
{
    /// <summary>Mix bus, default Gameplay.</summary>
    public AudioBus Bus { get; init; } = AudioBus.Gameplay;
    /// <summary>Gain in [0, 1].</summary>
    public float Volume { get; init; } = 1;
    /// <summary>Positive playback pitch multiplier.</summary>
    public float Pitch { get; init; } = 1;
    /// <summary>Stereo pan in [-1, 1].</summary>
    public float Pan { get; init; }
    /// <summary>Repeat playback.</summary>
    public bool Loop { get; init; }
    /// <summary>Stream the resource. Null follows Loop.</summary>
    public bool? Stream { get; init; }
    /// <summary>Create the voice paused.</summary>
    public bool StartPaused { get; init; }
    /// <summary>Null selects 2D; a world position selects spatial playback.</summary>
    public Vector3? Position { get; init; }
    /// <summary>Distance at which attenuation starts.</summary>
    public float MinDistance { get; init; } = 1;
    /// <summary>Maximum attenuation distance, at least MinDistance.</summary>
    public float MaxDistance { get; init; } = 100;
    /// <summary>Nonnegative attenuation strength.</summary>
    public float Rolloff { get; init; } = 1;
    /// <summary>Distance curve.</summary>
    public AudioAttenuation Attenuation { get; init; } = AudioAttenuation.Inverse;
    /// <summary>Nonnegative Doppler multiplier.</summary>
    public float DopplerFactor { get; init; } = 1;

    internal Dictionary<string, object> Request(ResourceReference resource)
    {
        var result = new Dictionary<string, object> {
            ["path"] = resource.Id, ["bus"] = (int)Bus, ["volume"] = Volume, ["pitch"] = Pitch,
            ["pan"] = Pan, ["loop"] = Loop, ["stream"] = Stream ?? Loop, ["paused"] = StartPaused,
            ["minDistance"] = MinDistance, ["maxDistance"] = MaxDistance, ["rolloff"] = Rolloff,
            ["attenuation"] = (int)Attenuation, ["dopplerFactor"] = DopplerFactor };
        if (Position is { } position) result["position"] = Physics.Vector(position);
        return result;
    }
}

/// <summary>Native scene audio. Call on the engine thread in a live script callback. Use AudioPlayback for automatic owner cleanup.</summary>
public static class Audio
{
    /// <summary>Starts a voice; zero means device/resource failure. Runtime handles must never be saved.</summary>
    public static ulong Play(ResourceReference resource, AudioOptions? options = null) => SceneServices.Call<ulong>("audio.play", (options ?? new()).Request(resource));
    /// <summary>Stops a voice; false if absent.</summary>
    public static bool Stop(ulong handle) => SceneServices.Call<bool>("audio.stop", new { handle });
    /// <summary>Pauses a voice; false if absent.</summary>
    public static bool Pause(ulong handle) => SceneServices.Call<bool>("audio.pause", new { handle });
    /// <summary>Resumes a voice; false if absent.</summary>
    public static bool Resume(ulong handle) => SceneServices.Call<bool>("audio.resume", new { handle });
    /// <summary>True while actively playing.</summary>
    public static bool IsPlaying(ulong handle) => SceneServices.Call<bool>("audio.isPlaying", new { handle });
    /// <summary>True while paused.</summary>
    public static bool IsPaused(ulong handle) => SceneServices.Call<bool>("audio.isPaused", new { handle });
    /// <summary>Changes gain in [0, 1].</summary>
    public static bool SetVolume(ulong handle, float value) => SceneServices.Call<bool>("audio.volume", new { handle, value });
    /// <summary>Changes positive pitch multiplier.</summary>
    public static bool SetPitch(ulong handle, float value) => SceneServices.Call<bool>("audio.pitch", new { handle, value });
    /// <summary>Changes stereo pan in [-1, 1].</summary>
    public static bool SetPan(ulong handle, float value) => SceneServices.Call<bool>("audio.pan", new { handle, value });
    /// <summary>Moves a spatial voice in world space.</summary>
    public static bool SetPosition(ulong handle, Vector3 value) => SceneServices.Call<bool>("audio.position", new { handle, value = Physics.Vector(value) });
    /// <summary>Sets world velocity for Doppler.</summary>
    public static bool SetVelocity(ulong handle, Vector3 value) => SceneServices.Call<bool>("audio.velocity", new { handle, value = Physics.Vector(value) });
    /// <summary>Sets the spatial cone direction.</summary>
    public static bool SetDirection(ulong handle, Vector3 value) => SceneServices.Call<bool>("audio.direction", new { handle, value = Physics.Vector(value) });
    /// <summary>Sets cone angles in radians (0 &lt;= inner &lt;= outer &lt;= 2 pi) and outer gain in [0, 1].</summary>
    public static bool SetCone(ulong handle, float inner, float outer, float gain) => SceneServices.Call<bool>("audio.cone", new { handle, inner, outer, gain });
    /// <summary>Seeks to nonnegative seconds; false if unsupported or absent.</summary>
    public static bool Seek(ulong handle, double seconds) => SceneServices.Call<bool>("audio.seek", new { handle, seconds });
    /// <summary>Playback cursor in seconds; absent voices return zero.</summary>
    public static double GetPlaybackPosition(ulong handle) => SceneServices.Call<double>("audio.playbackPosition", new { handle });
    /// <summary>Duration in seconds; unavailable duration returns zero.</summary>
    public static double GetLength(ulong handle) => SceneServices.Call<double>("audio.length", new { handle });
    /// <summary>Global gain in [0, 1].</summary>
    public static float MasterVolume { get => SceneServices.Call<float>("audio.masterVolume", new { }); set => SceneServices.Call<float>("audio.masterVolume", new { value }); }
    /// <summary>Reads a bus gain.</summary>
    public static float GetBusVolume(AudioBus bus) => SceneServices.Call<float>("audio.busVolume", new { bus = (int)bus });
    /// <summary>Sets a bus gain in [0, 1].</summary>
    public static void SetBusVolume(AudioBus bus, float value) => SceneServices.Call<float>("audio.busVolume", new { bus = (int)bus, value });
    /// <summary>Stops all voices on a bus.</summary>
    public static void StopBus(AudioBus bus) => SceneServices.Call<bool>("audio.stopBus", new { bus = (int)bus });
    /// <summary>Whether the native audio device is available.</summary>
    public static bool IsDeviceAvailable => SceneServices.Call<bool>("audio.deviceAvailable", new { });
    /// <summary>Last backend diagnostic, possibly empty.</summary>
    public static string LastError => SceneServices.Call<string>("audio.lastError", new { });
    /// <summary>Sets listener 0 immediately. The scene AudioListenerComponent or primary camera overwrites it on the next audio update.</summary>
    public static bool SetListener(Vector3 position, Vector3 forward, Vector3 up, Vector3 velocity = default) => SceneServices.Call<bool>("audio.listener", new {
        position = Physics.Vector(position), forward = Physics.Vector(forward), up = Physics.Vector(up), velocity = Physics.Vector(velocity) });
}
