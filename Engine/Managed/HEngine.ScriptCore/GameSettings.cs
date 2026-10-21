using System.Text.Json;

namespace HEngine;

/// <summary>Project defaults with per-user Saved/Config overrides. Never writes into the installed engine or packaged project.</summary>
public static class GameSettings
{
    /// <summary>Reads merged object settings from a Config/*.json path, limited to 64 KiB per file.</summary>
    public static JsonElement Read(string path) => SceneServices.Call<JsonElement>("settings.read", new { path });
    /// <summary>Atomically saves a settings object under Saved/Config; rejects nonobjects, traversal and values over 64 KiB.</summary>
    public static void Write(string path, IReadOnlyDictionary<string, object> value) => SceneServices.Call<bool>("settings.write", new { path, value });
    /// <summary>Applies supported display preferences; null leaves the corresponding setting unchanged. Returns effective display state.</summary>
    public static JsonElement ApplyDisplay(bool? fullscreen = null, bool? vsync = null, float? uiScale = null)
    {
        var values = new Dictionary<string, object>();
        if (fullscreen is { } full) values["Fullscreen"] = full;
        if (vsync is { } sync) values["VSync"] = sync;
        if (uiScale is { } scale)
        {
            if (!float.IsFinite(scale) || scale < .75f || scale > 1.5f) throw new ArgumentOutOfRangeException(nameof(uiScale));
            values["UIScale"] = scale;
        }
        return SceneServices.Call<JsonElement>("settings.display", values);
    }
}
