#include "Hepch.h"

#include "Runtime/UnitTest/EffekseerRegressionManifestTests.h"

#include "Runtime/Graphics/VFX/EffekseerRegressionManifest.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace HEngine::Tests
{
    namespace
    {
        void Require(bool condition, std::string_view message)
        {
            if (!condition)
                throw std::runtime_error(std::string(message));
        }

        class ScopedTempDirectory final
        {
        public:
            ScopedTempDirectory()
            {
                static std::atomic_uint64_t sSuffix = 0;
                const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
                mPath = std::filesystem::temp_directory_path() /
                    ("HEngine_EffekseerRegressionManifest_" + std::to_string(timestamp) + "_" +
                        std::to_string(sSuffix.fetch_add(1, std::memory_order_relaxed)));
                std::filesystem::create_directories(mPath);
            }

            ~ScopedTempDirectory()
            {
                std::error_code error;
                std::filesystem::remove_all(mPath, error);
            }

            [[nodiscard]] const std::filesystem::path& GetPath() const noexcept
            {
                return mPath;
            }

        private:
            std::filesystem::path mPath;
        };

        void WriteText(const std::filesystem::path& path, std::string_view contents)
        {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            Require(file.is_open(), "Could not create an Effekseer manifest test fixture.");
            file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            file.close();
            Require(!file.fail(), "Could not write an Effekseer manifest test fixture.");
        }

        void ExpectFailure(
            const std::filesystem::path& projectRoot,
            std::string_view expectedMessage)
        {
            bool bFailed = false;
            try
            {
                (void)LoadEffekseerRegressionManifest(projectRoot);
            }
            catch (const std::exception& exception)
            {
                bFailed = true;
                if (std::string_view(exception.what()).find(expectedMessage) == std::string_view::npos)
                {
                    throw std::runtime_error(
                        "Effekseer manifest diagnostic mismatch. expected='" +
                        std::string(expectedMessage) + "', actual='" + exception.what() + "'.");
                }
            }
            Require(bFailed, "An invalid Effekseer regression manifest was accepted.");
        }
    }

    void EffekseerRegressionManifestContract()
    {
        ScopedTempDirectory temporaryDirectory;
        const std::filesystem::path projectRoot = temporaryDirectory.GetPath() / "Project";
        const std::filesystem::path manifestPath =
            projectRoot / "Config" / "EffekseerRegression.json";

        WriteText(projectRoot / "Assets" / "Effects" / "First.efkpkg", "first");
        WriteText(projectRoot / "Assets" / "Effects" / "Second.EFKPKG", "second");
        ExpectFailure(projectRoot, "file does not exist or is not a regular file");

        WriteText(
            manifestPath,
            R"({"SchemaVersion":1,"Packages":["Assets\\Effects\\First.efkpkg","Assets/Effects/Second.EFKPKG"]})");

        const EffekseerRegressionManifest valid =
            LoadEffekseerRegressionManifest(projectRoot);
        Require(valid.Packages.size() == 2, "The valid Effekseer manifest lost packages.");
        Require(
            valid.Packages[0] == "Assets/Effects/First.efkpkg" &&
                valid.Packages[1] == "Assets/Effects/Second.EFKPKG",
            "Effekseer manifest package paths were not normalized.");

        WriteText(
            manifestPath,
            R"({"SchemaVersion":2,"Packages":[]})");
        ExpectFailure(projectRoot, "SchemaVersion must be 1");

        WriteText(
            manifestPath,
            R"({"SchemaVersion":1,"Packages":"Assets/Effects/First.efkpkg"})");
        ExpectFailure(projectRoot, "Packages must be an array");

        std::string oversized = R"({"SchemaVersion":1,"Packages":[)";
        for (std::size_t index = 0;
             index <= EffekseerRegressionManifestLimits::MaxPackages;
             ++index)
        {
            if (index > 0)
                oversized += ',';
            oversized += "\"Assets/Effects/Package" + std::to_string(index) + ".efkpkg\"";
        }
        oversized += "]}";
        WriteText(manifestPath, oversized);
        ExpectFailure(projectRoot, "maximum of 64");

        WriteText(
            manifestPath,
            R"({"SchemaVersion":1,"Packages":["../Outside.efkpkg"]})");
        ExpectFailure(projectRoot, "parent traversal");

        WriteText(projectRoot / "Assets" / "Effects" / "Wrong.txt", "wrong");
        WriteText(
            manifestPath,
            R"({"SchemaVersion":1,"Packages":["Assets/Effects/Wrong.txt"]})");
        ExpectFailure(projectRoot, "project-relative .efkpkg");

        WriteText(
            manifestPath,
            R"({"SchemaVersion":1,"Packages":["Assets/Effects/Missing.efkpkg"]})");
        ExpectFailure(projectRoot, "package does not exist");

        WriteText(
            manifestPath,
            R"({"SchemaVersion":1,"Packages":["Assets/Effects/First.efkpkg","assets/effects/FIRST.EFKPKG"]})");
        ExpectFailure(projectRoot, "duplicates another package path case-insensitively");

        WriteText(
            manifestPath,
            R"({"SchemaVersion":1,"Packages":[17]})");
        ExpectFailure(projectRoot, "Packages[0] must be a string");
    }
}
