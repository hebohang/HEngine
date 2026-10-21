#pragma once

#include "Runtime/Core/Timestep.h"
#include "Runtime/Graphics/VFX/EffectTransformBatch.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>
#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace HEngine
{
    using VFXHandle = int32_t;

    enum class VFXMetricStatus : uint8_t
    {
        Unavailable,
        Exact,
        Estimated
    };

    enum class VFXBudgetCategory : uint8_t
    {
        GameplayCritical,
        Combat,
        Ambient,
        UI,
        Count
    };

    enum class VFXQualityTier : uint8_t
    {
        Low,
        Medium,
        High,
        Count
    };

    struct VFXRollingMetric
    {
        float Current = 0.0f;
        float Peak = 0.0f;
        float P50 = 0.0f;
        float P95 = 0.0f;
        float P99 = 0.0f;
        uint32_t SampleCount = 0;
        VFXMetricStatus Status = VFXMetricStatus::Unavailable;
    };

    struct VFXBudget
    {
        uint32_t MaxActiveEffects = 0;
        uint32_t MaxCpuInstances = 0;
        uint32_t MaxGpuParticles = 0;
        uint32_t MaxNewParticlesPerFrame = 0;
        uint32_t MaxTrailSegments = 0;
        uint32_t MaxTransparentVertices = 0;
        uint32_t MaxDrawCalls = 0;
        uint32_t ParticleBufferCapacity = 0;
        uint32_t MaxEffectCache = 0;
        uint32_t MaxPackageCache = 0;
    };

    struct VFXPlayParams
    {
        glm::vec3 Position = glm::vec3(0.0f);
        glm::vec3 Forward = glm::vec3(0.0f, 0.0f, -1.0f);
        float Scale = 1.0f;
        float Speed = 1.0f;
        VFXBudgetCategory Category = VFXBudgetCategory::Combat;
        // Applied before the first simulation update. Authored seed is used
        // unless the caller explicitly selects a reproducible preview seed.
        bool bOverrideRandomSeed = false;
        int32_t RandomSeed = 0;
    };

    struct VFXStats
    {
        static constexpr uint32_t RollingWindowSamples = 600;

        VFXRollingMetric CpuUpdateMs;
        VFXRollingMetric CpuDrawSubmitMs;
        VFXRollingMetric EffectLoadMs;
        VFXRollingMetric FirstPlayMs;
        VFXRollingMetric BatchTransformMs;
        VFXRollingMetric GpuSimulationMs;
        VFXRollingMetric GpuRenderMs;
        VFXRollingMetric GpuBackgroundCopyMs;
        VFXRollingMetric GpuTransparentRenderMs;

        VFXRollingMetric ActiveEffects;
        VFXRollingMetric ActiveCpuInstances;
        VFXRollingMetric ActiveGpuParticles;
        VFXRollingMetric EffectsCreatedThisFrame;
        VFXRollingMetric EffectsDestroyedThisFrame;
        VFXRollingMetric CpuInstancesCreatedThisFrame;
        VFXRollingMetric CpuInstancesDestroyedThisFrame;
        VFXRollingMetric CachedEffects;
        VFXRollingMetric CachedPackages;
        VFXRollingMetric DrawCalls;
        VFXRollingMetric VertexCount;
        VFXRollingMetric ParticleBufferUsed;
        VFXRollingMetric ParticleBufferCapacity;
        VFXRollingMetric BudgetExceededCount;
        VFXRollingMetric BudgetRejectedCount;

        std::string HighestCostEffectPath;
        uint32_t HighestCostEffectEstimatedInstances = 0;
        VFXMetricStatus HighestCostEffectStatus = VFXMetricStatus::Unavailable;

        VFXQualityTier QualityTier = VFXQualityTier::High;
        bool bDetailedStatsEnabled = false;
        bool bGpuParticlesCapable = false;
        bool bGpuParticlesEnabled = false;
        uint64_t RecordedDrawCount = 0;
        uint64_t RecordedDispatchCount = 0;
        uint64_t RecordingSerial = 0;
        uint64_t TotalRecordedDrawCount = 0;
        uint64_t TotalRecordedDispatchCount = 0;
        uint64_t LastGpuTimingSubmission = 0;
        uint64_t LastGpuSimulationSubmission = 0;
        uint64_t LastGpuRenderSubmission = 0;
        uint64_t UpdateSerial = 0;
        uint64_t GpuEmitterSlotFailures = 0;
        uint64_t GpuParticleBlockFailures = 0;
        uint64_t GpuTrailBlockFailures = 0;
        uint64_t GpuAllocatedEmitters = 0;
        uint64_t GpuReservedParticles = 0;
        uint64_t GpuReservedTrails = 0;
        // Actual held LLGI buffers, counted by identity; GetSize logical bytes.
        bool bBufferPoolStatsAvailable = false;
        uint64_t StorageBufferCount = 0;
        uint64_t StorageLogicalBytes = 0;
        uint64_t UniformBufferCount = 0;
        uint64_t UniformLogicalBytes = 0;
        uint64_t UniformSnapshotCount = 0;
    };

    enum class VFXRenderBackendStatus : uint8_t
    {
        Unconfigured,
        Ready,
        FrameActive
    };

    struct EffekseerRuntimeDesc
    {
        nvrhi::IDevice* Device = nullptr;
        nvrhi::Format ColorFormat = nvrhi::Format::UNKNOWN;
        nvrhi::Format DepthFormat = nvrhi::Format::UNKNOWN;
        uint32_t FramesInFlight = 0;
        bool bReversedDepth = false;
    };

    struct EffekseerRenderDesc
    {
        nvrhi::ICommandList* CommandList = nullptr;
        nvrhi::IFramebuffer* Framebuffer = nullptr;
        nvrhi::ITexture* DepthSampleTexture = nullptr;
        nvrhi::ITexture* BackgroundTexture = nullptr;

        float ZNear = 0.1f;
        float ZFar = 1000.0f;
        glm::vec3 CameraPosition = glm::vec3(0.0f);
        glm::vec3 LightDirection = glm::vec3(0.0f, 1.0f, 0.0f);
        glm::vec3 LightColor = glm::vec3(1.0f);
        glm::vec3 LightAmbientColor = glm::vec3(0.25f);
    };

    struct VFXRenderRequirements
    {
        bool bHasActiveEffects = false;
        bool bRequiresDepthSnapshot = false;
        bool bRequiresBackgroundSnapshot = false;
    };

    class EffekseerRuntime
    {
    public:
        // Final NVRHI path. The device owner must wait for GPU idle before
        // Shutdown releases renderer-owned resources.
        static void Initialize(const EffekseerRuntimeDesc& desc);
        static void Init();
        static void Shutdown();
        // Direct hosts must bracket all view recordings with one host-frame
        // scope. Publish every issued submission (including present failure),
        // or abort after the host command list is discarded. Shutdown requires
        // GPU idle; these hooks do not wait for the GPU.
        static void BeginHostFrame(uint32_t slot, uint64_t completed, nvrhi::ICommandList* commandList);
        static void ComputeParticles(nvrhi::ICommandList* commandList);
        static void NotifyHostFrameSubmitted(uint32_t slot, uint64_t submission) noexcept;
        static void AbortHostFrame(uint32_t slot) noexcept;

        static bool IsInitialized();
		[[nodiscard]] static VFXRenderBackendStatus GetRenderBackendStatus();
		[[nodiscard]] static bool HasActiveEffects();
        [[nodiscard]] static VFXRenderRequirements GetRenderRequirements();

        static bool LoadEffect(const std::string& relativePath);
        // Publish a fully loaded replacement for future plays. Existing handles
        // retain their EffectRef and continue with the previous resource version.
        static bool ReloadEffect(const std::string& relativePath);
        static VFXHandle PlayEffect(const std::string& relativePath, const glm::vec3& position);
        static VFXHandle PlayEffect(const std::string& relativePath, const VFXPlayParams& params);
        static VFXHandle PlayEffectAdvanced(const std::string& relativePath,
                                            const glm::vec3& position,
                                            const glm::vec3& forward,
                                            float scale,
                                            float speed);
        static bool SetEffectTransform(VFXHandle handle,
                                       const glm::vec3& position,
                                       const glm::vec3& forward);
        static void SetEffectTransforms(std::span<const EffectTransformCommand> commands,
                                        std::vector<uint8_t>& outResults);
        static bool IsEffectPlaying(VFXHandle handle);
        [[nodiscard]] static glm::vec3 GetEffectPosition(VFXHandle handle);
        [[nodiscard]] static nlohmann::json GetEffectStatus(VFXHandle handle);
        static bool SetEffectPaused(VFXHandle handle, bool bPaused);
        static bool SetEffectColor(VFXHandle handle, const glm::vec4& color);
        static bool SetEffectSpeed(VFXHandle handle, float speed);
        static bool SetEffectParameter(VFXHandle handle, const std::string& name, float value);
        static bool SetEffectDynamicInput(VFXHandle handle, uint32_t index, float value);
        static void StopEffect(VFXHandle handle);
        static void StopAllEffects();

        // Resource-pool traversal is opt-in for diagnostics, never per-frame metrics.
        [[nodiscard]] static VFXStats GetStats(bool includeResourcePools = false);
        [[nodiscard]] static nlohmann::json Describe();
        // Shared diagnostic projection; includes opt-in resource-pool traversal.
        [[nodiscard]] static nlohmann::json DescribeStats();
        [[nodiscard]] static VFXBudget GetBudget(VFXBudgetCategory category, VFXQualityTier qualityTier);
        [[nodiscard]] static VFXQualityTier GetQualityTier();
        static void SetQualityTier(VFXQualityTier qualityTier);

        [[nodiscard]] static bool IsGpuParticleCapable();
        [[nodiscard]] static bool IsGpuParticlesEnabled();
        static bool SetGpuParticlesEnabled(bool bEnabled);

        // Bounded development regression checks. Package ownership stays in
        // the active project; the zero-argument wrapper loads that project's
        // Config/EffekseerRegression.json manifest.
        static bool ValidateEffectPackages(std::span<const std::string> relativePaths);
        static bool ValidateAllEffectPackages();

        static void Update(Timestep ts);
        static void SetCamera(const glm::mat4& view, const glm::mat4& projection);
        static void Render(const EffekseerRenderDesc& desc);

        // Legacy immediate renderer entry point retained only until its caller
        // is moved into the RenderGraph VFX pass. It never selects or creates a
        // graphics backend and reports an explicit skipped render if invoked.
        static void Render(float zNear, float zFar, const glm::vec3& cameraPosition);

    private:
        static bool LoadEffectInternal(const std::string& relativePath, bool bBypassCacheBudget);
    };
}
