#include "Hepch.h"

#include "Runtime/Core/Plugins/PluginDescriptor.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace HEngine
{
    namespace
    {
        [[nodiscard]] bool IsValidPluginId(const std::string& value) noexcept
        {
            if (value.empty() || value.size() > 128)
                return false;
            const auto isLowerAlphaNumeric = [](unsigned char character)
            {
                return (character >= 'a' && character <= 'z') ||
                    (character >= '0' && character <= '9');
            };
            if (!isLowerAlphaNumeric(static_cast<unsigned char>(value.front())))
                return false;
            for (unsigned char character : value)
            {
                if (!isLowerAlphaNumeric(character) && character != '.' &&
                    character != '_' && character != '-')
                    return false;
            }
            return value.back() != '.';
        }

        [[nodiscard]] bool IsValidDisplayName(const std::string& value) noexcept
        {
            if (value.empty() || value.size() > 128)
                return false;
            return std::none_of(value.begin(), value.end(), [](unsigned char character)
            {
                return character < 0x20u || character == 0x7fu;
            });
        }

        [[nodiscard]] bool IsValidModuleName(const std::string& value) noexcept
        {
            if (value.empty() || value.size() > 128)
                return false;
            const auto isAlpha = [](unsigned char character)
            {
                return (character >= 'a' && character <= 'z') ||
                    (character >= 'A' && character <= 'Z');
            };
            if (!isAlpha(static_cast<unsigned char>(value.front())))
                return false;
            return std::all_of(value.begin() + 1, value.end(), [isAlpha](unsigned char character)
            {
                return isAlpha(character) ||
                    (character >= '0' && character <= '9') ||
                    character == '_';
            });
        }

        [[nodiscard]] bool IsValidEntrySymbol(const std::string& value) noexcept
        {
            if (value.empty() || value.size() > 128)
                return false;
            const auto isAlphaOrUnderscore = [](unsigned char character)
            {
                return (character >= 'a' && character <= 'z') ||
                    (character >= 'A' && character <= 'Z') || character == '_';
            };
            if (!isAlphaOrUnderscore(static_cast<unsigned char>(value.front())))
                return false;
            return std::all_of(
                value.begin() + 1,
                value.end(),
                [isAlphaOrUnderscore](unsigned char character)
                {
                    return isAlphaOrUnderscore(character) ||
                        (character >= '0' && character <= '9');
                });
        }

        [[nodiscard]] bool IsSafeRelativeRoot(const std::string& value) noexcept
        {
            if (value.empty() || value.size() > 260 || value.front() == '/' ||
                value.find('\\') != std::string::npos ||
                value.find(':') != std::string::npos)
                return false;
            size_t begin = 0;
            while (begin < value.size())
            {
                const size_t end = value.find('/', begin);
                const size_t componentEnd = end == std::string::npos
                    ? value.size()
                    : end;
                const std::string_view component(
                    value.data() + begin,
                    componentEnd - begin);
                if (component.empty() || component == "." || component == "..")
                    return false;
                if (std::any_of(
                        component.begin(),
                        component.end(),
                        [](unsigned char character)
                        {
                            return character < 0x20u || character == 0x7fu;
                        }))
                    return false;
                if (end == std::string::npos)
                    break;
                begin = end + 1;
            }
            return value.back() != '/';
        }

        [[nodiscard]] bool IsSingleHost(PluginHost host) noexcept
        {
            const uint32_t value = static_cast<uint32_t>(host);
            constexpr uint32_t ValidHosts =
                static_cast<uint32_t>(PluginHost::Editor) |
                static_cast<uint32_t>(PluginHost::DevGame) |
                static_cast<uint32_t>(PluginHost::Game) |
                static_cast<uint32_t>(PluginHost::Tests) |
                static_cast<uint32_t>(PluginHost::Tools) |
                static_cast<uint32_t>(PluginHost::RenderSmoke);
            return value != 0u && (value & (value - 1u)) == 0u &&
                (value & ~ValidHosts) == 0u;
        }

        [[nodiscard]] bool IsValidHostMask(PluginHost hosts) noexcept
        {
            constexpr uint32_t ValidHosts =
                static_cast<uint32_t>(PluginHost::Editor) |
                static_cast<uint32_t>(PluginHost::DevGame) |
                static_cast<uint32_t>(PluginHost::Game) |
                static_cast<uint32_t>(PluginHost::Tests) |
                static_cast<uint32_t>(PluginHost::Tools) |
                static_cast<uint32_t>(PluginHost::RenderSmoke);
            const uint32_t value = static_cast<uint32_t>(hosts);
            return value != 0u && (value & ~ValidHosts) == 0u;
        }

        [[nodiscard]] bool IsValidPlatformMask(PluginPlatform platforms) noexcept
        {
            const uint32_t value = static_cast<uint32_t>(platforms);
            constexpr uint32_t Valid =
                static_cast<uint32_t>(PluginPlatform::Windows | PluginPlatform::Web | PluginPlatform::WeChat | PluginPlatform::Android | PluginPlatform::macOS);
            return value != 0u && (value & ~Valid) == 0u;
        }

        [[nodiscard]] bool IsSingleConfiguration(
            PluginConfiguration configuration) noexcept
        {
            const uint32_t value = static_cast<uint32_t>(configuration);
            constexpr uint32_t Valid =
                static_cast<uint32_t>(PluginConfiguration::Debug) |
                static_cast<uint32_t>(PluginConfiguration::RelWithDebInfo) |
                static_cast<uint32_t>(PluginConfiguration::Release) |
                static_cast<uint32_t>(PluginConfiguration::MinSizeRel);
            return value != 0u && (value & (value - 1u)) == 0u &&
                (value & ~Valid) == 0u;
        }

        [[nodiscard]] bool IsValidConfigurationMask(
            PluginConfiguration configurations) noexcept
        {
            const uint32_t value = static_cast<uint32_t>(configurations);
            constexpr uint32_t Valid =
                static_cast<uint32_t>(PluginConfiguration::Debug) |
                static_cast<uint32_t>(PluginConfiguration::RelWithDebInfo) |
                static_cast<uint32_t>(PluginConfiguration::Release) |
                static_cast<uint32_t>(PluginConfiguration::MinSizeRel);
            return value != 0u && (value & ~Valid) == 0u;
        }

        [[nodiscard]] int CompareVersion(
            const PluginVersion& left,
            const PluginVersion& right) noexcept
        {
            if (left.Major != right.Major)
                return left.Major < right.Major ? -1 : 1;
            if (left.Minor != right.Minor)
                return left.Minor < right.Minor ? -1 : 1;
            if (left.Patch != right.Patch)
                return left.Patch < right.Patch ? -1 : 1;
            return 0;
        }

        [[nodiscard]] PluginValidationResult Failure(
            PluginValidationError error,
            std::string message)
        {
            return { .Error = error, .Message = std::move(message) };
        }

        [[nodiscard]] PluginValidationResult ValidateDependency(
            const PluginDescriptor& descriptor,
            const PluginDependency& dependency,
            std::unordered_set<PluginId>& dependencyIds)
        {
            if (!IsValidPluginId(dependency.Id) || dependency.Id == descriptor.Id)
                return Failure(PluginValidationError::InvalidDependency,
                    "Plugin dependency is invalid or self-referential: " + descriptor.Id);
            if (!dependencyIds.insert(dependency.Id).second)
                return Failure(PluginValidationError::DuplicateDependency,
                    "Plugin dependency is duplicated across required/optional lists: " +
                        descriptor.Id + " -> " + dependency.Id);
            if (dependency.VersionRange.MinimumInclusive.Major == 0 ||
                (dependency.VersionRange.MaximumExclusive &&
                    CompareVersion(
                        dependency.VersionRange.MinimumInclusive,
                        *dependency.VersionRange.MaximumExclusive) >= 0))
                return Failure(PluginValidationError::InvalidDependencyVersion,
                    "Plugin dependency version range is invalid: " +
                        descriptor.Id + " -> " + dependency.Id);
            if (dependency.MinimumApiVersion.Major == 0)
                return Failure(PluginValidationError::InvalidDependencyApi,
                    "Plugin dependency API requirement is invalid: " +
                        descriptor.Id + " -> " + dependency.Id);
            return {};
        }

        [[nodiscard]] PluginValidationResult ValidateRoots(
            const PluginDescriptor& descriptor,
            const std::vector<std::string>& roots,
            std::string_view kind)
        {
            std::unordered_set<std::string> uniqueRoots;
            for (const std::string& root : roots)
            {
                if (!IsSafeRelativeRoot(root) || !uniqueRoots.insert(root).second)
                    return Failure(PluginValidationError::InvalidRoot,
                        "Plugin " + std::string(kind) +
                            " root is unsafe or duplicated: " + descriptor.Id +
                            " -> " + root);
            }
            return {};
        }
    }

    bool IsPluginApiCompatible(
        const PluginApiVersion& host,
        const PluginApiVersion& required) noexcept
    {
        if (host.Major == 0 || required.Major == 0 ||
            host.Major != required.Major)
            return false;
        if (host.Minor != required.Minor)
            return host.Minor > required.Minor;
        return host.Patch >= required.Patch;
    }

    bool IsPluginVersionInRange(
        const PluginVersion& version,
        const PluginVersionRange& range) noexcept
    {
        if (CompareVersion(version, range.MinimumInclusive) < 0)
            return false;
        return !range.MaximumExclusive ||
            CompareVersion(version, *range.MaximumExclusive) < 0;
    }

    PluginValidationResult ValidatePluginDescriptor(
        const PluginDescriptor& descriptor,
        const PluginHostEnvironment& environment)
    {
        if (!IsValidPluginId(descriptor.Id))
            return Failure(PluginValidationError::InvalidId,
                "Plugin Id must be a canonical lowercase identifier: " + descriptor.Id);
        if (!IsValidDisplayName(descriptor.DisplayName))
            return Failure(PluginValidationError::InvalidDisplayName,
                "Plugin display name is invalid: " + descriptor.Id);
        if (!IsValidModuleName(descriptor.ModuleName))
            return Failure(PluginValidationError::InvalidModuleName,
                "Plugin module name is invalid: " + descriptor.Id);
        if (!IsValidEntrySymbol(descriptor.EntrySymbol))
            return Failure(PluginValidationError::InvalidEntrySymbol,
                "Plugin entry symbol is not a C++ identifier: " + descriptor.Id);
        if (descriptor.Version.Major == 0)
            return Failure(PluginValidationError::InvalidVersion,
                "Plugin version major must be non-zero: " + descriptor.Id);
        if (descriptor.ApiVersion.Major == 0 ||
            descriptor.RequiredHostApiVersion.Major == 0 ||
            environment.ApiVersion.Major == 0)
            return Failure(PluginValidationError::InvalidApiVersion,
                "Plugin API versions must have a non-zero major: " + descriptor.Id);
        if (!IsPluginApiCompatible(
                environment.ApiVersion,
                descriptor.RequiredHostApiVersion))
            return Failure(PluginValidationError::IncompatibleApi,
                "Plugin requires an incompatible host API version: " + descriptor.Id);
        if (!IsSingleHost(environment.Host) ||
            !IsValidHostMask(descriptor.SupportedHosts))
            return Failure(PluginValidationError::InvalidHost,
                "Plugin host selection or mask is invalid: " + descriptor.Id);
        if (!HasPluginHost(descriptor.SupportedHosts, environment.Host))
            return Failure(PluginValidationError::UnsupportedHost,
                "Plugin does not support the selected host: " + descriptor.Id);

        switch (descriptor.Distribution)
        {
        case PluginDistribution::Runtime:
        case PluginDistribution::EditorOnly:
        case PluginDistribution::DeveloperOnly:
            break;
        default:
            return Failure(PluginValidationError::InvalidDistribution,
                "Plugin distribution is invalid: " + descriptor.Id);
        }
        switch (environment.BuildDistribution)
        {
        case PluginBuildDistribution::Development:
        case PluginBuildDistribution::Shipping:
            break;
        default:
            return Failure(PluginValidationError::InvalidDistribution,
                "Plugin host build distribution is invalid.");
        }
        if (descriptor.Distribution == PluginDistribution::EditorOnly &&
            descriptor.SupportedHosts != PluginHost::Editor)
            return Failure(PluginValidationError::UnsupportedDistribution,
                "EditorOnly plugin may support only the Editor host: " + descriptor.Id);
        if (environment.BuildDistribution == PluginBuildDistribution::Shipping &&
            descriptor.Distribution == PluginDistribution::DeveloperOnly)
            return Failure(PluginValidationError::ShippingPolicyViolation,
                "DeveloperOnly plugin cannot be registered in Shipping: " + descriptor.Id);

        if ((environment.Platform != PluginPlatform::Windows && environment.Platform != PluginPlatform::Web && environment.Platform != PluginPlatform::WeChat && environment.Platform != PluginPlatform::Android && environment.Platform != PluginPlatform::macOS) ||
            !IsValidPlatformMask(descriptor.Platforms))
            return Failure(PluginValidationError::InvalidPlatform,
                "Plugin platform selection or mask is invalid: " + descriptor.Id);
        if (!HasPluginPlatform(descriptor.Platforms, environment.Platform))
            return Failure(PluginValidationError::UnsupportedPlatform,
                "Plugin does not support the selected platform: " + descriptor.Id);
        if (!IsSingleConfiguration(environment.Configuration) ||
            !IsValidConfigurationMask(descriptor.Configurations))
            return Failure(PluginValidationError::InvalidConfiguration,
                "Plugin configuration selection or mask is invalid: " + descriptor.Id);
        if (!HasPluginConfiguration(
                descriptor.Configurations,
                environment.Configuration))
            return Failure(PluginValidationError::UnsupportedConfiguration,
                "Plugin does not support the selected configuration: " + descriptor.Id);

        switch (descriptor.LoadPhase)
        {
        case PluginLoadPhase::PreDevice:
        case PluginLoadPhase::PostDevice:
        case PluginLoadPhase::PostEngine:
        case PluginLoadPhase::OnDemand:
            break;
        default:
            return Failure(PluginValidationError::InvalidLoadPhase,
                "Plugin load phase is invalid: " + descriptor.Id);
        }
        switch (descriptor.PackagePolicy)
        {
        case PluginPackagePolicy::Always:
        case PluginPackagePolicy::ProjectSelectable:
        case PluginPackagePolicy::Never:
            break;
        default:
            return Failure(PluginValidationError::InvalidPackagePolicy,
                "Plugin package policy is invalid: " + descriptor.Id);
        }
        if (environment.BuildDistribution == PluginBuildDistribution::Shipping &&
            descriptor.PackagePolicy == PluginPackagePolicy::Never)
            return Failure(PluginValidationError::ShippingPolicyViolation,
                "PackagePolicy Never plugin cannot be registered in Shipping: " +
                    descriptor.Id);
        if (descriptor.Distribution != PluginDistribution::Runtime &&
            descriptor.PackagePolicy != PluginPackagePolicy::Never)
            return Failure(PluginValidationError::ShippingPolicyViolation,
                "Non-Runtime plugin must use PackagePolicy Never: " + descriptor.Id);
        const bool bSupportsGame =
            HasPluginHost(descriptor.SupportedHosts, PluginHost::Game);
        if (bSupportsGame &&
            (descriptor.Distribution != PluginDistribution::Runtime ||
                descriptor.PackagePolicy == PluginPackagePolicy::Never))
            return Failure(PluginValidationError::ShippingPolicyViolation,
                "Game plugin must be Runtime and packageable: " + descriptor.Id);
        if (descriptor.PackagePolicy == PluginPackagePolicy::Always &&
            !bSupportsGame)
            return Failure(PluginValidationError::ShippingPolicyViolation,
                "PackagePolicy Always requires the Game host: " + descriptor.Id);

        if (const PluginValidationResult result =
                ValidateRoots(descriptor, descriptor.ShaderRoots, "shader");
            !result)
            return result;
        if (const PluginValidationResult result =
                ValidateRoots(descriptor, descriptor.ContentRoots, "content");
            !result)
            return result;

        std::unordered_set<PluginId> dependencyIds;
        for (const PluginDependency& dependency : descriptor.Dependencies)
        {
            if (const PluginValidationResult result =
                    ValidateDependency(descriptor, dependency, dependencyIds);
                !result)
                return result;
        }
        for (const PluginDependency& dependency : descriptor.OptionalDependencies)
        {
            if (const PluginValidationResult result =
                    ValidateDependency(descriptor, dependency, dependencyIds);
                !result)
                return result;
        }
        return {};
    }
}
