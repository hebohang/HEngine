#include "Runtime/UnitTest/EffekseerRuntimeLifecycleTests.h"

#include "Runtime/Graphics/VFX/EffekseerRuntime.h"
#include "Runtime/Graphics/VFX/EffekseerRecordLeases.h"
#include "Runtime/Graphics/VFX/EffekseerCamera.h"
#include "Runtime/Graphics/VFX/EffekseerPackage.h"

#include <Effekseer/Geometry/Effekseer.GeometryUtility.h>
#include <glm/gtc/matrix_transform.hpp>

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
        // Exercise the SDK's actual UTF-16 boundary with ASCII, multilingual
        // paths and a surrogate pair independently of native wchar_t width.
        for (const auto path : { u"", u"textures/ring.png", u"贴图/星光.png", u"Éffects/crème.png", u"Textures/\U0001F331.png" })
        {
            const std::u16string expected(path);
            Require(EffekseerPackage::Utf8PathToUtf16(EffekseerPackage::Utf16PathToUtf8(expected)) == expected,
                "An Effekseer UTF-16 package dependency did not survive UTF-8 conversion");
        }
        Require(EffekseerPackage::Utf16PathToUtf8(u"textures/ring.png") == "textures/ring.png",
            "Effekseer package lookup corrupted ASCII path code units");
        // Production lease allocator controls both native adapters and dynamic
        // vertex rings. Main and preview recordings must never share a lease.
        EffekseerRecordLeases leases(2);
        leases.Begin(0, 0);
        const auto mainRecord = leases.Acquire();
        const auto previewRecord = leases.Acquire();
        Require(mainRecord.FrameSlot == previewRecord.FrameSlot && mainRecord.Record != previewRecord.Record,
            "Two views in one host submission shared mutable Effekseer storage");
        leases.Submitted(0, 10);
        leases.Submitted(0, 10);
        bool bBlocked = false;
        try { leases.Begin(0, 9); } catch (const std::logic_error&) { bBlocked = true; }
        Require(bBlocked, "Incomplete GPU submission allowed Effekseer storage reset");
        leases.Begin(1, 0);
        const auto parallelRecord = leases.Acquire();
        Require(parallelRecord.FrameSlot != mainRecord.FrameSlot,
            "Another host slot reused pending Effekseer storage");
        leases.Abort(1);
        leases.Begin(0, 10);
        const auto reusedRecord = leases.Acquire();
        Require(reusedRecord.FrameSlot == mainRecord.FrameSlot && reusedRecord.Record == mainRecord.Record,
            "Completed Effekseer storage was not reused");
        leases.Submitted(0, 10); // Late duplicate of the previous frame.
        Require(leases.Acquire().Record == previewRecord.Record,
            "Late duplicate submission closed a new host recording scope");
        leases.Abort(0);
        bBlocked = false;
        try { leases.Begin(0, 9); } catch (const std::logic_error&) { bBlocked = true; }
        Require(bBlocked, "Abort erased the previous GPU retirement fence");
        leases.Begin(0, 10);
        (void)leases.Acquire();
        leases.Submitted(0, 11);
        leases.Abort(0); // Abort after issue must not undo its fence.
        bBlocked = false;
        try { leases.Begin(0, 10); } catch (const std::logic_error&) { bBlocked = true; }
        Require(bBlocked, "Abort after submission made in-flight storage reusable");

        // Exercise the production adapter through Effekseer's actual culler.
        // Both graphics clip conventions and an orthographic camera must keep
        // an authored sphere in front of the camera, and reject one outside.
        const glm::vec3 cameraPosition(3.0f, 2.0f, 5.0f);
        const auto view = glm::lookAtRH(cameraPosition, glm::vec3(3.0f, 2.0f, -5.0f), glm::vec3(0, 1, 0));
        for (const auto& projection : {
            glm::perspectiveRH_NO(glm::radians(60.0f), 1.0f, 0.1f, 1000.0f),
            glm::perspectiveRH_ZO(glm::radians(60.0f), 1.0f, 0.1f, 1000.0f),
            glm::orthoRH_ZO(-10.0f, 10.0f, -10.0f, 10.0f, 0.1f, 1000.0f) })
        {
            const auto parameters = BuildEffekseerCameraDrawParameters(view, projection, 0.1f, 1000.0f, cameraPosition);
            Require(std::abs(parameters.ZFar - 1.0f) < 1e-4f,
                "Effekseer far clip parameter was a world distance instead of NDC depth");
            const auto planes = Effekseer::GeometryUtility::CalculateFrustumPlanes(
                parameters.ViewProjectionMatrix, parameters.ZNear, parameters.ZFar, EffekseerSceneCoordinateSystem);
            Require(Effekseer::GeometryUtility::IsContain(planes, { {3.0f, 2.0f, -5.0f}, 1.12f }),
                "Production Effekseer camera adapter culled a visible effect sphere");
            Require(!Effekseer::GeometryUtility::IsContain(planes, { {103.0f, 2.0f, -5.0f}, 1.12f }),
                "Production Effekseer camera adapter disabled sphere culling");
            Require(parameters.CameraFrontDirection.Z < -0.99f,
                "Effekseer depth clipping used the wrong camera forward direction");
        }

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
