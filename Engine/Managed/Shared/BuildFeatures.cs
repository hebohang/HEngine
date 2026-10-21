namespace HEngine.Platform;

/// <summary>Immutable export identity. Quality controls presentation, never save or gameplay rules.</summary>
public static class BuildFeatures
{
#if HE_WINDOWS
    public const string Target = "Windows";
#elif HE_MACOS
    public const string Target = "macOS";
#elif HE_WEB
    public const string Target = "Web";
#elif HE_ANDROID
    public const string Target = "Android";
#elif HE_WECHAT
    public const string Target = "WeChat";
#else
#error Import Engine/Managed/Platform.props before compiling shared gameplay.
#endif
#if HE_QUALITY_DESKTOP
    public const string Quality = "Desktop";
    public const int TextureLimit = 2048;
    public const int PresentationParticleLimit = 1024;
#else
    public const string Quality = "Portable";
    public const int TextureLimit = 512;
    public const int PresentationParticleLimit = 128;
#endif
    // Web may also run with a mouse; the host supplies actual device capabilities.
    public static bool HasFeature(string feature) => feature switch
    {
        "android" => Target == "Android",
        "windows" => Target == "Windows",
        "macos" => Target == "macOS",
        "web" => Target == "Web",
        "wechat" => Target == "WeChat",
        "desktop_quality" => Quality == "Desktop",
        "portable_quality" => Quality == "Portable",
        _ => false
    };
}
