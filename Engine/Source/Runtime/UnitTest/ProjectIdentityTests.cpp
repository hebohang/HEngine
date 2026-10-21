#include "Hepch.h"

#include "ProjectIdentityTests.h"
#include "Runtime/Launch/ProjectIdentity.h"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

namespace HEngine::Tests
{
    void ProjectIdentityContract()
    {
        const std::string fingerprint(64, 'a');
        const nlohmann::json descriptor{
            {"SchemaVersion", 1}, {"EngineVersion", "1.1.0"},
            {"ProjectId", "independent-game"}, {"DisplayName", "Independent Game"},
            {"ExecutableName", "IndependentGame"}, {"GameConfig", "Config/Alternate.json"},
            {"LegacySavedDirectory", "OldGame"}, {"RuntimeFingerprint", fingerprint},
        };
        const auto identity = ReadPlayerIdentity(descriptor, "1.1.0", fingerprint);
        if (identity.ProjectId != "independent-game" || identity.GameConfig != "Config/Alternate.json" ||
            identity.LegacySavedDirectory != "OldGame" || identity.ExecutableName != "IndependentGame")
            throw std::runtime_error("Portable player did not select the package's identity.");
        auto second = descriptor;
        second["ProjectId"] = "another-game";
        if (ReadPlayerIdentity(second, "1.1.0", fingerprint).ProjectId == identity.ProjectId)
            throw std::runtime_error("Two games share a template's compiled project identity.");

        const auto reject = [&](const char* field, nlohmann::json invalid) {
            auto candidate = descriptor;
            candidate[field] = std::move(invalid);
            try { (void)ReadPlayerIdentity(candidate, "1.1.0", fingerprint); }
            catch (const std::exception&) { return; }
            throw std::runtime_error(std::string("Player accepted invalid identity field: ") + field);
        };
        reject("SchemaVersion", true);
        reject("SchemaVersion", 2);
        reject("EngineVersion", "1.2.0");
        reject("RuntimeFingerprint", std::string(64, 'b'));
        reject("RuntimeFingerprint", nullptr);
        for (const auto* name : {"../outside", "C:/absolute", "nul", "con.json", "com1", "trailing.", "UPPER"})
            reject("ProjectId", name);
        reject("ExecutableName", "Bad.EXE");
        reject("ExecutableName", "LPT9.txt");
        reject("ExecutableName", "bad:stream");
        reject("DisplayName", "");
        reject("DisplayName", "line\nbreak");
        for (const auto* path : {"../Game.json", "C:/Game.json", "Config/Game.txt", "Config/Game.json:stream"})
            reject("GameConfig", path);
        reject("LegacySavedDirectory", "../other-game");
        reject("LegacySavedDirectory", "NUL");
        auto missing = descriptor;
        missing.erase("RuntimeFingerprint");
        try { (void)ReadPlayerIdentity(missing, "1.1.0", fingerprint); }
        catch (const std::exception&) { return; }
        throw std::runtime_error("Portable player accepted a package with no runtime contract.");
    }
}
