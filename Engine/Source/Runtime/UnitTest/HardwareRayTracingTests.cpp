#include "Hepch.h"

#include "Runtime/UnitTest/HardwareRayTracingTests.h"

#include "Runtime/Graphics/GlobalIllumination/GlobalIllumination.h"
#include "Runtime/Graphics/GlobalIllumination/DepthPyramidPass.h"
#include "Runtime/Graphics/GlobalIllumination/GlobalIlluminationSignal.h"
#include "Runtime/Graphics/GlobalIllumination/GlobalIlluminationSettingsJson.h"
#include "Runtime/Graphics/GlobalIllumination/GlobalIlluminationTechniqueRegistry.h"
#if HE_WITH_OFFLINE_REFERENCE_RENDERER_TESTS
#include "Plugins/OfflineReferenceRenderer/Source/OfflineReferenceRendererSession.h"
#endif
#include "Runtime/Graphics/RayTracing/HardwareRayTracing.h"
#include "Runtime/Graphics/RayTracing/RayTracingScene.h"

#include <array>
#include <cmath>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

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

        struct RegistryTechniqueLifecycleCounters
        {
            uint32_t Prepare = 0;
            uint32_t AddPasses = 0;
            uint32_t ResetHistory = 0;
            uint32_t UpdateImportedResources = 0;
            uint32_t NotifyFrameSubmitted = 0;
            uint32_t AbortFrame = 0;
            uint32_t Collect = 0;
            uint32_t Clear = 0;
            uint64_t LastSubmission = 0;
            uint64_t LastCompletedSubmission = 0;
        };

        class RegistryTestGlobalIlluminationTechnique final :
            public IGlobalIlluminationTechnique
        {
        public:
            RegistryTestGlobalIlluminationTechnique(
                GlobalIlluminationTechniqueKind id,
                uint32_t* destroyedCount,
                RegistryTechniqueLifecycleCounters* lifecycle = nullptr) noexcept
                : mId(id)
                , mDestroyedCount(destroyedCount)
                , mLifecycle(lifecycle)
            {
            }

            ~RegistryTestGlobalIlluminationTechnique() override
            {
                if (mDestroyedCount)
                    ++*mDestroyedCount;
            }

            [[nodiscard]] bool IsSupported(
                const RenderCapabilities&) const override
            {
                return true;
            }

            [[nodiscard]] GlobalIlluminationPrepareResult PrepareFrame(
                const GlobalIlluminationFrameContext&,
                const GlobalIlluminationSettings&) override
            {
                if (mLifecycle)
                    ++mLifecycle->Prepare;
                return {
                    .bGraphRebuildRequested = mLifecycle != nullptr,
                };
            }

            [[nodiscard]] GlobalIlluminationOutputs AddPasses(
                RenderGraph&,
                const GlobalIlluminationInputs&,
                const GlobalIlluminationSettings&) override
            {
                if (mLifecycle)
                    ++mLifecycle->AddPasses;
                return {};
            }

            void ResetHistory(GlobalIlluminationHistoryResetReason) override
            {
                if (mLifecycle)
                    ++mLifecycle->ResetHistory;
            }

            void UpdateImportedResources(RenderGraph&) override
            {
                if (mLifecycle)
                    ++mLifecycle->UpdateImportedResources;
            }

            void NotifyFrameSubmitted(uint64_t submission) noexcept override
            {
                if (mLifecycle)
                {
                    ++mLifecycle->NotifyFrameSubmitted;
                    mLifecycle->LastSubmission = submission;
                }
            }

            void AbortFrame() noexcept override
            {
                if (mLifecycle)
                    ++mLifecycle->AbortFrame;
            }

            void Collect(uint64_t completedSubmission) override
            {
                if (mLifecycle)
                {
                    ++mLifecycle->Collect;
                    mLifecycle->LastCompletedSubmission = completedSubmission;
                }
            }

            void Clear(uint64_t completedSubmission) override
            {
                if (mLifecycle)
                {
                    ++mLifecycle->Clear;
                    mLifecycle->LastCompletedSubmission = completedSubmission;
                }
            }

            [[nodiscard]] bool RequiresContinuousUpdate() const noexcept override
            {
                return false;
            }

            [[nodiscard]] GlobalIlluminationStats GetStats() const override
            {
                return { .Technique = mId };
            }

        private:
            GlobalIlluminationTechniqueKind mId;
            uint32_t* mDestroyedCount = nullptr;
            RegistryTechniqueLifecycleCounters* mLifecycle = nullptr;
        };

        [[nodiscard]] GlobalIlluminationTechniqueDescriptor
            MakeRegistryTestDescriptor(
                GlobalIlluminationTechniqueKind id,
                std::string displayName,
                uint32_t& createdCount,
                uint32_t& destroyedCount,
                RegistryTechniqueLifecycleCounters* lifecycle = nullptr)
        {
            return {
                .Id = id,
                .DisplayName = std::move(displayName),
                .Factory = [&createdCount, &destroyedCount, lifecycle, id]() ->
                    Scope<IGlobalIlluminationTechnique>
                {
                    ++createdCount;
                    return CreateScope<RegistryTestGlobalIlluminationTechnique>(
                        id,
                        &destroyedCount,
                        lifecycle);
                },
                .CapabilityRequirements =
                    GlobalIlluminationTechniqueCapabilityRequirements::GraphicsQueue |
                    GlobalIlluminationTechniqueCapabilityRequirements::
                        RayTracingAccelerationStructure |
                    GlobalIlluminationTechniqueCapabilityRequirements::
                        RayTracingPipeline,
                .RequiredInputs =
                    GlobalIlluminationTechniqueRequiredInput::SceneDepth |
                    GlobalIlluminationTechniqueRequiredInput::NormalRoughness |
                    GlobalIlluminationTechniqueRequiredInput::SceneTlas,
                .OutputSemantics = {
                    .Outputs =
                        GlobalIlluminationTechniqueOutput::
                            DiffuseIndirectRadiance |
                        GlobalIlluminationTechniqueOutput::HitDistance |
                        GlobalIlluminationTechniqueOutput::Confidence |
                        GlobalIlluminationTechniqueOutput::CoverageMask,
                },
                .SupportedDebugViews = {
                    GlobalIlluminationDebugView::Composite,
                    GlobalIlluminationDebugView::RawIndirectDiffuse,
                    GlobalIlluminationDebugView::HitDistance,
                    GlobalIlluminationDebugView::Confidence,
                },
            };
        }

        [[nodiscard]] bool NearlyEqual(
            const glm::vec3& left,
            const glm::vec3& right,
            float epsilon = 1e-5f)
        {
            return std::abs(left.x - right.x) <= epsilon &&
                std::abs(left.y - right.y) <= epsilon &&
                std::abs(left.z - right.z) <= epsilon;
        }

        [[nodiscard]] bool IsFinite(const glm::vec3& value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y) &&
                std::isfinite(value.z);
        }

        [[nodiscard]] Ref<SubMesh> MakeStaticTriangle()
        {
            const std::vector<StaticVertex> vertices = {
                StaticVertex(
                    { 0.0f, 0.0f, 0.0f },
                    { 0.0f, 1.0f, 0.0f },
                    { 0.0f, 0.0f },
                    { 1.0f, 0.0f, 0.0f },
                    { 0.0f, 0.0f, 1.0f }),
                StaticVertex(
                    { 1.0f, 0.0f, 0.0f },
                    { 0.0f, 1.0f, 0.0f },
                    { 1.0f, 0.0f },
                    { 1.0f, 0.0f, 0.0f },
                    { 0.0f, 0.0f, 1.0f }),
                StaticVertex(
                    { 0.0f, 0.0f, 1.0f },
                    { 0.0f, 1.0f, 0.0f },
                    { 0.0f, 1.0f },
                    { 1.0f, 0.0f, 0.0f },
                    { 0.0f, 0.0f, 1.0f }),
            };
            return CreateRef<SubMesh>(vertices, std::vector<uint32_t>{ 0, 1, 2 });
        }

        void TestCapabilityAndFallbackPolicy()
        {
            RenderCapabilities capabilities;
            ResolveRendererCapabilityPolicy(capabilities);
            const HardwareRayTracingSupport unsupported =
                QueryHardwareRayTracingSupport(capabilities);
            Require(!unsupported.bRealtimeRtgi,
                "A raster-only device reported a hardware ray-tracing technique.");

            GlobalIlluminationSettings settings;
            settings.Technique = GlobalIlluminationTechniqueKind::HardwareRayTracing;
            settings.UnsupportedFallback =
                GlobalIlluminationFallbackKind::ScreenSpace;
            const GlobalIlluminationSelection ssgiFallback =
                ResolveGlobalIlluminationSelection(settings, capabilities, true);
            Require(ssgiFallback.Selected ==
                    GlobalIlluminationTechniqueKind::ScreenSpace &&
                    ssgiFallback.bFallback &&
                    !ssgiFallback.bUsesHardwareRayTracing,
                "Unsupported hardware RTGI did not select the explicit SSGI fallback.");

            const GlobalIlluminationSelection offFallback =
                ResolveGlobalIlluminationSelection(settings, capabilities, false);
            Require(offFallback.Selected == GlobalIlluminationTechniqueKind::Off &&
                    offFallback.bFallback &&
                    !offFallback.bUsesHardwareRayTracing,
                "Unsupported hardware RTGI did not preserve raster with GI Off.");

            capabilities.RayTracingAccelStruct = {
                .bSupported = true,
                .bEnabled = true,
                .Reason = "Synthetic acceleration structures",
            };
            capabilities.RayTracingPipeline = {
                .bSupported = true,
                .bEnabled = true,
                .Reason = "Synthetic ray-tracing pipeline",
            };
            ResolveRendererCapabilityPolicy(capabilities, true);
            const HardwareRayTracingSupport supported =
                QueryHardwareRayTracingSupport(capabilities);
            Require(supported.bRealtimeRtgi &&
                    capabilities.Profile == RendererProfile::HardwareRayTracing &&
                    std::string_view(ToString(capabilities.Profile)) ==
                        "Hardware Ray Tracing",
                "NVRHI hardware-ray-tracing readiness was not exposed generically.");
        }

        void TestHistoryInvalidation()
        {
            GlobalIlluminationHistoryTracker tracker;
            GlobalIlluminationHistorySignature signature{
                .Width = 1920,
                .Height = 1080,
                .RenderScale = 1.0f,
                .ProjectionFingerprint = 10,
                .JitterSequenceGeneration = 20,
                .SceneGeneration = 30,
                .GeometryTopologyGeneration = 40,
                .MaterialGeneration = 50,
                .LightGeneration = 60,
                .Technique = GlobalIlluminationTechniqueKind::HardwareRayTracing,
                .Denoiser = GlobalIlluminationDenoiserKind::Svgf,
                .SamplesPerPixel = 1,
                .MaxBounces = 4,
                .bHalfResolution = false,
            };
            Require(HasHistoryResetReason(
                    tracker.Update(signature),
                    GlobalIlluminationHistoryResetReason::FirstFrame),
                "The first GI frame did not reset history.");
            Require(tracker.Update(signature) ==
                    GlobalIlluminationHistoryResetReason::None,
                "An unchanged frame reset GI history; ordinary object motion must use motion/disocclusion instead.");

            GlobalIlluminationHistorySignature changed = signature;
            changed.Width = 2560;
            changed.RenderScale = 0.5f;
            changed.ProjectionFingerprint++;
            changed.JitterSequenceGeneration++;
            changed.SceneGeneration++;
            changed.GeometryTopologyGeneration++;
            changed.MaterialGeneration++;
            changed.LightGeneration++;
            changed.TechniqueStateFingerprint++;
            changed.Technique =
                GlobalIlluminationTechniqueKind::ExternalViewportRenderer;
            changed.Denoiser = GlobalIlluminationDenoiserKind::None;
            changed.SamplesPerPixel = 2;
            changed.MaxBounces = 8;
            changed.bHalfResolution = true;
            changed.bCameraCut = true;
            const GlobalIlluminationHistoryResetReason reasons =
                tracker.Update(changed);
            for (GlobalIlluminationHistoryResetReason reason : {
                GlobalIlluminationHistoryResetReason::Resize,
                GlobalIlluminationHistoryResetReason::RenderScale,
                GlobalIlluminationHistoryResetReason::CameraCut,
                GlobalIlluminationHistoryResetReason::Projection,
                GlobalIlluminationHistoryResetReason::JitterSequence,
                GlobalIlluminationHistoryResetReason::SceneReload,
                GlobalIlluminationHistoryResetReason::GeometryTopology,
                GlobalIlluminationHistoryResetReason::Material,
                GlobalIlluminationHistoryResetReason::Light,
                GlobalIlluminationHistoryResetReason::Technique,
                GlobalIlluminationHistoryResetReason::BounceCount,
                GlobalIlluminationHistoryResetReason::ResolutionMode,
                GlobalIlluminationHistoryResetReason::Denoiser,
                GlobalIlluminationHistoryResetReason::Sampling,
                GlobalIlluminationHistoryResetReason::ExternalRendererState,
            })
            {
                Require(HasHistoryResetReason(reasons, reason),
                    "A required GI history reset reason was omitted.");
            }

            changed.bCameraCut = false;
            tracker.Reset(GlobalIlluminationHistoryResetReason::Explicit);
            Require(HasHistoryResetReason(
                    tracker.Update(changed),
                    GlobalIlluminationHistoryResetReason::Explicit),
                "An explicit GI history reset was lost.");

#if HE_WITH_OFFLINE_REFERENCE_RENDERER_TESTS
            ReferencePathTracerFrameParameters referenceFrame;
            referenceFrame.Camera.View = glm::mat4(1.0f);
            referenceFrame.Camera.Projection = glm::mat4(1.0f);
            referenceFrame.Camera.ViewProjection = glm::mat4(1.0f);
            referenceFrame.Camera.Position = glm::vec3(0.0f);
            referenceFrame.SceneStateFingerprint = 100;
            GlobalIlluminationSettings referenceSettings;
            referenceSettings.Technique =
                GlobalIlluminationTechniqueKind::ExternalViewportRenderer;
            referenceSettings.Denoiser = GlobalIlluminationDenoiserKind::None;
            const uint64_t stableReferenceFingerprint =
                ComputeReferencePathTracerAccumulationFingerprint(
                    referenceFrame,
                    referenceSettings);
            Require(stableReferenceFingerprint ==
                    ComputeReferencePathTracerAccumulationFingerprint(
                        referenceFrame,
                        referenceSettings),
                "An unchanged reference frame produced an unstable accumulation fingerprint.");
            referenceFrame.Camera.Position.x += 1.0f;
            Require(stableReferenceFingerprint !=
                    ComputeReferencePathTracerAccumulationFingerprint(
                        referenceFrame,
                        referenceSettings),
                "Reference camera motion did not invalidate accumulation.");
            referenceFrame.Camera.Position.x = 0.0f;
            ++referenceFrame.SceneStateFingerprint;
            Require(stableReferenceFingerprint !=
                    ComputeReferencePathTracerAccumulationFingerprint(
                        referenceFrame,
                        referenceSettings),
                "Reference instance motion did not invalidate accumulation.");
            --referenceFrame.SceneStateFingerprint;
            Require(stableReferenceFingerprint ==
                    ComputeReferencePathTracerAccumulationFingerprint(
                        referenceFrame,
                        referenceSettings),
                "An unchanged reference integrator state invalidated accumulation.");
            referenceSettings.bDirectOnly = true;
            Require(stableReferenceFingerprint !=
                    ComputeReferencePathTracerAccumulationFingerprint(
                        referenceFrame,
                        referenceSettings),
                "Reference integrator setting changes did not invalidate accumulation.");
#endif
        }

        void TestGeometryCacheAndSupportPolicy()
        {
            const Ref<SubMesh> mesh = MakeStaticTriangle();
            const RayTracingGeometryCacheKey original =
                MakeRayTracingGeometryCacheKey(
                    "Assets/Cornell/Cornell.hmesh",
                    0,
                    *mesh,
                    RenderMaterialAlphaMode::Opaque,
                    true);
            const RayTracingGeometryCacheKey identical =
                MakeRayTracingGeometryCacheKey(
                    "Assets/Cornell/Cornell.hmesh",
                    0,
                    *mesh,
                    RenderMaterialAlphaMode::Opaque,
                    true);
            Require(original == identical,
                "Identical geometry produced an unstable BLAS cache key.");

            mesh->mStaticVertices[0].Pos.x += 0.125f;
            const RayTracingGeometryCacheKey modified =
                MakeRayTracingGeometryCacheKey(
                    "Assets/Cornell/Cornell.hmesh",
                    0,
                    *mesh,
                    RenderMaterialAlphaMode::Opaque,
                    true);
            Require(original.ContentRevision != modified.ContentRevision &&
                    original != modified,
                "Vertex content mutation did not invalidate the BLAS cache key.");
            const RayTracingGeometryCacheKey masked =
                MakeRayTracingGeometryCacheKey(
                    "Assets/Cornell/Cornell.hmesh",
                    0,
                    *mesh,
                    RenderMaterialAlphaMode::Mask,
                    false);
            Require(masked != modified,
                "Opacity/alpha changes did not invalidate the BLAS cache key.");
            const RayTracingGeometryCacheKey doubleSided =
                MakeRayTracingGeometryCacheKey(
                    "Assets/Cornell/Cornell.hmesh",
                    0,
                    *mesh,
                    RenderMaterialAlphaMode::Opaque,
                    true,
                    true);
            Require(doubleSided != modified,
                "Sidedness changes did not invalidate the geometry/material record cache key.");

            RenderObject object;
            object.Path = RenderPath::CpuOpaque;
            RenderMeshSection section;
            section.SubMeshResource = mesh;
            section.AlphaMode = RenderMaterialAlphaMode::Opaque;
            Require(ClassifyRayTracingGeometry(object, section) ==
                    RayTracingGeometrySupport::SupportedStaticOpaque,
                "Static opaque geometry was excluded from hardware RT.");
            section.AlphaMode = RenderMaterialAlphaMode::Mask;
            Require(ClassifyRayTracingGeometry(object, section) ==
                    RayTracingGeometrySupport::AlphaMask,
                "Alpha-mask geometry did not select the documented raster fallback.");
            section.AlphaMode = RenderMaterialAlphaMode::Opaque;
            section.MaterialInstance.bCustomSurface = true;
            Require(ClassifyRayTracingGeometry(object, section) ==
                    RayTracingGeometrySupport::CustomMaterialShader,
                "Custom material geometry was not explicitly excluded.");
            section.MaterialInstance.bCustomSurface = false;
            section.MaterialInstance.LegacyTextures[0].bEnabled = true;
            Require(ClassifyRayTracingGeometry(object, section) ==
                    RayTracingGeometrySupport::TexturedMaterial,
                "Textured BasePBR geometry was silently admitted without hit-shader texture sampling.");
            section.MaterialInstance.LegacyTextures[0].bEnabled = false;
            mesh->mStaticVertices[0].Normal = glm::normalize(
                glm::vec3(1.0f, 1.0f, 0.0f));
            Require(ClassifyRayTracingGeometry(object, section) ==
                    RayTracingGeometrySupport::SmoothShadingNormals,
                "Smooth shading normals were silently admitted without a shading-normal correction.");
        }

        void TestStableInstanceMapping()
        {
            RayTracingStableInstanceRegistry registry;
            const RayTracingInstanceIdentity first{
                .SceneGeneration = 7,
                .EntityId = 42,
                .ObjectOrdinal = 3,
                .SectionIndex = 0,
                .MeshAssetPath = "Assets/Cornell/Cornell.hmesh",
            };
            const uint32_t firstId = registry.Resolve(first);
            Require(firstId == registry.Resolve(first) && registry.Size() == 1,
                "A stable RT instance changed ID within one scene generation.");
            RayTracingInstanceIdentity reordered = first;
            reordered.ObjectOrdinal = 99;
            Require(registry.Resolve(reordered) == firstId && registry.Size() == 1,
                "Entity-backed RT instance ID changed when submission order changed.");
            RayTracingInstanceIdentity second = first;
            second.SectionIndex = 1;
            Require(registry.Resolve(second) != firstId && registry.Size() == 2,
                "Two mesh sections aliased one stable RT instance ID.");
            RayTracingInstanceIdentity reloaded = first;
            reloaded.SceneGeneration = 8;
            Require(registry.Resolve(reloaded) == 0 && registry.Size() == 1 &&
                    registry.GetSceneGeneration() == 8,
                "Scene reload did not restart the stable instance namespace.");
        }

        void TestEmissiveTriangleDistribution()
        {
            std::vector<RayTracingEmissiveTriangle> triangles(2);
            triangles[0].NormalArea.w = 2.0f;
            triangles[0].RadianceCdf = glm::vec4(1.0f);
            triangles[1].NormalArea.w = 1.0f;
            triangles[1].RadianceCdf = glm::vec4(1.0f);
            const float total =
                BuildRayTracingEmissiveTriangleDistribution(triangles);
            Require(std::abs(total - 3.0f) < 1e-5f &&
                    std::abs(triangles[0].RadianceCdf.w - 2.0f / 3.0f) < 1e-5f &&
                    triangles[1].RadianceCdf.w == 1.0f,
                "Emissive triangle CDF is not proportional to area*luminance.");

            triangles[0].NormalArea.w = -1.0f;
            triangles[0].RadianceCdf = glm::vec4(-1.0f);
            triangles[1].NormalArea.w = 0.0f;
            triangles[1].RadianceCdf = glm::vec4(0.0f);
            Require(BuildRayTracingEmissiveTriangleDistribution(triangles) == 0.0f &&
                    triangles[0].RadianceCdf.w == 0.0f &&
                    triangles[1].RadianceCdf.w == 0.0f,
                "A zero-energy light table produced a non-zero PDF.");
        }

        void TestGlobalIlluminationTechniqueRegistry()
        {
            GlobalIlluminationTechniqueRegistry registry;
            uint32_t hardwareCreatedCount = 0;
            uint32_t hardwareDestroyedCount = 0;
            uint32_t screenSpaceCreatedCount = 0;
            uint32_t screenSpaceDestroyedCount = 0;
            RegistryTechniqueLifecycleCounters hardwareLifecycle;

            GlobalIlluminationTechniqueDescriptor hardwareDescriptor =
                MakeRegistryTestDescriptor(
                    GlobalIlluminationTechniqueKind::HardwareRayTracing,
                    "Hardware RTGI",
                    hardwareCreatedCount,
                    hardwareDestroyedCount,
                    &hardwareLifecycle);
            GlobalIlluminationTechniqueDescriptor screenSpaceDescriptor =
                MakeRegistryTestDescriptor(
                    GlobalIlluminationTechniqueKind::ScreenSpace,
                    "Screen Space GI",
                    screenSpaceCreatedCount,
                    screenSpaceDestroyedCount);
            screenSpaceDescriptor.CapabilityRequirements =
                GlobalIlluminationTechniqueCapabilityRequirements::ComputeQueue;
            screenSpaceDescriptor.RequiredInputs =
                GlobalIlluminationTechniqueRequiredInput::SceneDepth |
                GlobalIlluminationTechniqueRequiredInput::NormalRoughness |
                GlobalIlluminationTechniqueRequiredInput::MotionVectors;

            registry.Register(std::move(hardwareDescriptor));
            registry.Register(std::move(screenSpaceDescriptor));
            Require(registry.Contains(
                        GlobalIlluminationTechniqueKind::HardwareRayTracing) &&
                    registry.Contains(
                        GlobalIlluminationTechniqueKind::ScreenSpace),
                "Registered GI techniques were not discoverable.");

            const std::vector<GlobalIlluminationTechniqueDescriptor> descriptors =
                registry.Enumerate();
            Require(descriptors.size() == 2 &&
                    descriptors[0].Id ==
                        GlobalIlluminationTechniqueKind::HardwareRayTracing &&
                    descriptors[1].Id ==
                        GlobalIlluminationTechniqueKind::ScreenSpace &&
                    descriptors[0].DisplayName == "Hardware RTGI" &&
                    descriptors[1].DisplayName == "Screen Space GI",
                "GI technique enumeration did not preserve registration order and metadata.");
            Require(HasGlobalIlluminationTechniqueRequiredInput(
                        descriptors[0].RequiredInputs,
                        GlobalIlluminationTechniqueRequiredInput::SceneTlas) &&
                    HasGlobalIlluminationTechniqueOutput(
                        descriptors[0].OutputSemantics.Outputs,
                        GlobalIlluminationTechniqueOutput::
                            DiffuseIndirectRadiance) &&
                    descriptors[0].OutputSemantics.bSceneLinearHdr &&
                    descriptors[0].OutputSemantics.bPreExposed &&
                    descriptors[0].OutputSemantics.bPreTonemap &&
                    descriptors[0].SupportedDebugViews ==
                        std::vector<GlobalIlluminationDebugView>{
                            GlobalIlluminationDebugView::Composite,
                            GlobalIlluminationDebugView::RawIndirectDiffuse,
                            GlobalIlluminationDebugView::HitDistance,
                            GlobalIlluminationDebugView::Confidence,
                        },
                "GI descriptor lost its input, output, or debug-view contract.");

            RenderCapabilities capabilities;
            Require(!descriptors[0].AreCapabilitiesSatisfied(capabilities) &&
                    !descriptors[1].AreCapabilitiesSatisfied(capabilities),
                "GI capability requirements admitted an unsupported device.");
            capabilities.bGraphicsQueue = true;
            capabilities.bComputeQueue = true;
            capabilities.RayTracingAccelStruct = {
                .bSupported = true,
                .bEnabled = true,
            };
            capabilities.RayTracingPipeline = {
                .bSupported = true,
                .bEnabled = true,
            };
            Require(descriptors[0].AreCapabilitiesSatisfied(capabilities) &&
                    descriptors[1].AreCapabilitiesSatisfied(capabilities),
                "GI capability requirements rejected a synthetic supported device.");
            capabilities.RayTracingPipeline.bEnabled = false;
            Require(!descriptors[0].AreCapabilitiesSatisfied(capabilities) &&
                    descriptors[1].AreCapabilitiesSatisfied(capabilities),
                "GI capability checks did not isolate hardware RT from SSGI.");

            RequireThrows<std::logic_error>(
                [&registry, &hardwareCreatedCount, &hardwareDestroyedCount]()
                {
                    registry.Register(MakeRegistryTestDescriptor(
                        GlobalIlluminationTechniqueKind::HardwareRayTracing,
                        "Duplicate Hardware RTGI",
                        hardwareCreatedCount,
                        hardwareDestroyedCount));
                },
                "Duplicate GI technique registration was not rejected.");
            Require(registry.Enumerate().size() == 2,
                "Duplicate GI registration mutated the stable registry order.");

            const GlobalIlluminationTechniqueKind unknownId =
                static_cast<GlobalIlluminationTechniqueKind>(255);
            RequireThrows<std::out_of_range>(
                [&registry, unknownId]()
                {
                    (void)registry.Create(unknownId);
                },
                "Creating an unknown GI technique did not report an error.");
            RequireThrows<std::out_of_range>(
                [&registry, unknownId]()
                {
                    registry.Unregister(unknownId);
                },
                "Unregistering an unknown GI technique did not report an error.");

            GlobalIlluminationTechniqueLease hardware = registry.Create(
                GlobalIlluminationTechniqueKind::HardwareRayTracing);
            Require(hardware &&
                    hardware.GetId() ==
                        GlobalIlluminationTechniqueKind::HardwareRayTracing &&
                    hardware->GetStats().Technique ==
                        GlobalIlluminationTechniqueKind::HardwareRayTracing &&
                    hardwareCreatedCount == 1 &&
                    registry.GetActiveLeaseCount(
                        GlobalIlluminationTechniqueKind::HardwareRayTracing) == 1,
                "Creating a registered GI technique did not return an active lease.");
            RenderGraph lifecycleGraph;
            const GlobalIlluminationPrepareResult prepareResult =
                hardware->PrepareFrame(
                    GlobalIlluminationFrameContext{},
                    GlobalIlluminationSettings{});
            (void)hardware->AddPasses(
                lifecycleGraph,
                GlobalIlluminationInputs{},
                GlobalIlluminationSettings{});
            hardware->ResetHistory(
                GlobalIlluminationHistoryResetReason::Explicit);
            hardware->UpdateImportedResources(lifecycleGraph);
            hardware->NotifyFrameSubmitted(41);
            hardware->AbortFrame();
            hardware->Collect(37);
            hardware->Clear(37);
            Require(prepareResult.bGraphRebuildRequested &&
                    hardwareLifecycle.Prepare == 1 &&
                    hardwareLifecycle.AddPasses == 1 &&
                    hardwareLifecycle.ResetHistory == 1 &&
                    hardwareLifecycle.UpdateImportedResources == 1 &&
                    hardwareLifecycle.NotifyFrameSubmitted == 1 &&
                    hardwareLifecycle.AbortFrame == 1 &&
                    hardwareLifecycle.Collect == 1 &&
                    hardwareLifecycle.Clear == 1 &&
                    hardwareLifecycle.LastSubmission == 41 &&
                    hardwareLifecycle.LastCompletedSubmission == 37,
                "GI registry lease did not dispatch the complete technique lifecycle through the interface.");
            RequireThrows<std::logic_error>(
                [&registry]()
                {
                    registry.Unregister(
                        GlobalIlluminationTechniqueKind::HardwareRayTracing);
                },
                "An active GI technique was illegally unregistered.");

            GlobalIlluminationTechniqueLease movedHardware = std::move(hardware);
            Require(!hardware && movedHardware &&
                    registry.GetActiveLeaseCount(
                        GlobalIlluminationTechniqueKind::HardwareRayTracing) == 1,
                "Moving a GI technique lease changed its active acquisition count.");
            movedHardware.Release();
            Require(hardwareDestroyedCount == 1 &&
                    registry.GetActiveLeaseCount(
                        GlobalIlluminationTechniqueKind::HardwareRayTracing) == 0,
                "Releasing a GI technique lease did not destroy and deactivate it.");
            registry.Unregister(
                GlobalIlluminationTechniqueKind::HardwareRayTracing);
            Require(!registry.Contains(
                        GlobalIlluminationTechniqueKind::HardwareRayTracing) &&
                    registry.Enumerate().size() == 1 &&
                    registry.Enumerate()[0].Id ==
                        GlobalIlluminationTechniqueKind::ScreenSpace,
                "GI technique unregistration removed or reordered the wrong descriptor.");

            GlobalIlluminationTechniqueDescriptor nullFactoryDescriptor =
                MakeRegistryTestDescriptor(
                    GlobalIlluminationTechniqueKind::ExternalViewportRenderer,
                    "Null Factory",
                    hardwareCreatedCount,
                    hardwareDestroyedCount);
            nullFactoryDescriptor.Factory = []() ->
                Scope<IGlobalIlluminationTechnique>
            {
                return nullptr;
            };
            registry.Register(std::move(nullFactoryDescriptor));
            RequireThrows<std::runtime_error>(
                [&registry]()
                {
                    (void)registry.Create(
                        GlobalIlluminationTechniqueKind::ExternalViewportRenderer);
                },
                "A null GI factory result was not rejected.");
            Require(registry.GetActiveLeaseCount(
                        GlobalIlluminationTechniqueKind::ExternalViewportRenderer) == 0,
                "A failed GI factory leaked an active acquisition.");
            registry.Unregister(
                GlobalIlluminationTechniqueKind::ExternalViewportRenderer);

            Require(screenSpaceCreatedCount == 0 &&
                    screenSpaceDestroyedCount == 0,
                "GI registry enumeration or capability checks instantiated a technique.");
        }

        void TestGlobalIlluminationDiffuseSignal()
        {
            const glm::vec3 baseColor(0.8f, 0.5f, 0.2f);
            const float metallic = 0.25f;
            const glm::vec3 expectedAlbedo(0.6f, 0.375f, 0.15f);
            Require(NearlyEqual(
                    ComputeGlobalIlluminationDiffuseAlbedo(
                        baseColor,
                        metallic),
                    expectedAlbedo),
                "Diffuse albedo is not baseColor * (1 - metallic).");

            const glm::vec3 expectedIllumination(2.0f, 0.5f, 4.0f);
            const glm::vec3 radiance =
                expectedAlbedo * expectedIllumination;
            const GlobalIlluminationDiffuseSignal signal =
                DemodulateGlobalIlluminationDiffuse(
                    radiance,
                    baseColor,
                    metallic,
                    false);
            Require(signal.bHasDiffuseResponse &&
                    NearlyEqual(signal.DiffuseAlbedo, expectedAlbedo) &&
                    NearlyEqual(
                        signal.DemodulatedIllumination,
                        expectedIllumination) &&
                    NearlyEqual(
                        RemodulateGlobalIlluminationDiffuse(signal),
                        radiance),
                "Component-wise diffuse demodulation/remodulation did not round-trip.");

            const GlobalIlluminationDiffuseSignal zeroAlbedo =
                DemodulateGlobalIlluminationDiffuse(
                    glm::vec3(4.0f, 2.0f, 1.0f),
                    glm::vec3(0.0f),
                    0.0f,
                    false);
            Require(!zeroAlbedo.bHasDiffuseResponse &&
                    zeroAlbedo.DemodulatedIllumination == glm::vec3(0.0f) &&
                    RemodulateGlobalIlluminationDiffuse(zeroAlbedo) ==
                        glm::vec3(0.0f),
                "Zero diffuse albedo did not produce a defined zero signal.");

            const GlobalIlluminationDiffuseSignal mixedZeroAlbedo =
                DemodulateGlobalIlluminationDiffuse(
                    glm::vec3(3.0f, 1.0f, 2.0f),
                    glm::vec3(0.0f, 0.5f, 0.0f),
                    0.0f,
                    false);
            Require(NearlyEqual(
                    mixedZeroAlbedo.DemodulatedIllumination,
                    glm::vec3(0.0f, 2.0f, 0.0f)) &&
                    NearlyEqual(
                        RemodulateGlobalIlluminationDiffuse(mixedZeroAlbedo),
                        glm::vec3(0.0f, 1.0f, 0.0f)),
                "A zero-albedo component leaked diffuse GI during demodulation.");

            for (const GlobalIlluminationDiffuseSignal& noDiffuse : {
                DemodulateGlobalIlluminationDiffuse(
                    radiance,
                    baseColor,
                    0.0f,
                    true),
                DemodulateGlobalIlluminationDiffuse(
                    radiance,
                    baseColor,
                    1.0f,
                    false),
            })
            {
                Require(!noDiffuse.bHasDiffuseResponse &&
                        noDiffuse.DemodulatedIllumination == glm::vec3(0.0f) &&
                        RemodulateGlobalIlluminationDiffuse(noDiffuse) ==
                            glm::vec3(0.0f),
                    "Emissive or fully-metallic surface retained diffuse GI.");
            }

            const GlobalIlluminationDiffuseSignal lowAlbedo =
                DemodulateGlobalIlluminationDiffuse(
                    glm::vec3(1.0f),
                    glm::vec3(1e-8f, 2e-8f, 3e-8f),
                    0.0f,
                    false);
            const glm::vec3 lowAlbedoRadiance =
                RemodulateGlobalIlluminationDiffuse(lowAlbedo);
            Require(lowAlbedo.bHasDiffuseResponse &&
                    IsFinite(lowAlbedo.DemodulatedIllumination) &&
                    IsFinite(lowAlbedoRadiance) &&
                    lowAlbedo.DemodulatedIllumination.x <= 10000.0f &&
                    lowAlbedo.DemodulatedIllumination.y <= 10000.0f &&
                    lowAlbedo.DemodulatedIllumination.z <= 10000.0f,
                "Epsilon-protected low-albedo demodulation was unstable or non-finite.");
        }

        void TestGlobalIlluminationPixelMapping()
        {
            Require(MapGlobalIlluminationTracePixelToFullPixel(
                    { 0u, 0u },
                    { 1920u, 1080u },
                    { 1920u, 1080u }) == glm::uvec2(0u, 0u) &&
                    MapGlobalIlluminationTracePixelToFullPixel(
                        { 1919u, 1079u },
                        { 1920u, 1080u },
                        { 1920u, 1080u }) == glm::uvec2(1919u, 1079u),
                "Full-resolution trace-to-full mapping missed an image boundary.");

            Require(MapGlobalIlluminationTracePixelToFullPixel(
                    { 0u, 0u },
                    { 960u, 540u },
                    { 1920u, 1080u }) == glm::uvec2(1u, 1u) &&
                    MapGlobalIlluminationTracePixelToFullPixel(
                        { 959u, 539u },
                        { 960u, 540u },
                        { 1920u, 1080u }) == glm::uvec2(1919u, 1079u),
                "Half-resolution trace-to-full mapping missed an image boundary.");

            Require(MapGlobalIlluminationTracePixelToFullPixel(
                    { 1u, 0u },
                    { 3u, 2u },
                    { 5u, 3u }) == glm::uvec2(2u, 0u) &&
                    MapGlobalIlluminationTracePixelToFullPixel(
                        { 2u, 1u },
                        { 3u, 2u },
                        { 5u, 3u }) == glm::uvec2(4u, 2u) &&
                    MapGlobalIlluminationTracePixelToFullPixel(
                        { 99u, 99u },
                        { 3u, 2u },
                        { 5u, 3u }) == glm::uvec2(4u, 2u) &&
                    MapGlobalIlluminationTracePixelToFullPixel(
                        { 0u, 0u },
                        { 0u, 0u },
                        { 5u, 3u }) == glm::uvec2(0u, 0u),
                "Odd, clamped, or degenerate trace-to-full mapping was undefined.");
        }

        void TestGlobalIlluminationSpatialCompatibility()
        {
            const GlobalIlluminationSpatialSurface center{
                .MaterialId = 7,
                .DiffuseAlbedo = glm::vec3(0.8f, 0.2f, 0.1f),
                .Roughness = 0.4f,
            };
            GlobalIlluminationSpatialSurface sample = center;
            sample.MaterialId = 8;
            Require(ComputeGlobalIlluminationSpatialCompatibility(
                    center,
                    sample) == 0.0f,
                "A material boundary retained a non-zero spatial weight.");

            sample = center;
            const float identicalWeight =
                ComputeGlobalIlluminationSpatialCompatibility(center, sample);
            sample.DiffuseAlbedo = glm::vec3(0.5f, 0.25f, 0.15f);
            const float albedoWeight =
                ComputeGlobalIlluminationSpatialCompatibility(center, sample);
            sample = center;
            sample.Roughness = 0.7f;
            const float roughnessWeight =
                ComputeGlobalIlluminationSpatialCompatibility(center, sample);
            sample.DiffuseAlbedo = glm::vec3(0.5f, 0.25f, 0.15f);
            const float combinedWeight =
                ComputeGlobalIlluminationSpatialCompatibility(center, sample);
            Require(identicalWeight == 1.0f &&
                    std::isfinite(albedoWeight) && albedoWeight > 0.0f &&
                    albedoWeight < identicalWeight &&
                    std::isfinite(roughnessWeight) && roughnessWeight > 0.0f &&
                    roughnessWeight < identicalWeight &&
                    std::isfinite(combinedWeight) && combinedWeight > 0.0f &&
                    combinedWeight < albedoWeight &&
                    combinedWeight < roughnessWeight,
                "Same-material albedo/roughness compatibility was not finite and selective.");
        }

        void TestDepthPyramidReduction()
        {
            Require(CalculateMipCount(0, 3) == 0 &&
                    CalculateMipCount(1, 1) == 1 &&
                    CalculateMipCount(5, 3) == 3 &&
                    CalculateMipCount(512, 512) == 10,
                "Depth-pyramid mip count is not defined for degenerate, odd, and power-of-two dimensions.");
            const std::array<float, 15> source = {
                0.9f, 0.8f, 0.7f, 0.6f, 0.5f,
                0.4f, 0.3f, 0.2f, 0.1f, 0.95f,
                0.85f, 0.75f, 0.65f, 0.55f, 0.01f,
            };
            const std::vector<float> reduced = ReduceMinDepth(source, 5, 3);
            Require(reduced.size() == 2 &&
                    std::abs(reduced[0] - 0.3f) < 1e-6f &&
                    std::abs(reduced[1] - 0.01f) < 1e-6f,
                "Odd-size min-depth reduction dropped the final source row or column.");
            Require(std::abs(ReduceMinDepth(
                        std::span<const float>(source.data(), 4)) - 0.6f) < 1e-6f,
                "Normal-Z depth reduction did not preserve the nearest occluder.");
        }

        void TestGlobalIlluminationSettingsJson()
        {
            GlobalIlluminationSettings settings;
            settings.Technique = GlobalIlluminationTechniqueKind::ScreenSpace;
            settings.Denoiser = GlobalIlluminationDenoiserKind::Svgf;
            settings.UnsupportedFallback = GlobalIlluminationFallbackKind::ScreenSpace;
            settings.DebugView = GlobalIlluminationDebugView::Coverage;
            settings.bDirectOnly = true;
            settings.RenderScale = 0.75f;
            settings.MaxBounces = 3;
            settings.AtrousIterations = 5;
            settings.ScreenSpaceRayCount = 7;
            settings.ScreenSpaceStepCount = 91;
            settings.ScreenSpaceHiZStartMip = 3;
            settings.ScreenSpaceBinaryRefinementSteps = 6;
            settings.ScreenSpaceMaximumRayDistance = 7.5f;
            settings.ScreenSpaceThickness = 0.075f;
            settings.ScreenSpaceEdgeFade = 0.125f;
            settings.ScreenSpaceTemporalWeight = 0.987f;
            const Json serialized = SerializeGlobalIlluminationSettings(settings);
            Require(!serialized.contains("DebugView") &&
                    !serialized.contains("DirectOnly"),
                "GI JSON persisted diagnostic-only settings.");
            const GlobalIlluminationSettings restored =
                DeserializeGlobalIlluminationSettings(serialized);
            Require(restored.Technique == settings.Technique &&
                    restored.Denoiser == settings.Denoiser &&
                    restored.UnsupportedFallback == settings.UnsupportedFallback &&
                    restored.DebugView == GlobalIlluminationDebugView::Composite &&
                    !restored.bDirectOnly &&
                    std::abs(restored.RenderScale - settings.RenderScale) < 1e-6f &&
                    restored.MaxBounces == settings.MaxBounces &&
                    restored.AtrousIterations == settings.AtrousIterations &&
                    restored.ScreenSpaceRayCount == settings.ScreenSpaceRayCount &&
                    restored.ScreenSpaceStepCount == settings.ScreenSpaceStepCount &&
                    restored.ScreenSpaceHiZStartMip == settings.ScreenSpaceHiZStartMip &&
                    restored.ScreenSpaceBinaryRefinementSteps ==
                        settings.ScreenSpaceBinaryRefinementSteps &&
                    std::abs(restored.ScreenSpaceMaximumRayDistance -
                        settings.ScreenSpaceMaximumRayDistance) < 1e-6f &&
                    std::abs(restored.ScreenSpaceThickness -
                        settings.ScreenSpaceThickness) < 1e-6f &&
                    std::abs(restored.ScreenSpaceEdgeFade -
                        settings.ScreenSpaceEdgeFade) < 1e-6f &&
                    std::abs(restored.ScreenSpaceTemporalWeight -
                        settings.ScreenSpaceTemporalWeight) < 1e-6f,
                "GI settings JSON round-trip changed a persisted field.");

            Json legacySerialized = serialized;
            legacySerialized["DebugView"] =
                static_cast<uint32_t>(GlobalIlluminationDebugView::Coverage);
            legacySerialized["DirectOnly"] = true;
            const GlobalIlluminationSettings legacyRestored =
                DeserializeGlobalIlluminationSettings(legacySerialized);
            Require(legacyRestored.DebugView ==
                        GlobalIlluminationDebugView::Coverage &&
                    legacyRestored.bDirectOnly,
                "Legacy GI JSON no longer imports diagnostic settings.");

            GlobalIlluminationSettings customDefaults;
            customDefaults.Technique =
                GlobalIlluminationTechniqueKind::HardwareRayTracing;
            customDefaults.Denoiser = GlobalIlluminationDenoiserKind::Nrd;
            customDefaults.UnsupportedFallback =
                GlobalIlluminationFallbackKind::ScreenSpace;
            const GlobalIlluminationSettings missingFields =
                DeserializeGlobalIlluminationSettings(Json::object(), customDefaults);
            const GlobalIlluminationSettings unknownEnums =
                DeserializeGlobalIlluminationSettings(
                    Json{
                        { "Technique", "Unknown" },
                        { "Denoiser", "Unknown" },
                        { "UnsupportedFallback", "Unknown" },
                    },
                    customDefaults);
            const GlobalIlluminationSettings nrdRoundTrip =
                DeserializeGlobalIlluminationSettings(
                    SerializeGlobalIlluminationSettings(customDefaults));
            for (const GlobalIlluminationSettings& fallback : {
                missingFields,
                unknownEnums,
                nrdRoundTrip,
            })
            {
                Require(fallback.Technique == customDefaults.Technique &&
                        fallback.Denoiser == customDefaults.Denoiser &&
                        fallback.UnsupportedFallback ==
                            customDefaults.UnsupportedFallback,
                    "GI settings JSON ignored enum defaults or collapsed a known value.");
            }
        }

        void TestGlobalIlluminationCompositionPolicy()
        {
            Require(SelectGlobalIlluminationCompositionSource(
                        GlobalIlluminationCompositionMode::Replace,
                        1.0f,
                        0.1f,
                        1.0f,
                        1.0f) ==
                    GlobalIlluminationCompositionSource::Primary,
                "Replace composition did not preserve the primary GI owner.");
            Require(SelectGlobalIlluminationCompositionSource(
                        GlobalIlluminationCompositionMode::FillInvalidByCoverage,
                        0.0f,
                        1.0f,
                        1.0f,
                        0.1f) ==
                    GlobalIlluminationCompositionSource::Secondary,
                "Coverage fill did not select the fallback for an invalid primary.");
            Require(SelectGlobalIlluminationCompositionSource(
                        GlobalIlluminationCompositionMode::PreferHigherConfidence,
                        1.0f,
                        0.25f,
                        1.0f,
                        0.75f) ==
                    GlobalIlluminationCompositionSource::Secondary,
                "Confidence composition did not select the higher-confidence owner.");
            Require(SelectGlobalIlluminationCompositionSource(
                        GlobalIlluminationCompositionMode::FillInvalidByCoverage,
                        0.0f,
                        0.0f,
                        0.0f,
                        0.0f) ==
                    GlobalIlluminationCompositionSource::None,
                "Composition invented GI when both coverage masks were invalid.");
        }
    }

    void HardwareRayTracingContract()
    {
        TestCapabilityAndFallbackPolicy();
        TestHistoryInvalidation();
        TestGeometryCacheAndSupportPolicy();
        TestStableInstanceMapping();
        TestEmissiveTriangleDistribution();
        TestGlobalIlluminationTechniqueRegistry();
        TestGlobalIlluminationDiffuseSignal();
        TestGlobalIlluminationPixelMapping();
        TestGlobalIlluminationSpatialCompatibility();
        TestDepthPyramidReduction();
        TestGlobalIlluminationSettingsJson();
        TestGlobalIlluminationCompositionPolicy();
    }
}
