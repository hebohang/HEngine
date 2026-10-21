using System.Text.Json;

namespace HEngine;

/// <summary>Transactional runtime renderer preferences persisted by the native settings service.</summary>
public static class RenderPreferences
{
    /// <summary>Target material texture payload budget, integer MiB from 1 to 8192; not total GPU memory.</summary>
    public const string TextureStreamingBudgetId = "Rendering.TextureStreaming.BudgetMiB";
    /// <summary>Mode setting: Off or FSR3.1.</summary>
    public const string UpscalingModeId = "Rendering.Upscaling.Mode";
    /// <summary>Quality setting: NativeAA, Quality, Balanced, Performance or UltraPerformance.</summary>
    public const string UpscalingQualityId = "Rendering.Upscaling.Quality";
    /// <summary>FSR reconstruction sharpness setting, from zero to one.</summary>
    public const string UpscalingSharpnessId = "Rendering.Upscaling.Sharpness";
    /// <summary>Last recorded output/internal sizes and actual upscaling/fallback state. May lag a frame after applying settings.</summary>
    public static JsonElement ReadDisplay() => SceneServices.Call<JsonElement>("settings.display", new { });
    /// <summary>Resolved requested/effective values, availability, diagnostics and revision.</summary>
    public static JsonElement Read() => SceneServices.Call<JsonElement>("settings.renderRead", new { });
    /// <summary>Applies stable property IDs atomically to the user layer and saves them.</summary>
    public static JsonElement Apply(IReadOnlyDictionary<string, object> patch) =>
        SceneServices.Call<JsonElement>("settings.renderApply", new { patch });
}

/// <summary>A preloaded, scene-owned mesh/material appearance. Applying validates compatible skin palettes.</summary>
public sealed class PreparedAppearance : IDisposable
{
    private readonly ScriptBehaviour mOwner;
    private readonly IEngineApi mApi;
    private readonly ulong mEntity;
    private ulong mLease;
    /// <summary>Preloads geometry and materials; an optional target also prepares its private deformation and GPU resources during startup.</summary>
    public PreparedAppearance(ScriptBehaviour owner, ResourceReference mesh, string[] materials, Entity target = default)
    {
        mOwner = owner; mApi = Engine.Api; mEntity = Engine.CurrentEntity;
        owner.Lifetime.ThrowIfCancellationRequested();
        if (owner.Entity.Handle != mEntity) throw new InvalidOperationException("Appearance must be prepared in its owner callback.");
        mLease = SceneServices.Call<ulong>("appearance.prepare", new { path = mesh.Id, materials, entity = target.Handle });
        try { owner.OwnResource(this); } catch { Dispose(); throw; }
    }
    /// <summary>Replaces the actor mesh, invalidates animation state and preserves its compatible animation skeleton.</summary>
    public bool Apply(Entity actor)
    {
        if (mLease == 0 || mOwner.Lifetime.IsCancellationRequested) throw new ObjectDisposedException(nameof(PreparedAppearance));
        var previous = Engine.ExchangeEntity(mEntity);
        try { return SceneServices.Call<bool>("appearance.apply", new { entity = actor.Handle, lease = mLease }); }
        finally { Engine.ExchangeEntity(previous); }
    }
    /// <summary>Releases preload references. Already applied appearances remain independently owned by their actors.</summary>
    public void Dispose()
    {
        if (mLease == 0) return;
        var previous = Engine.ExchangeEntity(mEntity);
        try { if (mApi.IsValid(mEntity)) mApi.PlatformService("service.release", JsonSerializer.Serialize(new { kind = "appearance", lease = mLease })); }
        finally { Engine.ExchangeEntity(previous); mLease = 0; mOwner.DisownResource(this); }
    }
}

/// <summary>Restores the exact follow rig, lens and pose after an in-game character/camera preview.</summary>
public sealed class CameraPreview : IDisposable
{
    private readonly OwnedSceneService mState;
    /// <summary>Captures the primary follow camera. Scene/script teardown also restores it.</summary>
    public CameraPreview(ScriptBehaviour owner) => mState = new(owner, "camera-preview", "camera.preview", new { });
    /// <summary>Restores the captured camera; repeated disposal is safe.</summary>
    public void Dispose() => mState.Dispose();
}
