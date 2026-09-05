#include "Runtime/UnitTest/EffekseerRuntimeLifecycleTests.h"

#include "Runtime/Graphics/VFX/EffekseerRuntime.h"

#include <stdexcept>

namespace HEngine::Tests
{
    namespace
    {
        void Require(bool condition, const char* message)
        {
            if (!condition)
                throw std::runtime_error(message);
        }
    }

    void EffekseerRuntimeLifecycleContract()
    {
        EffekseerRuntime::Shutdown();
        Require(!EffekseerRuntime::IsInitialized(),
            "A fresh Effekseer runtime reported initialized");
        Require(EffekseerRuntime::GetRenderBackendStatus() == VFXRenderBackendStatus::Unconfigured,
            "A fresh Effekseer runtime exposed a configured backend");
        Require(!EffekseerRuntime::ValidateAllEffectPackages(),
            "Effekseer regression validation accessed project state before runtime initialization");

        bool bRejectedMissingDevice = false;
        try
        {
            EffekseerRuntime::Initialize({
                .ColorFormat = nvrhi::Format::RGBA8_UNORM,
                .FramesInFlight = 3,
            });
        }
        catch (const std::invalid_argument&)
        {
            bRejectedMissingDevice = true;
        }
        Require(bRejectedMissingDevice,
            "Effekseer runtime accepted initialization without an NVRHI device");
        Require(!EffekseerRuntime::IsInitialized(),
            "Rejected Effekseer initialization changed lifecycle state");

        const VFXStats stats = EffekseerRuntime::GetStats();
        Require(stats.GpuSimulationMs.Status == VFXMetricStatus::Unavailable &&
                stats.GpuRenderMs.Status == VFXMetricStatus::Unavailable,
            "Effekseer runtime reported GPU timing without a timestamp provider");

        // Compatibility entry points must not resurrect a legacy backend.
        EffekseerRuntime::Init();
        EffekseerRuntime::Render(0.1f, 1000.0f, {});
        Require(!EffekseerRuntime::IsInitialized() &&
                EffekseerRuntime::GetRenderBackendStatus() == VFXRenderBackendStatus::Unconfigured,
            "Legacy Effekseer entry points selected a graphics backend");

        EffekseerRuntime::Shutdown();
    }
}
