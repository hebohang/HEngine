using System.Numerics;
using System.Text.Json;

namespace HEngine;

/// <summary>Scene-checked desktop physics operations. Velocities use world units per second.</summary>
public static class Physics
{
    /// <summary>Reads world velocity; false and zero when the body is absent or disabled. Two-dimensional bodies return Z=0.</summary>
    public static bool TryGetVelocity(Entity entity, out Vector3 velocity, bool twoD = false)
    {
        var result = SceneServices.Call<JsonElement>("physics.readVelocity", new { entity = entity.Handle, twoD });
        var value = result.GetProperty("value");
        velocity = new(value[0].GetSingle(), value[1].GetSingle(), value[2].GetSingle());
        return result.GetProperty("found").GetBoolean();
    }
    /// <summary>Applies a world-space center-of-mass force for the next physics step. Call in FixedUpdate for continuous force; do not multiply by delta time. False for absent, disabled, static or kinematic bodies. Two-dimensional bodies ignore Z.</summary>
    public static bool AddForce(Entity entity, Vector3 force, bool twoD = false) =>
        SceneServices.Call<bool>("physics.force", new { entity = entity.Handle, value = Vector(force), twoD });
    /// <summary>Applies a one-off world-space center-of-mass impulse, waking a dynamic body. Do not multiply by delta time. False for absent, disabled, static or kinematic bodies; two-dimensional bodies ignore Z.</summary>
    public static bool AddImpulse(Entity entity, Vector3 impulse, bool twoD = false) =>
        SceneServices.Call<bool>("physics.impulse", new { entity = entity.Handle, value = Vector(impulse), twoD });
    /// <summary>Reads the current collision-driven character stance and world velocity.</summary>
    public static CharacterMotionState CharacterState(Entity entity)
    {
        var value = SceneServices.Call<JsonElement>("physics.characterState", new { entity = entity.Handle });
        var velocity = value.GetProperty("velocity");
        return new(value.GetProperty("grounded").GetBoolean(), value.GetProperty("crouched").GetBoolean(),
            new Vector3(velocity[0].GetSingle(), velocity[1].GetSingle(), velocity[2].GetSingle()));
    }
    /// <summary>Supplies a desired world-space character velocity for the next simulation step; collisions remain engine-owned.</summary>
    public static bool MoveCharacter(Entity entity, Vector3 velocity) =>
        SceneServices.Call<bool>("physics.characterMove", new { entity = entity.Handle, value = Vector(velocity) });
    /// <summary>Sets a live rigid body's velocity. Returns false for a missing body; invalid entities and nonfinite input throw.</summary>
    public static bool SetVelocity(Entity entity, Vector3 velocity, bool twoD = false) =>
        SceneServices.Call<bool>("physics.velocity", new { entity = entity.Handle, value = Vector(velocity), twoD });
    /// <summary>Enables or disables a live rigid body's collisions and queries, retaining its shape for reactivation. Returns false for a missing body.</summary>
    public static bool SetBodyEnabled(Entity entity, bool enabled) =>
        SceneServices.Call<bool>("physics.bodyEnabled", new { entity = entity.Handle, enabled });
    /// <summary>Closest scene collision distance; ignores an optional source rigid body.</summary>
    public static float RaycastDistance(Vector3 origin, Vector3 direction, float distance, Entity ignore = default) =>
        SceneServices.Call<float>("physics.raycastDistance", new { origin = Vector(origin), direction = Vector(direction), distance, entity = ignore.Handle });
    /// <summary>Closest scene collision distance, excluding up to 32 live entities in this scene.</summary>
    public static float RaycastDistance(Vector3 origin, Vector3 direction, float distance, Entity[] ignore)
    {
        ArgumentNullException.ThrowIfNull(ignore);
        if (ignore.Length > 32) throw new ArgumentOutOfRangeException(nameof(ignore), "Ray queries can exclude at most 32 entities.");
        return SceneServices.Call<float>("physics.raycastDistance", new { origin = Vector(origin), direction = Vector(direction), distance,
            entities = ignore.Select(entity => entity.Handle).ToArray() });
    }
    /// <summary>Overrides a character's facing while retaining collision-driven movement.</summary>
    public static bool FaceCharacter(Entity entity, float yaw, bool enabled) =>
        SceneServices.Call<bool>("physics.characterFacing", new { entity = entity.Handle, yaw, enabled });
    internal static float[] Vector(Vector3 value) => [value.X, value.Y, value.Z];
    /// <summary>Casts vertically to find a floor; returns null when the ray misses.</summary>
    public static float? FloorHeight(Vector3 position, float above = 1, float below = 100, Entity ignore = default)
    {
        var result = SceneServices.Call<JsonElement>("physics.floor", new { position = Vector(position), above, below, entity = ignore.Handle });
        return result.GetProperty("found").GetBoolean() ? result.GetProperty("height").GetSingle() : null;
    }
}

/// <summary>Snapshot of a physics character after its most recent simulation step.</summary>
public readonly record struct CharacterMotionState(bool Grounded, bool Crouched, Vector3 Velocity);

/// <summary>Typed animation controller operations on a generation-checked scene entity.</summary>
public readonly struct Animator(Entity entity)
{
    /// <summary>True when the loaded controller declares a parameter with this name.</summary>
    public bool HasParameter(string parameter) => SceneServices.Call<bool>("animation.hasParameter", new { entity = entity.Handle, parameter });
    /// <summary>Reads a declared boolean; missing or mismatched parameter types throw.</summary>
    public bool GetBool(string parameter) => SceneServices.Call<bool>("animation.parameter", new { entity = entity.Handle, parameter, kind = "bool" });
    /// <summary>Reads a declared integer; missing or mismatched parameter types throw.</summary>
    public int GetInt(string parameter) => SceneServices.Call<int>("animation.parameter", new { entity = entity.Handle, parameter, kind = "int" });
    /// <summary>Reads a declared float; missing or mismatched parameter types throw.</summary>
    public float GetFloat(string parameter) => SceneServices.Call<float>("animation.parameter", new { entity = entity.Handle, parameter, kind = "float" });
    /// <summary>Sets a declared integer; false if the component/parameter/type is absent.</summary>
    public bool SetInt(string parameter, int value) => SceneServices.Call<bool>("animation.set", new { entity = entity.Handle, parameter, @int = value });
    /// <summary>Clears a declared trigger; false if the component/parameter/type is absent.</summary>
    public bool ResetTrigger(string parameter) => SceneServices.Call<bool>("animation.set", new { entity = entity.Handle, resetTrigger = parameter });
    /// <summary>Pauses animation evaluation; false if there is no animator.</summary>
    public bool Pause() => SceneServices.Call<bool>("animation.control", new { entity = entity.Handle, action = "pause" });
    /// <summary>Resumes paused animation; does not restart an explicitly stopped animator.</summary>
    public bool Resume() => SceneServices.Call<bool>("animation.control", new { entity = entity.Handle, action = "resume" });
    /// <summary>Stops animation and resets its progress and queued events.</summary>
    public bool Stop() => SceneServices.Call<bool>("animation.control", new { entity = entity.Handle, action = "stop" });
    /// <summary>Restarts the current clip/controller without changing its resource.</summary>
    public bool Play() => SceneServices.Call<bool>("animation.control", new { entity = entity.Handle, action = "play" });
    /// <summary>Sets a finite nonnegative playback speed.</summary>
    public bool SetSpeed(float value) => SceneServices.Call<bool>("animation.control", new { entity = entity.Handle, action = "speed", value });
    /// <summary>Reads current playback state. Playing is the requested play flag, not proof that an animation resource loaded successfully.</summary>
    public AnimationPlaybackState State
    {
        get
        {
            var value = SceneServices.Call<JsonElement>("animation.state", new { entity = entity.Handle });
            return new(value.GetProperty("playing").GetBoolean(), value.GetProperty("paused").GetBoolean(),
                value.GetProperty("speed").GetSingle(), value.GetProperty("progress").GetSingle(), value.GetProperty("state").GetString() ?? "");
        }
    }
    /// <summary>Consumes this frame's base/transition-target clip events once. Blend samples and layers do not duplicate events.</summary>
    public AnimationEvent[] ConsumeEvents() => SceneServices.Call<JsonElement>("animation.events", new { entity = entity.Handle })
        .EnumerateArray().Select(e => new AnimationEvent(e.GetProperty("id").GetString()!, e.GetProperty("name").GetString()!,
            e.GetProperty("payload").GetString()!, e.GetProperty("timeStamp").GetSingle(), e.GetProperty("cycle").GetInt64())).ToArray();
    /// <summary>Shows or hides an entity with a BoneAttachmentComponent.</summary>
    public static bool SetAttachmentVisible(Entity attachment, bool enabled) =>
        SceneServices.Call<bool>("animation.attachment", new { entity = attachment.Handle, enabled });
    /// <summary>Sets every authored foot contact's absolute weight in [0, 1]. Zero clears contact history; scene reload restores authored weights.</summary>
    public bool SetFootContactWeight(float weight) =>
        SceneServices.Call<bool>("animation.footContactWeight", new { entity = entity.Handle, weight });
    /// <summary>Sets a declared boolean parameter.</summary>
    public bool SetBool(string parameter, bool value) => SceneServices.SetAnimationBool(entity, parameter, value);
    /// <summary>Sets a declared finite numeric parameter.</summary>
    public bool SetFloat(string parameter, float value) => SceneServices.SetAnimationFloat(entity, parameter, value);
    /// <summary>Raises a declared controller trigger.</summary>
    public bool Trigger(string parameter) => SceneServices.Call<bool>("animation.set", new { entity = entity.Handle, parameter, trigger = parameter });
    /// <summary>Loads a project-relative animation controller.</summary>
    public bool SetController(ResourceReference resource) => SceneServices.Call<bool>("animation.set", new { entity = entity.Handle, controller = resource.Id });
    /// <summary>Plays a project-relative clip, replacing the current controller.</summary>
    public bool Play(ResourceReference resource, bool loop = true) => SceneServices.Call<bool>("animation.set", new { entity = entity.Handle, clip = resource.Id, loop });
}

/// <summary>One authored animation event, identified by clip-local ID and loop cycle.</summary>
public sealed record AnimationEvent(string Id, string Name, string Payload, float TimeStamp, long Cycle);

/// <summary>Animation play/pause flags, speed, normalized progress and controller state name.</summary>
public readonly record struct AnimationPlaybackState(bool Playing, bool Paused, float Speed, float Progress, string CurrentState);

/// <summary>Controls a persisted scene sequence; the engine restores gameplay camera and input on completion.</summary>
public static class SceneSequence
{
    /// <summary>Starts the sequence on a scene entity; returns false if it is absent or another sequence is playing.</summary>
    public static bool Play(Entity entity) => SceneServices.Call<bool>("sequence.control", new { entity = entity.Handle, play = true });
    /// <summary>Requests that the sequence stop and gameplay state be restored on the next scene update.</summary>
    public static bool Stop(Entity entity) => SceneServices.Call<bool>("sequence.control", new { entity = entity.Handle, play = false });
    /// <summary>Pauses an active sequence; false if it is absent or not playing.</summary>
    public static bool Pause(Entity entity) => SceneServices.Call<bool>("sequence.control", new { entity = entity.Handle, pause = true });
    /// <summary>Resumes an active sequence; false if it is absent or not playing.</summary>
    public static bool Resume(Entity entity) => SceneServices.Call<bool>("sequence.control", new { entity = entity.Handle, pause = false });
    /// <summary>Queues a seek within [0, Duration] for the next sequence update. Returns false if not playing; out-of-range or nonfinite input throws.</summary>
    public static bool Seek(Entity entity, float seconds) => SceneServices.Call<bool>("sequence.control", new { entity = entity.Handle, seek = seconds });
    /// <summary>Reads current playback time and any runtime validation error.</summary>
    public static SequencePlaybackState State(Entity entity)
    {
        var value = SceneServices.Call<JsonElement>("sequence.state", new { entity = entity.Handle });
        return new(value.GetProperty("playing").GetBoolean(), value.GetProperty("paused").GetBoolean(), value.GetProperty("time").GetSingle(),
            value.GetProperty("duration").GetSingle(), value.GetProperty("error").GetString() ?? "");
    }
}

/// <summary>Current scene-sequence playback status.</summary>
public readonly struct SequencePlaybackState
{
    internal SequencePlaybackState(bool playing, bool paused, float time, float duration, string error)
    {
        Playing = playing; Paused = paused; Time = time; Duration = duration; Error = error;
    }
    /// <summary>Whether this sequence is requested to play or currently playing.</summary>
    public bool Playing { get; }
    /// <summary>Whether sequence time is explicitly paused.</summary>
    public bool Paused { get; }
    /// <summary>Current playback time in seconds.</summary>
    public float Time { get; }
    /// <summary>Authored duration in seconds.</summary>
    public float Duration { get; }
    /// <summary>Runtime validation error, or empty when playback is valid.</summary>
    public string Error { get; }
}

/// <summary>Scene transitions are queued and processed at the host's safe frame boundary.</summary>
public static class SceneFlow
{
    /// <summary>Queues a project-relative scene; returns false when another transition is pending.</summary>
    public static bool Load(ResourceReference scene, bool forceReload = false) => SceneServices.Call<bool>("scene.load", new { path = scene.Id, force = forceReload });
}

/// <summary>Primary-camera follow configuration. Angles are radians; pitch bounds are degrees.</summary>
public static class Camera
{
    /// <summary>Sets the primary perspective camera's vertical field of view in [0.1, 2.8] radians; false if absent.</summary>
    public static bool SetFieldOfView(float radians) => SceneServices.Call<bool>("camera.fov", new { radians });
    /// <summary>Returns the primary camera's world-space forward ray through its center.</summary>
    public static CameraRay ForwardRay()
    {
        var result = SceneServices.Call<JsonElement>("camera.ray", new { });
        var origin = result.GetProperty("origin"); var direction = result.GetProperty("direction");
        return new(new(origin[0].GetSingle(), origin[1].GetSingle(), origin[2].GetSingle()),
            new(direction[0].GetSingle(), direction[1].GetSingle(), direction[2].GetSingle()));
    }
    /// <summary>The current primary camera entity.</summary>
    public static Entity Primary => SceneServices.Camera;
    /// <summary>Adjusts an existing follow rig without resetting its orbit or collision state; pitch delta is in radians.</summary>
    public static bool SetSpringArm(float distance, Vector3 socketOffset, Vector3 lookAtOffset, float sensitivity, float pitchDelta = 0) =>
        SceneServices.Call<bool>("camera.springArm", new { distance, socketOffset = Physics.Vector(socketOffset),
            lookAtOffset = Physics.Vector(lookAtOffset), sensitivity, pitchDelta });
    /// <summary>Follows an entity through the scene camera's spring arm.</summary>
    public static bool Follow(Entity target, float distance = 6, float yaw = 0, float pitch = -.4f, float targetHeight = 1, float lagSpeed = 10, bool reset = false) =>
        SceneServices.Call<bool>("camera.follow", new { entity = target.Handle, enabled = true, distance, yaw, pitch, targetHeight,
            socketHeight = 0, lagSpeed, minDistance = .2f, minPitch = -80, maxPitch = 80, reset });
    /// <summary>Clears the primary camera's follow target.</summary>
    public static bool StopFollowing() => SceneServices.Call<bool>("camera.follow", new { enabled = false });
}

/// <summary>A world-space camera ray with a normalized direction.</summary>
public readonly record struct CameraRay(Vector3 Origin, Vector3 Direction);

/// <summary>Complete path result; an unreachable target has no partial points.</summary>
public sealed record NavigationPath(bool Success, Vector3[] Points, string Diagnostic);

/// <summary>Per-scene Recast navigation using the project's persisted agent and area settings.</summary>
public static class Navigation
{
    /// <summary>Rebuilds from current scene geometry, retaining the old mesh on failure.</summary>
    public static NavigationPath Build()
    {
        var value = SceneServices.Call<JsonElement>("navigation.build", new { });
        return new(value.GetProperty("success").GetBoolean(), [], value.GetProperty("diagnostic").GetString() ?? "");
    }
    /// <summary>Rebuilds only the world-space box. Geometry crossing its boundary is clipped.</summary>
    public static NavigationPath Build(Vector3 minimum, Vector3 maximum)
    {
        var value = SceneServices.Call<JsonElement>("navigation.build", new {
            minimum = Physics.Vector(minimum), maximum = Physics.Vector(maximum) });
        return new(value.GetProperty("success").GetBoolean(), [], value.GetProperty("diagnostic").GetString() ?? "");
    }
    /// <summary>Finds a complete path after Build; failed queries contain an actionable diagnostic.</summary>
    public static NavigationPath FindPath(Vector3 start, Vector3 end)
    {
        var value = SceneServices.Call<JsonElement>("navigation.path", new { start = Physics.Vector(start), end = Physics.Vector(end) });
        var points = value.GetProperty("points").EnumerateArray().Select(p => new Vector3(p[0].GetSingle(), p[1].GetSingle(), p[2].GetSingle())).ToArray();
        return new(value.GetProperty("success").GetBoolean(), points, value.GetProperty("diagnostic").GetString() ?? "");
    }
}

internal sealed class OwnedSceneService : IDisposable
{
    private readonly ScriptBehaviour mOwner;
    private readonly IEngineApi mApi;
    private readonly ulong mEntity;
    private readonly string mKind;
    private ulong mLease;
    public string Document { get; } = "";
    public OwnedSceneService(ScriptBehaviour owner, string kind, string operation, object request)
    {
        ArgumentNullException.ThrowIfNull(owner);
        owner.Lifetime.ThrowIfCancellationRequested();
        mOwner = owner; mApi = Engine.Api; mEntity = Engine.CurrentEntity; mKind = kind;
        if (owner.Entity.Handle != mEntity || mEntity == 0) throw new InvalidOperationException("Playback must be created in its owner's callback.");
        if (kind == "ui")
        {
            var result = Call<JsonElement>(operation, request);
            mLease = result.GetProperty("lease").GetUInt64(); Document = result.GetProperty("document").GetString()!;
        }
        else mLease = Call<ulong>(operation, request);
        try { owner.OwnResource(this); } catch { Dispose(); throw; }
    }
    private T Call<T>(string operation, object request)
    {
        var previous = Engine.ExchangeEntity(mEntity);
        try { return JsonSerializer.Deserialize<T>(mApi.PlatformService(operation, JsonSerializer.Serialize(request)))!; }
        finally { Engine.ExchangeEntity(previous); }
    }
    public bool Update(Vector3 position, Vector3 forward, bool paused, Vector4? color = null)
    {
        if (mLease == 0 || mOwner.Lifetime.IsCancellationRequested) throw new ObjectDisposedException(mKind);
        float[]? tint = color is { } value ? [value.X, value.Y, value.Z, value.W] : null;
        return Call<bool>("vfx.ownedUpdate", new { lease = mLease, position = Physics.Vector(position), forward = Physics.Vector(forward), paused, color = tint });
    }
    public bool Ui(string operation, object request)
    {
        if (mLease == 0 || mOwner.Lifetime.IsCancellationRequested) throw new ObjectDisposedException(mKind);
        return Call<bool>(operation, request);
    }
    public T Audio<T>(string operation, object? value = null, double? seconds = null)
    {
        if (mLease == 0 || mOwner.Lifetime.IsCancellationRequested) throw new ObjectDisposedException(mKind);
        return Call<T>("audio." + operation, new { lease = mLease, value, seconds });
    }
    public void Dispose()
    {
        if (mLease == 0) return;
        if (mApi.IsValid(mEntity)) Call<bool>("service.release", new { kind = mKind, lease = mLease });
        mLease = 0; mOwner.DisownResource(this);
    }
}

/// <summary>Audio playback owned by a script and released when that script or scene ends.</summary>
public sealed class AudioPlayback : IDisposable
{
    private readonly OwnedSceneService mPlayback;
    private AudioPlayback(ScriptBehaviour owner, ResourceReference resource, bool loop, float volume) =>
        mPlayback = new(owner, "audio", "audio.ownedPlay", new { path = resource.Id, loop, volume });
    private AudioPlayback(ScriptBehaviour owner, ResourceReference resource, AudioOptions options) =>
        mPlayback = new(owner, "audio", "audio.ownedPlay", options.Request(resource));
    /// <summary>Starts a sound; resource/load failures throw at the script boundary.</summary>
    public static AudioPlayback Play(ScriptBehaviour owner, ResourceReference resource, bool loop = false, float volume = 1) => new(owner, resource, loop, volume);
    /// <summary>Starts a configured 2D or spatial voice owned by this script.</summary>
    public static AudioPlayback Play(ScriptBehaviour owner, ResourceReference resource, AudioOptions options) => new(owner, resource, options);
    /// <summary>Pauses the owned voice.</summary>
    public bool Pause() => mPlayback.Audio<bool>("pause");
    /// <summary>Resumes the owned voice.</summary>
    public bool Resume() => mPlayback.Audio<bool>("resume");
    /// <summary>True while playing; throws after disposal.</summary>
    public bool IsPlaying => mPlayback.Audio<bool>("isPlaying");
    /// <summary>True while paused; throws after disposal.</summary>
    public bool IsPaused => mPlayback.Audio<bool>("isPaused");
    /// <summary>Changes gain in [0, 1].</summary>
    public bool SetVolume(float value) => mPlayback.Audio<bool>("volume", value);
    /// <summary>Changes the positive pitch multiplier.</summary>
    public bool SetPitch(float value) => mPlayback.Audio<bool>("pitch", value);
    /// <summary>Changes stereo pan in [-1, 1].</summary>
    public bool SetPan(float value) => mPlayback.Audio<bool>("pan", value);
    /// <summary>Moves a spatial voice in world space.</summary>
    public bool SetPosition(Vector3 value) => mPlayback.Audio<bool>("position", Physics.Vector(value));
    /// <summary>Changes world velocity for Doppler.</summary>
    public bool SetVelocity(Vector3 value) => mPlayback.Audio<bool>("velocity", Physics.Vector(value));
    /// <summary>Seeks to nonnegative seconds; false if unsupported or the voice has ended.</summary>
    public bool Seek(double seconds) => mPlayback.Audio<bool>("seek", seconds: seconds);
    /// <summary>Current playback cursor in seconds.</summary>
    public double PlaybackPosition => mPlayback.Audio<double>("playbackPosition");
    /// <summary>Duration in seconds, or zero when unavailable.</summary>
    public double Length => mPlayback.Audio<double>("length");
    /// <summary>Stops playback. Repeated disposal is safe.</summary>
    public void Dispose() => mPlayback.Dispose();
}

/// <summary>Effekseer playback owned by a script and released on script/scene teardown.</summary>
public sealed class VfxPlayback : IDisposable
{
    private readonly OwnedSceneService mPlayback;
    private VfxPlayback(ScriptBehaviour owner, ResourceReference resource, Vector3 position, Vector3 forward, float scale, Vector4? color)
    {
        float[]? tint = color is { } value ? [value.X, value.Y, value.Z, value.W] : null;
        mPlayback = new(owner, "vfx", "vfx.ownedPlay", new { path = resource.Id, position = Physics.Vector(position), forward = Physics.Vector(forward), scale, color = tint });
    }
    /// <summary>Starts a project effect at a world-space transform.</summary>
    public static VfxPlayback Play(ScriptBehaviour owner, ResourceReference resource, Vector3 position, Vector3 forward, float scale = 1, Vector4? color = null) => new(owner, resource, position, forward, scale, color);
    /// <summary>Updates a live effect's transform, pause state, and optional global color; returns false after natural completion.</summary>
    public bool Update(Vector3 position, Vector3 forward, bool paused = false, Vector4? color = null) => mPlayback.Update(position, forward, paused, color);
    /// <summary>Stops playback. Repeated disposal is safe.</summary>
    public void Dispose() => mPlayback.Dispose();
}

/// <summary>Scene UI document released automatically with its script owner.</summary>
public sealed class UiDocument : IDisposable
{
    private readonly OwnedSceneService mDocument;
    /// <summary>Scene-local identity for advanced element, cursor and focus services.</summary>
    public string DocumentId => mDocument.Document;
    private UiDocument(ScriptBehaviour owner, ResourceReference resource) => mDocument = new(owner, "ui", "ui.ownedLoad", new { path = resource.Id });
    /// <summary>Loads and shows a .hui or authored .rml resource in the current scene.</summary>
    public static UiDocument Load(ScriptBehaviour owner, ResourceReference resource) => new(owner, resource);
    /// <summary>Shows or hides this document.</summary>
    public bool SetVisible(bool visible) => mDocument.Ui("ui.visible", new { document = mDocument.Document, visible });
    /// <summary>Writes plain text into an element.</summary>
    public bool SetText(string element, string value) => mDocument.Ui("ui.text", new { document = mDocument.Document, element, value });
    /// <summary>Changes one element style property.</summary>
    public bool SetStyle(string element, string name, string value) => mDocument.Ui("ui.style", new { document = mDocument.Document, element, name, value });
    /// <summary>Adds or removes a CSS class.</summary>
    public bool SetClass(string element, string name, bool enabled) => mDocument.Ui("ui.class", new { document = mDocument.Document, element, name, enabled });
    /// <summary>Writes authored markup into an element; callers own markup validity.</summary>
    public bool SetMarkup(string element, string value) => mDocument.Ui("ui.markup", new { document = mDocument.Document, element, value });
    /// <summary>Sets an element attribute.</summary>
    public bool SetAttribute(string element, string name, string value) => mDocument.Ui("ui.attribute", new { document = mDocument.Document, element, name, value });
    /// <summary>Scrolls an element into its container without changing keyboard focus.</summary>
    public bool ScrollIntoView(string element) => mDocument.Ui("ui.scrollIntoView", new { document = mDocument.Document, element });
    /// <summary>Shows or hides this document's mouse cursor.</summary>
    public bool SetCursorVisible(bool visible) => mDocument.Ui("ui.cursor", new { document = mDocument.Document, visible });
    /// <summary>Unloads the document; repeated disposal is safe.</summary>
    public void Dispose() => mDocument.Dispose();
}
