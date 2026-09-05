#include "Hepch.h"

#include "Runtime/UnitTest/ViewportRendererExtensionTests.h"

#include "Runtime/Graphics/Renderer/ViewportMode.h"

#include <algorithm>
#include <array>
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
            bool bCaughtExpected = false;
            try
            {
                std::forward<Callable>(callable)();
            }
            catch (const Exception&)
            {
                bCaughtExpected = true;
            }
            catch (...)
            {
            }
            Require(bCaughtExpected, message);
        }

        class FakeOfflineViewportRendererSession final :
            public IOfflineViewportRendererSession
        {
        public:
            FakeOfflineViewportRendererSession(
                std::string providerId,
                size_t& destructionCount)
                : mProviderId(std::move(providerId)),
                  mDestructionCount(destructionCount)
            {
            }

            ~FakeOfflineViewportRendererSession() override
            {
                ++mDestructionCount;
            }

            [[nodiscard]] std::string_view GetProviderId() const noexcept override
            {
                return mProviderId;
            }

            [[nodiscard]] const OfflineViewportRendererSettings&
                GetOfflineSettings() const noexcept override
            {
                return mSettings;
            }

            void ApplyOfflineSettings(
                const OfflineViewportRendererSettings& settings) override
            {
                mSettings = settings;
            }

            [[nodiscard]] OfflineViewportRendererProgress
                GetOfflineProgress() const override
            {
                return {};
            }

            void SetLastGpuMilliseconds(double) noexcept override {}

            [[nodiscard]] std::span<const OfflineViewportAov>
                GetAvailableAovs() const noexcept override
            {
                return mAovs;
            }

            [[nodiscard]] OfflineViewportNativeAov GetNativeAov(
                OfflineViewportAov) const override
            {
                return {};
            }

            void Pause() noexcept override {}
            void Resume() noexcept override {}
            void Restart() override {}
            void Cancel() noexcept override {}

            [[nodiscard]] TextureReadbackHandle RequestExport(
                OfflineViewportAov,
                std::filesystem::path,
                OfflineViewportOutputFormat) override
            {
                return {};
            }

            [[nodiscard]] bool IsExportValid(
                TextureReadbackHandle) const noexcept override
            {
                return false;
            }

            [[nodiscard]] TextureReadbackResult GetExportResult(
                TextureReadbackHandle) const override
            {
                return {};
            }

            void ReleaseExport(TextureReadbackHandle) override {}

            [[nodiscard]] bool IsSupported(
                const RenderCapabilities&) const override
            {
                return true;
            }

            [[nodiscard]] GlobalIlluminationPrepareResult PrepareFrame(
                const GlobalIlluminationFrameContext&,
                const GlobalIlluminationSettings&) override
            {
                return {};
            }

            [[nodiscard]] GlobalIlluminationOutputs AddPasses(
                RenderGraph&,
                const GlobalIlluminationInputs&,
                const GlobalIlluminationSettings&) override
            {
                return {};
            }

            void ResetHistory(GlobalIlluminationHistoryResetReason) override {}
            void UpdateImportedResources(RenderGraph&) override {}
            void NotifyFrameSubmitted(uint64_t) noexcept override {}
            void AbortFrame() noexcept override {}
            void Collect(uint64_t) override {}
            void Clear(uint64_t) override {}

            [[nodiscard]] bool RequiresContinuousUpdate() const noexcept override
            {
                return false;
            }

            [[nodiscard]] GlobalIlluminationStats GetStats() const override
            {
                return {};
            }

        private:
            std::string mProviderId;
            size_t& mDestructionCount;
            OfflineViewportRendererSettings mSettings;
            std::array<OfflineViewportAov, 0> mAovs;
        };

        [[nodiscard]] OfflineViewportRendererProviderDescriptor MakeProvider(
            std::string id,
            std::string displayName,
            int32_t sortOrder,
            bool bSupported,
            size_t* destructionCount = nullptr)
        {
            const std::string providerId = id;
            return {
                .Id = std::move(id),
                .DisplayName = std::move(displayName),
                .OwnerPluginId = "he.test.viewport-renderer",
                .SortOrder = sortOrder,
                .QuerySupport = [bSupported](
                    const RenderCapabilities&,
                    const std::filesystem::path&)
                {
                    return OfflineViewportRendererSupport{
                        .bSupported = bSupported,
                        .Reason = bSupported
                            ? "Test provider is available."
                            : "Test provider is intentionally unavailable.",
                    };
                },
                .Factory = [providerId, destructionCount](
                    const OfflineViewportRendererCreateContext&)
                    -> Scope<IOfflineViewportRendererSession>
                {
                    if (!destructionCount)
                        return {};
                    return CreateScope<FakeOfflineViewportRendererSession>(
                        providerId,
                        *destructionCount);
                },
            };
        }
    }

    void ViewportRendererExtensionContract()
    {
        constexpr std::string_view FirstId = "he.test.viewport.first";
        constexpr std::string_view SecondId = "he.test.viewport.second";
        constexpr std::string_view DrainingId = "he.test.viewport.draining";
        constexpr std::string_view UnregisterId = "he.test.viewport.unregister";
        OfflineViewportRendererProviderRegistry& registry =
            OfflineViewportRendererProviderRegistry::Get();
        Require(!registry.Contains(FirstId) && !registry.Contains(SecondId) &&
                !registry.Contains(DrainingId) &&
                !registry.Contains(UnregisterId),
            "Viewport provider test IDs leaked from a previous lifecycle.");

        OfflineViewportRendererProviderRegistration second =
            registry.Register(MakeProvider(
                std::string(SecondId), "Second", 20, false));
        OfflineViewportRendererProviderRegistration first =
            registry.Register(MakeProvider(
                std::string(FirstId), "First", 10, true));
        RequireThrows<std::logic_error>(
            [&registry, FirstId]
            {
                (void)registry.Register(MakeProvider(
                    std::string(FirstId), "Duplicate", 0, true));
            },
            "Viewport provider registry accepted a duplicate stable ID.");

        const std::vector<OfflineViewportRendererProviderDescriptor> providers =
            registry.Enumerate();
        const auto firstPosition = std::ranges::find_if(
            providers,
            [FirstId](const auto& provider) { return provider.Id == FirstId; });
        const auto secondPosition = std::ranges::find_if(
            providers,
            [SecondId](const auto& provider) { return provider.Id == SecondId; });
        Require(firstPosition != providers.end() &&
                secondPosition != providers.end() &&
                firstPosition < secondPosition,
            "Viewport providers were not enumerated in deterministic sort order.");
        Require(!firstPosition->Factory && !firstPosition->QuerySupport,
            "Provider enumeration leaked callable factory/support callbacks.");

        RenderCapabilities capabilities;
        ViewportModeController controller;
        const std::vector<ViewportMode> modes =
            controller.EnumerateModes(capabilities);
        Require(modes.size() >= 4 &&
                modes[0].Id == "raster.2d" &&
                modes[0].Dimension == SceneDimension::TwoD &&
                modes[1].Id == "raster.3d" &&
                modes[1].Dimension == SceneDimension::ThreeD,
            "Raster 2D/3D modes are not the stable leading viewport modes.");
        const auto unavailable = std::ranges::find_if(
            modes,
            [SecondId](const ViewportMode& mode) { return mode.Id == SecondId; });
        Require(unavailable != modes.end() && !unavailable->bAvailable &&
                unavailable->UnavailableReason ==
                    "Test provider is intentionally unavailable.",
            "An unavailable plugin mode did not retain its diagnostic.");

        RequireThrows<std::logic_error>(
            [&controller, &capabilities, FirstId]
            {
                (void)controller.SetMode(FirstId, capabilities, false);
            },
            "An offline viewport mode was accepted outside Editor Edit state.");
        RequireThrows<std::runtime_error>(
            [&controller, &capabilities, SecondId]
            {
                (void)controller.SetMode(SecondId, capabilities, true);
            },
            "An unavailable viewport provider was activated.");
        const ViewportModeTransition activated =
            controller.SetMode(FirstId, capabilities, true);
        Require(activated.bChanged && controller.IsOfflineRendererActive() &&
                activated.Current.Dimension == SceneDimension::ThreeD,
            "The plugin viewport mode did not map to 3D plus an offline renderer.");
        const ViewportModeTransition playTransition =
            controller.PrepareForPlay();
        Require(playTransition.bChanged &&
                playTransition.Current.Id == "raster.3d" &&
                !controller.IsOfflineRendererActive(),
            "Preparing for Play did not return to 3D Raster.");

        RequireThrows<std::runtime_error>(
            [&registry, FirstId]
            {
                (void)registry.Create(
                    FirstId,
                    OfflineViewportRendererCreateContext{});
            },
            "A null viewport session factory result was accepted.");
        Require(registry.GetActiveSessionCount(FirstId) == 0,
            "A failed viewport session factory leaked its active-session count.");

        first.Release();
        second.Release();
        Require(!registry.Contains(FirstId) && !registry.Contains(SecondId),
            "Viewport provider RAII handles did not fully unregister modes.");

        size_t drainingSessionDestructions = 0;
        OfflineViewportRendererProviderRegistration drainingRegistration =
            registry.Register(MakeProvider(
                std::string(DrainingId),
                "Draining",
                30,
                true,
                &drainingSessionDestructions));
        OfflineViewportRendererProviderRegistration movedRegistration =
            std::move(drainingRegistration);
        Require(!drainingRegistration && movedRegistration,
            "Moving a provider registration duplicated or lost ownership.");
        OfflineViewportRendererSessionLease drainingLease = registry.Create(
            DrainingId,
            OfflineViewportRendererCreateContext{});
        Require(drainingLease &&
                drainingLease->GetProviderId() == DrainingId &&
                registry.GetActiveSessionCount(DrainingId) == 1,
            "A live provider session lease was not tracked.");

        movedRegistration.Release();
        movedRegistration.Release();
        Require(!movedRegistration && !registry.Contains(DrainingId),
            "A draining provider remained discoverable after registration release.");
        const std::vector<OfflineViewportRendererProviderDescriptor>
            visibleWhileDraining = registry.Enumerate();
        const auto drainingProvider = std::ranges::find_if(
            visibleWhileDraining,
            [DrainingId](const auto& provider)
            {
                return provider.Id == DrainingId;
            });
        Require(drainingProvider == visibleWhileDraining.end(),
            "A draining provider remained visible in enumeration.");
        const OfflineViewportRendererSupport drainingSupport =
            registry.QuerySupport(DrainingId, capabilities);
        Require(!drainingSupport.bSupported &&
                drainingSupport.Reason.find("not registered") !=
                    std::string::npos,
            "A draining provider still answered support queries.");
        RequireThrows<std::out_of_range>(
            [&registry, DrainingId]
            {
                (void)registry.Create(
                    DrainingId,
                    OfflineViewportRendererCreateContext{});
            },
            "A draining provider accepted a new session.");
        Require(drainingLease &&
                registry.GetActiveSessionCount(DrainingId) == 1,
            "Draining invalidated an existing session lease.");

        OfflineViewportRendererSessionLease movedLease =
            std::move(drainingLease);
        OfflineViewportRendererSessionLease assignedLease;
        assignedLease = std::move(movedLease);
        Require(!drainingLease && !movedLease && assignedLease,
            "Moving a session lease duplicated or lost ownership.");
        assignedLease.Release();
        assignedLease.Release();
        Require(drainingSessionDestructions == 1 &&
                !registry.Contains(DrainingId),
            "The final draining session release did not destroy the session once.");
        OfflineViewportRendererProviderRegistration replacement =
            registry.Register(MakeProvider(
                std::string(DrainingId), "Replacement", 30, true));
        replacement.Release();

        size_t unregisteredSessionDestructions = 0;
        OfflineViewportRendererProviderRegistration unregistered =
            registry.Register(MakeProvider(
                std::string(UnregisterId),
                "Unregister",
                40,
                true,
                &unregisteredSessionDestructions));
        OfflineViewportRendererSessionLease unregisteredLease = registry.Create(
            UnregisterId,
            OfflineViewportRendererCreateContext{});
        registry.Unregister(UnregisterId);
        registry.Unregister(UnregisterId);
        Require(!unregistered && !registry.Contains(UnregisterId),
            "Direct unregister did not tombstone an active provider.");
        RequireThrows<std::out_of_range>(
            [&registry, UnregisterId]
            {
                (void)registry.Create(
                    UnregisterId,
                    OfflineViewportRendererCreateContext{});
            },
            "Direct unregister allowed a new session while draining.");
        unregistered.Release();
        unregisteredLease.Release();
        registry.Unregister(UnregisterId);
        Require(unregisteredSessionDestructions == 1,
            "Direct unregister did not retain and release the existing lease exactly once.");
    }
}
