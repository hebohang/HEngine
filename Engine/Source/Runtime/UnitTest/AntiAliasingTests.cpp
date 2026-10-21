#include "Hepch.h"

#include "Runtime/UnitTest/AntiAliasingTests.h"

#include "Runtime/Graphics/Renderer/AntiAliasing.h"

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace HEngine::Tests
{
    namespace
    {
        void Require(bool condition, std::string_view message)
        {
            if (!condition)
                throw std::runtime_error(std::string(message));
        }

        template<typename Exception, typename Callable>
        void RequireThrows(Callable&& callable, std::string_view message)
        {
            bool bCaughtExpectedException = false;
            try
            {
                std::forward<Callable>(callable)();
            }
            catch (const Exception&)
            {
                bCaughtExpectedException = true;
            }
            catch (...)
            {
            }
            Require(bCaughtExpectedException, message);
        }

        template<typename Mutator>
        void RequireInvalidSettings(
            Mutator&& mutator,
            std::string_view message)
        {
            AntiAliasingSettings settings;
            std::forward<Mutator>(mutator)(settings);
            Require(!IsValidAntiAliasingSettings(settings), message);
            RequireThrows<std::invalid_argument>(
                [&settings]() { ValidateAntiAliasingSettings(settings); },
                message);
        }

        [[nodiscard]] bool NearlyEqual(
            float left,
            float right,
            float epsilon = 1e-6f) noexcept
        {
            return std::abs(left - right) <= epsilon;
        }

        void TestModeStrings()
        {
            constexpr std::array modes = {
                std::pair{ AntiAliasingMode::None, std::string_view("None") },
                std::pair{ AntiAliasingMode::Fxaa, std::string_view("FXAA") },
                std::pair{ AntiAliasingMode::Smaa, std::string_view("SMAA") },
                std::pair{ AntiAliasingMode::Taa, std::string_view("TAA") },
                std::pair{ AntiAliasingMode::Msaa, std::string_view("MSAA") },
            };
            for (const auto& [mode, name] : modes)
            {
                Require(std::string_view(ToString(mode)) == name,
                    "An anti-aliasing mode produced an unstable display name.");
                Require(ParseAntiAliasingMode(name) == mode,
                    "An anti-aliasing mode did not parse from its canonical name.");
            }

            Require(std::string_view(ToString(
                    static_cast<AntiAliasingMode>(255))) == "Unknown",
                "An unknown anti-aliasing enum did not use the safe display name.");
            RequireThrows<std::invalid_argument>(
                []() { (void)ParseAntiAliasingMode("fxaa"); },
                "A non-canonical anti-aliasing mode name was accepted.");
            RequireThrows<std::invalid_argument>(
                []() { (void)ParseAntiAliasingMode("Unknown"); },
                "An unknown anti-aliasing mode name was accepted.");
        }

        void TestJsonRoundTrip()
        {
            AntiAliasingSettings settings;
            settings.Mode = AntiAliasingMode::Smaa;
            settings.MsaaSamples = 8;
            settings.FxaaEdgeThreshold = 0.25f;
            settings.FxaaEdgeThresholdMin = 0.015625f;
            settings.FxaaSubpixelQuality = 0.5f;
            settings.SmaaEdgeThreshold = 0.125f;
            settings.SmaaMaxSearchSteps = 32;
            settings.SmaaMaxDiagonalSearchSteps = 16;
            settings.SmaaCornerRounding = 0.5f;
            settings.TaaHistoryWeight = 0.875f;
            settings.TaaSharpness = 0.375f;
            settings.bTaaOptimizedHistoryGather = true;
            settings.bTaaSharedNeighborhood = true;
            settings.CaptureSupersampleScale = 4;

            const Json serialized = SerializeAntiAliasingSettings(settings);
            Require(serialized.is_object() && serialized.size() == 13 &&
                    !serialized.contains("CaptureSupersampleScale"),
                "AA JSON serialization omitted or invented a persisted field.");
            AntiAliasingSettings persistedSettings = settings;
            persistedSettings.CaptureSupersampleScale = 1;
            Require(DeserializeAntiAliasingSettings(serialized) ==
                    persistedSettings,
                "AA JSON serialization did not round-trip every persisted field.");

            Json legacySerialized = serialized;
            legacySerialized["CaptureSupersampleScale"] = 4;
            Require(DeserializeAntiAliasingSettings(legacySerialized)
                        .CaptureSupersampleScale == 4,
                "Legacy AA JSON no longer imports capture supersampling.");

            AntiAliasingSettings customDefaults;
            customDefaults.Mode = AntiAliasingMode::Fxaa;
            customDefaults.FxaaSubpixelQuality = 0.25f;
            customDefaults.CaptureSupersampleScale = 2;
            const AntiAliasingSettings partial =
                DeserializeAntiAliasingSettings(
                    Json{ { "FxaaEdgeThreshold", 0.5 } },
                    customDefaults);
            Require(partial.Mode == customDefaults.Mode &&
                    partial.FxaaSubpixelQuality ==
                        customDefaults.FxaaSubpixelQuality &&
                    partial.CaptureSupersampleScale ==
                        customDefaults.CaptureSupersampleScale &&
                    NearlyEqual(partial.FxaaEdgeThreshold, 0.5f),
                "Partial AA JSON did not preserve caller-provided defaults.");
        }

        void TestProjectTaaDefaultsJson()
        {
            const Json projectConfig = {
                { "Renderer", {
                    { "AntiAliasing", {
                        { "Mode", "TAA" },
                        { "MsaaSamples", 4 },
                        { "FxaaEdgeThreshold", 0.125 },
                        { "FxaaEdgeThresholdMin", 0.0312 },
                        { "FxaaSubpixelQuality", 0.75 },
                        { "SmaaEdgeThreshold", 0.06 },
                        { "SmaaMaxSearchSteps", 24 },
                        { "SmaaMaxDiagonalSearchSteps", 8 },
                        { "SmaaCornerRounding", 0.25 },
                        { "TaaHistoryWeight", 0.92 },
                        { "TaaSharpness", 0.15 },
                        { "CaptureSupersampleScale", 1 },
                    } },
                } },
            };
            const AntiAliasingSettings settings =
                DeserializeAntiAliasingSettings(
                    projectConfig.at("Renderer").at("AntiAliasing"));
            Require(settings.Mode == AntiAliasingMode::Taa &&
                    settings.MsaaSamples == 4 &&
                    NearlyEqual(settings.FxaaEdgeThreshold, 0.125f) &&
                    NearlyEqual(settings.FxaaEdgeThresholdMin, 0.0312f) &&
                    NearlyEqual(settings.FxaaSubpixelQuality, 0.75f) &&
                    NearlyEqual(settings.SmaaEdgeThreshold, 0.06f) &&
                    settings.SmaaMaxSearchSteps == 24 &&
                    settings.SmaaMaxDiagonalSearchSteps == 8 &&
                    NearlyEqual(settings.SmaaCornerRounding, 0.25f) &&
                    NearlyEqual(settings.TaaHistoryWeight, 0.92f) &&
                    NearlyEqual(settings.TaaSharpness, 0.15f) &&
                    settings.CaptureSupersampleScale == 1,
                "The in-memory project default did not select the complete TAA preset.");
        }

        void TestInvalidInputs()
        {
            RequireThrows<std::invalid_argument>(
                []()
                {
                    (void)DeserializeAntiAliasingSettings(
                        Json{ { "UnknownField", true } });
                },
                "An unknown AA JSON field was accepted.");
            RequireThrows<std::invalid_argument>(
                []()
                {
                    (void)DeserializeAntiAliasingSettings(Json::array());
                },
                "A non-object AA JSON value was accepted.");
            RequireThrows<std::invalid_argument>(
                []()
                {
                    (void)DeserializeAntiAliasingSettings(Json{
                        { "TaaHistoryWeight",
                            std::numeric_limits<double>::quiet_NaN() },
                    });
                },
                "A NaN AA JSON value was accepted.");
            RequireThrows<std::invalid_argument>(
                []()
                {
                    (void)DeserializeAntiAliasingSettings(
                        Json{ { "MsaaSamples", 16 } });
                },
                "An out-of-range MSAA JSON value was accepted.");
            RequireThrows<std::invalid_argument>(
                []()
                {
                    (void)DeserializeAntiAliasingSettings(
                        Json{ { "CaptureSupersampleScale", 3 } });
                },
                "An out-of-range capture scale JSON value was accepted.");

            RequireInvalidSettings(
                [](auto& value)
                {
                    value.Mode = static_cast<AntiAliasingMode>(255);
                },
                "An unknown anti-aliasing enum was accepted.");
            RequireInvalidSettings(
                [](auto& value) { value.MsaaSamples = 1; },
                "An unsupported MSAA sample count was accepted.");
            RequireInvalidSettings(
                [](auto& value) { value.FxaaEdgeThreshold = 0.0f; },
                "A zero FXAA edge threshold was accepted.");
            RequireInvalidSettings(
                [](auto& value) { value.FxaaEdgeThresholdMin = -0.01f; },
                "A negative FXAA minimum edge threshold was accepted.");
            RequireInvalidSettings(
                [](auto& value) { value.FxaaSubpixelQuality = 1.01f; },
                "An out-of-range FXAA subpixel quality was accepted.");
            RequireInvalidSettings(
                [](auto& value) { value.SmaaEdgeThreshold = 0.0f; },
                "A zero SMAA edge threshold was accepted.");
            RequireInvalidSettings(
                [](auto& value) { value.SmaaMaxSearchSteps = 0; },
                "A zero SMAA search limit was accepted.");
            RequireInvalidSettings(
                [](auto& value) { value.SmaaMaxSearchSteps = 33; },
                "An excessive SMAA search limit was accepted.");
            RequireInvalidSettings(
                [](auto& value)
                {
                    value.SmaaMaxDiagonalSearchSteps = 17;
                },
                "An excessive SMAA diagonal search limit was accepted.");
            RequireInvalidSettings(
                [](auto& value) { value.SmaaCornerRounding = 1.01f; },
                "An out-of-range SMAA corner rounding value was accepted.");
            RequireInvalidSettings(
                [](auto& value) { value.TaaHistoryWeight = 1.0f; },
                "An out-of-range TAA history weight was accepted.");
            RequireInvalidSettings(
                [](auto& value)
                {
                    value.TaaSharpness =
                        std::numeric_limits<float>::quiet_NaN();
                },
                "A NaN TAA sharpness was accepted.");
            RequireInvalidSettings(
                [](auto& value) { value.CaptureSupersampleScale = 3; },
                "An unsupported capture supersample scale was accepted.");
        }

        void TestMsaaSamples()
        {
            for (uint32_t valid : { 2u, 4u, 8u })
            {
                Require(IsValidMsaaSampleCount(valid),
                    "A supported MSAA sample count was rejected.");
                AntiAliasingSettings settings;
                settings.Mode = AntiAliasingMode::Msaa;
                settings.MsaaSamples = valid;
                Require(GetAntiAliasingSampleCount(settings) == valid,
                    "MSAA did not expose its requested raster sample count.");
            }
            for (uint32_t invalid : { 0u, 1u, 3u, 16u })
            {
                Require(!IsValidMsaaSampleCount(invalid),
                    "An unsupported MSAA sample count was reported as valid.");
            }

            AntiAliasingSettings taa;
            taa.Mode = AntiAliasingMode::Taa;
            Require(GetAntiAliasingSampleCount(taa) == 1,
                "A post-process anti-aliasing mode changed raster sample count.");

            AntiAliasingSettings invalidMsaa;
            invalidMsaa.Mode = AntiAliasingMode::Msaa;
            invalidMsaa.MsaaSamples = 16;
            RequireThrows<std::invalid_argument>(
                [&invalidMsaa]()
                {
                    (void)GetAntiAliasingSampleCount(invalidMsaa);
                },
                "An unsupported MSAA sample count reached raster setup.");
        }

        void TestCapabilityFallback()
        {
            AntiAliasingCapabilities capabilities;
            capabilities.Fxaa = { true, "Synthetic FXAA support" };
            capabilities.Smaa = { false, "Synthetic SMAA shader failure" };
            capabilities.Taa = { true, "Synthetic TAA support" };
            capabilities.Msaa2x = { true, "Synthetic 2x support" };
            capabilities.Msaa4x = { false, "Synthetic 4x format failure" };
            capabilities.Msaa8x = { true, "Synthetic 8x support" };

            AntiAliasingSettings requested;
            requested.Mode = AntiAliasingMode::Fxaa;
            AntiAliasingSelection selection = ResolveAntiAliasingSelection(
                requested,
                capabilities);
            Require(selection.Effective.Mode == AntiAliasingMode::Fxaa &&
                    selection.bRequestedModeAvailable &&
                    !selection.bFallback && !selection.bBypassed,
                "An available FXAA mode did not remain selected.");

            requested.Mode = AntiAliasingMode::Smaa;
            selection = ResolveAntiAliasingSelection(requested, capabilities);
            Require(selection.Requested.Mode == AntiAliasingMode::Smaa &&
                    selection.Effective.Mode == AntiAliasingMode::None &&
                    !selection.bRequestedModeAvailable &&
                    selection.bFallback && !selection.bBypassed &&
                    selection.Reason == "Synthetic SMAA shader failure",
                "An unavailable SMAA mode did not fail closed with its reason.");

            requested.Mode = AntiAliasingMode::Taa;
            selection = ResolveAntiAliasingSelection(requested, capabilities);
            Require(selection.Effective.Mode == AntiAliasingMode::Taa &&
                    !selection.bFallback,
                "An available TAA mode incorrectly fell back.");

            requested.Mode = AntiAliasingMode::Msaa;
            requested.MsaaSamples = 2;
            selection = ResolveAntiAliasingSelection(requested, capabilities);
            Require(selection.Effective.Mode == AntiAliasingMode::Msaa &&
                    GetAntiAliasingSampleCount(selection.Effective) == 2 &&
                    !selection.bFallback,
                "Available 2x MSAA did not remain selected.");

            requested.MsaaSamples = 4;
            selection = ResolveAntiAliasingSelection(requested, capabilities);
            Require(selection.Effective.Mode == AntiAliasingMode::None &&
                    selection.bFallback &&
                    !selection.bRequestedModeAvailable &&
                    selection.Reason == "Synthetic 4x format failure",
                "Unavailable 4x MSAA did not fall back independently.");

            requested.MsaaSamples = 8;
            selection = ResolveAntiAliasingSelection(requested, capabilities);
            Require(selection.Effective.Mode == AntiAliasingMode::Msaa &&
                    GetAntiAliasingSampleCount(selection.Effective) == 8 &&
                    !selection.bFallback,
                "Available 8x MSAA did not remain selected.");

            requested.Mode = AntiAliasingMode::None;
            selection = ResolveAntiAliasingSelection(requested, {});
            Require(selection.Effective.Mode == AntiAliasingMode::None &&
                    selection.bRequestedModeAvailable &&
                    !selection.bFallback,
                "Explicitly disabled anti-aliasing was treated as unavailable.");
        }

        void TestOfflineBypass()
        {
            AntiAliasingSettings requested;
            requested.Mode = AntiAliasingMode::Taa;
            requested.TaaHistoryWeight = 0.875f;
            requested.CaptureSupersampleScale = 4;

            AntiAliasingCapabilities unavailableCapabilities;
            const AntiAliasingSelection selection =
                ResolveAntiAliasingSelection(
                    requested,
                    unavailableCapabilities,
                    AntiAliasingRenderPath::OfflinePathTracing);
            Require(selection.Requested == requested &&
                    selection.Effective.Mode == AntiAliasingMode::None &&
                    selection.Effective.CaptureSupersampleScale == 1 &&
                    selection.Effective.TaaHistoryWeight ==
                        requested.TaaHistoryWeight &&
                    selection.bBypassed && !selection.bFallback &&
                    selection.Reason.find("stochastic subpixel sampling") !=
                        std::string::npos,
                "Offline path tracing did not bypass realtime AA and capture supersampling.");
        }
    }

    void AntiAliasingContract()
    {
        TestModeStrings();
        TestJsonRoundTrip();
        TestProjectTaaDefaultsJson();
        TestInvalidInputs();
        TestMsaaSamples();
        TestCapabilityFallback();
        TestOfflineBypass();
    }
}
