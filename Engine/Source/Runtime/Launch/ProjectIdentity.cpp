#include "Hepch.h"

#include "ProjectIdentity.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>

namespace HEngine
{
    namespace
    {
        std::string ReadString(const nlohmann::json& value, const char* field)
        {
            if (!value.contains(field) || !value.at(field).is_string())
                throw std::runtime_error(std::string("Project identity requires a string: ") + field);
            return value.at(field).get<std::string>();
        }

        bool IsSafeName(std::string_view value, std::size_t maximum, bool bLowercase)
        {
            if (value.empty() || value.size() > maximum || value.back() == '.') return false;
            const auto isAlphaNumeric = [bLowercase](char character) {
                return (character >= 'a' && character <= 'z') ||
                    (!bLowercase && character >= 'A' && character <= 'Z') ||
                    (character >= '0' && character <= '9');
            };
            if (!isAlphaNumeric(value.front())) return false;
            for (const char character : value)
                if (!isAlphaNumeric(character) && character != '_' && character != '-' && character != '.') return false;
            std::string lower(value);
            for (char& character : lower)
                if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
            const auto stem = std::string_view(lower).substr(0, lower.find('.'));
            if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") return false;
            return !(stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) &&
                stem[3] >= '1' && stem[3] <= '9');
        }
    }

    ProjectIdentity ReadProjectIdentity(const nlohmann::json& descriptor, std::string_view engineVersion)
    {
        if (!descriptor.is_object() || !descriptor.contains("SchemaVersion") ||
            !descriptor.at("SchemaVersion").is_number_integer() || descriptor.at("SchemaVersion") != 1)
            throw std::runtime_error("Project identity SchemaVersion must be 1.");
        if (ReadString(descriptor, "EngineVersion") != engineVersion)
            throw std::runtime_error("Project identity EngineVersion does not match the player.");
        ProjectIdentity identity{
            .ProjectId = ReadString(descriptor, "ProjectId"),
            .DisplayName = ReadString(descriptor, "DisplayName"),
            .ExecutableName = ReadString(descriptor, "ExecutableName"),
            .GameConfig = ReadString(descriptor, "GameConfig"),
        };
        if (!IsSafeName(identity.ProjectId, 128, true))
            throw std::runtime_error("Project identity ProjectId is not a safe storage name.");
        std::string executableLower = identity.ExecutableName;
        for (char& character : executableLower)
            if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
        if (!IsSafeName(identity.ExecutableName, 64, false) || executableLower.ends_with(".exe"))
            throw std::runtime_error("Project identity ExecutableName is not a safe executable basename.");
        if (identity.DisplayName.empty() || identity.DisplayName.size() > 512 ||
            std::any_of(identity.DisplayName.begin(), identity.DisplayName.end(), [](unsigned char character) {
                return character < 32 || character == 127;
            }))
            throw std::runtime_error("Project identity DisplayName is empty, too long or contains control characters.");
        std::string normalized;
        if (!Utils::TryNormalizeProjectRelativePath(identity.GameConfig, normalized) ||
            !normalized.ends_with(".json"))
            throw std::runtime_error("Project identity GameConfig must be a safe project-relative JSON path.");
        identity.GameConfig = normalized;
        if (descriptor.contains("LegacySavedDirectory"))
        {
            identity.LegacySavedDirectory = ReadString(descriptor, "LegacySavedDirectory");
            if (!identity.LegacySavedDirectory.empty() && !IsSafeName(identity.LegacySavedDirectory, 128, false))
                throw std::runtime_error("Project identity LegacySavedDirectory is not a safe storage name.");
        }
        return identity;
    }

    ProjectIdentity ReadPlayerIdentity(const nlohmann::json& metadata, std::string_view engineVersion,
        std::string_view runtimeFingerprint)
    {
        if (runtimeFingerprint.size() != 64 || ReadString(metadata, "RuntimeFingerprint") != runtimeFingerprint)
            throw std::runtime_error("Game package requires a different player runtime fingerprint.");
        return ReadProjectIdentity(metadata, engineVersion);
    }
}
