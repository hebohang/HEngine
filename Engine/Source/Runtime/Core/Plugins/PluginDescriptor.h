#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace HEngine
{
    using PluginId = std::string;

    struct PluginVersion
    {
        uint32_t Major = 0;
        uint32_t Minor = 0;
        uint32_t Patch = 0;

        [[nodiscard]] bool operator==(const PluginVersion&) const = default;
    };

    struct PluginApiVersion
    {
        uint32_t Major = 0;
        uint32_t Minor = 0;
        uint32_t Patch = 0;

        constexpr PluginApiVersion() noexcept = default;
        constexpr PluginApiVersion(
            uint32_t major,
            uint32_t minor = 0,
            uint32_t patch = 0) noexcept
            : Major(major)
            , Minor(minor)
            , Patch(patch)
        {
        }

        [[nodiscard]] bool operator==(const PluginApiVersion&) const = default;
    };

    inline constexpr PluginApiVersion c_CurrentPluginApiVersion{ 1, 0, 0 };

    struct PluginVersionRange
    {
        PluginVersion MinimumInclusive{ 1, 0, 0 };
        std::optional<PluginVersion> MaximumExclusive;
    };

    struct PluginDependency
    {
        PluginId Id;
        PluginVersionRange VersionRange;
        PluginApiVersion MinimumApiVersion{ 1, 0, 0 };
    };

    enum class PluginHost : uint32_t
    {
        None = 0,
        Editor = 1u << 0u,
        DevGame = 1u << 1u,
        Game = 1u << 2u,
        Tests = 1u << 3u,
        Tools = 1u << 4u,
        RenderSmoke = 1u << 5u,
    };

    [[nodiscard]] constexpr PluginHost operator|(
        PluginHost left,
        PluginHost right) noexcept
    {
        return static_cast<PluginHost>(
            static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
    }

    [[nodiscard]] constexpr bool HasPluginHost(
        PluginHost hosts,
        PluginHost host) noexcept
    {
        return (static_cast<uint32_t>(hosts) & static_cast<uint32_t>(host)) != 0u;
    }

    enum class PluginDistribution : uint8_t
    {
        Runtime,
        EditorOnly,
        DeveloperOnly,
    };

    enum class PluginBuildDistribution : uint8_t
    {
        Development,
        Shipping,
    };

    enum class PluginPackagePolicy : uint8_t
    {
        Always,
        ProjectSelectable,
        Never,
    };

    enum class PluginPlatform : uint32_t
    {
        None = 0,
        Windows = 1u << 0u,
        Web = 1u << 1u,
        WeChat = 1u << 2u,
        Android = 1u << 3u,
        macOS = 1u << 4u,
    };

    [[nodiscard]] constexpr PluginPlatform GetCurrentPluginPlatform() noexcept
    {
#if defined(__ANDROID__)
        return PluginPlatform::Android;
#elif defined(__APPLE__)
        return PluginPlatform::macOS;
#elif defined(__EMSCRIPTEN__) && defined(HE_WEB)
        return PluginPlatform::Web;
#elif defined(__EMSCRIPTEN__)
        return PluginPlatform::WeChat;
#else
        return PluginPlatform::Windows;
#endif
    }

    [[nodiscard]] constexpr PluginPlatform operator|(
        PluginPlatform left,
        PluginPlatform right) noexcept
    {
        return static_cast<PluginPlatform>(
            static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
    }

    [[nodiscard]] constexpr bool HasPluginPlatform(
        PluginPlatform platforms,
        PluginPlatform platform) noexcept
    {
        return (static_cast<uint32_t>(platforms) &
                static_cast<uint32_t>(platform)) != 0u;
    }

    enum class PluginConfiguration : uint32_t
    {
        None = 0,
        Debug = 1u << 0u,
        RelWithDebInfo = 1u << 1u,
        Release = 1u << 2u,
        MinSizeRel = 1u << 3u,
    };

    [[nodiscard]] constexpr PluginConfiguration operator|(
        PluginConfiguration left,
        PluginConfiguration right) noexcept
    {
        return static_cast<PluginConfiguration>(
            static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
    }

    [[nodiscard]] constexpr bool HasPluginConfiguration(
        PluginConfiguration configurations,
        PluginConfiguration configuration) noexcept
    {
        return (static_cast<uint32_t>(configurations) &
                static_cast<uint32_t>(configuration)) != 0u;
    }

    enum class PluginLoadPhase : uint8_t
    {
        PreDevice,
        PostDevice,
        PostEngine,
        OnDemand,
    };

    struct PluginHostEnvironment
    {
        PluginHost Host = PluginHost::Tests;
        PluginBuildDistribution BuildDistribution =
            PluginBuildDistribution::Development;
        PluginPlatform Platform = GetCurrentPluginPlatform();
        PluginConfiguration Configuration = PluginConfiguration::Debug;
        PluginApiVersion ApiVersion = c_CurrentPluginApiVersion;
    };

    struct PluginDescriptor
    {
        PluginId Id;
        std::string DisplayName;
        std::string ModuleName;
        std::string EntrySymbol;
        PluginVersion Version;
        PluginApiVersion ApiVersion{ 1, 0, 0 };
        PluginApiVersion RequiredHostApiVersion = c_CurrentPluginApiVersion;
        PluginHost SupportedHosts = PluginHost::None;
        PluginDistribution Distribution = PluginDistribution::Runtime;
        PluginLoadPhase LoadPhase = PluginLoadPhase::PostDevice;
        PluginPackagePolicy PackagePolicy =
            PluginPackagePolicy::ProjectSelectable;
        PluginPlatform Platforms = PluginPlatform::None;
        PluginConfiguration Configurations = PluginConfiguration::None;
        std::vector<std::string> ShaderRoots;
        std::vector<std::string> ContentRoots;
        std::vector<PluginDependency> Dependencies;
        std::vector<PluginDependency> OptionalDependencies;
    };

    enum class PluginValidationError : uint8_t
    {
        None,
        InvalidId,
        InvalidDisplayName,
        InvalidModuleName,
        InvalidEntrySymbol,
        InvalidVersion,
        InvalidApiVersion,
        IncompatibleApi,
        InvalidHost,
        UnsupportedHost,
        InvalidDistribution,
        UnsupportedDistribution,
        InvalidPlatform,
        UnsupportedPlatform,
        InvalidConfiguration,
        UnsupportedConfiguration,
        InvalidLoadPhase,
        InvalidPackagePolicy,
        ShippingPolicyViolation,
        InvalidRoot,
        InvalidDependency,
        InvalidDependencyVersion,
        InvalidDependencyApi,
        DuplicateDependency,
    };

    struct PluginValidationResult
    {
        PluginValidationError Error = PluginValidationError::None;
        std::string Message;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return Error == PluginValidationError::None;
        }
    };

    [[nodiscard]] bool IsPluginApiCompatible(
        const PluginApiVersion& host,
        const PluginApiVersion& required) noexcept;

    [[nodiscard]] bool IsPluginVersionInRange(
        const PluginVersion& version,
        const PluginVersionRange& range) noexcept;

    [[nodiscard]] PluginValidationResult ValidatePluginDescriptor(
        const PluginDescriptor& descriptor,
        const PluginHostEnvironment& environment);
}
