using System.Text.Json;

namespace HEngine;

/// <summary>Presentation services supplied by a scene host, independent of game rules.</summary>
public static class SceneServices
{
    /// <summary>Calls an explicit optional host service. Unsupported services, incompatible results and invalid arguments throw. Gameplay should prefer typed wrappers.</summary>
    public static T Call<T>(string service, object request) =>
        JsonSerializer.Deserialize<T>(Engine.Api.PlatformService(service, JsonSerializer.Serialize(request)))!;
    /// <summary>Expensive scene-wide first-tag search; cache the returned Entity instead of calling every frame. Missing tags return an invalid handle.</summary>
    public static Entity FindTag(string tag) => new(Call<ulong>("scene.findTag", new { tag }));
    /// <summary>Synchronous compatibility prefab service using a stable project-relative path. Prefer PrefabLoad for asynchronous Windows loading.</summary>
    public static Entity Instantiate(string path) => new(Call<ulong>("scene.instantiate", new { path }));
    /// <summary>World-space position, including transform parents and evaluated bone attachments.</summary>
    public static System.Numerics.Vector3 WorldPosition(Entity entity)
    {
        var values = Call<float[]>("scene.worldPosition", new { entity = entity.Handle });
        return new(values[0], values[1], values[2]);
    }
    /// <summary>Current scene primary camera; an absent camera returns an invalid handle.</summary>
    public static Entity Camera => new(Call<ulong>("scene.camera", new { }));
    /// <summary>Starts a stable audio resource and returns a transient playback ID. Unsupported platforms and invalid parameters throw.</summary>
    public static ulong PlayAudio(string path, bool loop = false, float volume = 1) => Call<ulong>("audio.play",new{path,loop,volume});
    /// <summary>Stops a transient audio playback ID; returns false when it is already absent.</summary>
    public static bool StopAudio(ulong handle) => Call<bool>("audio.stop",new{handle});
    /// <summary>Sets a declared animation-controller boolean; unsupported parameters or missing controllers return false or a host diagnostic.</summary>
    public static bool SetAnimationBool(Entity entity, string parameter, bool value) =>
        Call<bool>("animation.set", new { entity = entity.Handle, parameter, @bool = value });
    /// <summary>Sets a declared finite animation-controller number; unsupported parameters or missing controllers return false or a host diagnostic.</summary>
    public static bool SetAnimationFloat(Entity entity, string parameter, float value) =>
        Call<bool>("animation.set", new { entity = entity.Handle, parameter, @float = value });
    /// <summary>Loads a stable UI resource and returns its scene document identity; unavailable UI backends throw.</summary>
    public static string LoadUi(string path) => Call<string>("ui.load", new { path });
    /// <summary>Changes visibility of an existing scene UI document; returns false if absent.</summary>
    public static bool SetUiVisible(string document, bool visible) => Call<bool>("ui.visible", new { document, visible });
    /// <summary>Releases all scene UI documents through the host service.</summary>
    public static void ClearUi() => Call<bool>("ui.clear", new { });
    /// <summary>Sets plain text on a known document element; returns false if the document/element is absent.</summary>
    public static bool SetText(string document, string element, string value) => Call<bool>("ui.text", new { document, element, value });
    /// <summary>Scrolls a visible element into its container without changing keyboard focus.</summary>
    public static bool ScrollIntoView(string document, string element) => Call<bool>("ui.scrollIntoView", new { document, element });
    /// <summary>Sets markup on a known document element; callers own markup validity.</summary>
    public static bool SetMarkup(string document, string element, string value) => Call<bool>("ui.markup", new { document, element, value });
    /// <summary>Sets one declared style property on an existing document element.</summary>
    public static bool SetStyle(string document, string element, string name, string value) => Call<bool>("ui.style", new { document, element, name, value });
    /// <summary>Adds or removes one class on an existing document element.</summary>
    public static bool SetClass(string document, string element, string name, bool enabled) => Call<bool>("ui.class", new { document, element, name, enabled });
    /// <summary>Sets an attribute on an existing document element.</summary>
    public static bool SetAttribute(string document, string element, string name, string value) => Call<bool>("ui.attribute", new { document, element, name, value });
}
