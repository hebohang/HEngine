#include "Hepch.h"

#include "Runtime/UnitTest/RenderCapabilityPolicyTests.h"

#include "Runtime/Graphics/RHI/RenderBackend.h"

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

        [[nodiscard]] RenderCapabilities MakeBaselineCapabilities()
        {
            RenderCapabilities capabilities;
            capabilities.BaselineRaster = {
                .bSupported = true,
                .bEnabled = true,
                .Reason = "Synthetic FL11_0/SM6.0 baseline",
            };
            return capabilities;
        }
    }

    void RenderCapabilityPolicyContract()
    {
        RenderCapabilities baseline = MakeBaselineCapabilities();
        ResolveRendererCapabilityPolicy(baseline);
        Require(baseline.Profile == RendererProfile::BaselineRaster,
            "An FL11 baseline-only device did not select Baseline Raster.");
        Require(!baseline.bHardwareRayTracingRendererReady &&
                !baseline.HardwareRayTracingRendererPath.bSupported &&
                !baseline.HardwareRayTracingRendererPath.bEnabled,
            "A baseline-only device incorrectly reported hardware-ray-tracing readiness or enablement.");

        RenderCapabilities modern = MakeBaselineCapabilities();
        modern.ShaderModel6_6 = {
            .bSupported = true,
            .bEnabled = true,
            .Reason = "Synthetic SM6.6",
        };
        modern.EnhancedBarriers = {
            .bSupported = true,
            .bEnabled = true,
            .Reason = "Synthetic Enhanced Barriers",
        };
        ResolveRendererCapabilityPolicy(modern);
        Require(modern.Profile == RendererProfile::ModernRaster,
            "A modern non-ray-tracing device did not select Modern Raster.");
        Require(!modern.bHardwareRayTracingRendererReady &&
                !modern.HardwareRayTracingRendererPath.bEnabled,
            "A modern non-ray-tracing device incorrectly reported an active hardware-ray-tracing path.");

        RenderCapabilities hardwareRayTracingReady = modern;
        hardwareRayTracingReady.RayTracingAccelStruct = {
            .bSupported = true,
            .bEnabled = true,
            .Reason = "Synthetic DXR acceleration structures",
        };
        hardwareRayTracingReady.RayTracingPipeline = {
            .bSupported = true,
            .bEnabled = true,
            .Reason = "Synthetic DXR pipeline",
        };
        ResolveRendererCapabilityPolicy(hardwareRayTracingReady);
        Require(hardwareRayTracingReady.Profile == RendererProfile::ModernRaster,
            "Ray-tracing-ready hardware incorrectly selected an unregistered renderer profile.");
        Require(hardwareRayTracingReady.bHardwareRayTracingRendererReady &&
                hardwareRayTracingReady.HardwareRayTracingRendererPath.bSupported &&
                !hardwareRayTracingReady.HardwareRayTracingRendererPath.bEnabled,
            "Ray-tracing-ready hardware did not preserve readiness separately from path enablement.");
        Require(hardwareRayTracingReady.HardwareRayTracingRendererPath.Reason.find(
                    "no renderer technique") != std::string::npos,
            "Hardware ray-tracing readiness did not explain why raster rendering remains selected.");

        RenderCapabilities registeredHardwareRayTracing = hardwareRayTracingReady;
        ResolveRendererCapabilityPolicy(registeredHardwareRayTracing, true);
        Require(registeredHardwareRayTracing.Profile ==
                    RendererProfile::HardwareRayTracing &&
                registeredHardwareRayTracing.HardwareRayTracingRendererPath.bEnabled,
            "The explicit hardware-ray-tracing registration gate did not select the ready path.");
    }
}
