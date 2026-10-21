#include "Hepch.h"

#include "Plugins/OfflineReferenceRenderer/Source/OfflineReferenceRendererSession.h"
#include "Plugins/OfflineReferenceRenderer/Source/OfflineReferenceRendererSettingsJson.h"
#include "Plugins/OfflineReferenceRenderer/Source/ProgressiveSchedule.h"
#include "Plugins/OfflineReferenceRenderer/Source/ProgressiveDenoiser.h"

#include "Runtime/Graphics/RHI/ShaderManager.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <nvrhi/utils.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace HEngine
{
    namespace
    {
        constexpr uint64_t FnvOffset = 14695981039346656037ull;
        constexpr uint64_t FnvPrime = 1099511628211ull;
        constexpr std::string_view ReferenceProviderId =
            "he.viewport.path-tracing";
        constexpr uint32_t MaximumOfflineDimension = 16384u;
        constexpr uint32_t MaximumOfflineSamplesPerFrame = 1024u;
        constexpr std::array<OfflineViewportAov, 10> ReferenceAovs = {
            OfflineViewportAov::DenoisedBeauty,
            OfflineViewportAov::Beauty,
            OfflineViewportAov::DirectDiffuse,
            OfflineViewportAov::IndirectDiffuse,
            OfflineViewportAov::Albedo,
            OfflineViewportAov::WorldNormal,
            OfflineViewportAov::ViewDepth,
            OfflineViewportAov::MaterialId,
            OfflineViewportAov::InstanceId,
            OfflineViewportAov::SampleCount,
        };

        void HashBytes(uint64_t& hash, const void* data, size_t size) noexcept
        {
            const auto* bytes = static_cast<const uint8_t*>(data);
            for (size_t index = 0; index < size; ++index)
            {
                hash ^= bytes[index];
                hash *= FnvPrime;
            }
        }

        template<typename Value>
        void HashValue(uint64_t& hash, const Value& value) noexcept
        {
            HashBytes(hash, &value, sizeof(value));
        }

        void HashMatrix(uint64_t& hash, const glm::mat4& matrix) noexcept
        {
            for (glm::length_t column = 0; column < 4; ++column)
            {
                for (glm::length_t row = 0; row < 4; ++row)
                    HashValue(hash, matrix[column][row]);
            }
        }

        [[nodiscard]] uint64_t ComputeCameraFingerprint(
            const RenderCamera& camera) noexcept
        {
            uint64_t hash = FnvOffset;
            HashMatrix(hash, camera.View);
            HashMatrix(hash, camera.Projection);
            HashMatrix(hash, camera.ViewProjection);
            HashValue(hash, camera.Position);
            HashValue(hash, camera.NearPlane);
            HashValue(hash, camera.FarPlane);
            return hash == 0 ? 1 : hash;
        }

        [[nodiscard]] bool IsFinite(const glm::vec3& value) noexcept
        {
            return std::isfinite(value.x) && std::isfinite(value.y) &&
                std::isfinite(value.z);
        }

        void ValidateOfflineSettings(
            const OfflineViewportRendererSettings& settings)
        {
            if (!std::isfinite(settings.TargetFramesPerSecond) || settings.TargetFramesPerSecond < 15.f ||
                settings.TargetFramesPerSecond > 120.f || !std::isfinite(settings.WorkBudgetMilliseconds) ||
                settings.WorkBudgetMilliseconds < .25f || settings.WorkBudgetMilliseconds > 16.f ||
                !std::isfinite(settings.InteractiveScale) || settings.InteractiveScale < .05f || settings.InteractiveScale > 1.f ||
                !std::isfinite(settings.SettleMilliseconds) || settings.SettleMilliseconds < 100.f || settings.SettleMilliseconds > 2000.f)
                throw std::invalid_argument("Offline viewport budget or navigation settings are outside the supported range");
            if (settings.TargetSamples == 0)
                throw std::invalid_argument(
                    "Offline reference target samples must be non-zero");
            if (settings.SamplesPerFrame == 0 ||
                settings.SamplesPerFrame > MaximumOfflineSamplesPerFrame)
            {
                throw std::invalid_argument(
                    "Offline reference samples per frame must be in [1, 1024]");
            }
            if (settings.MaxBounces > 16)
                throw std::invalid_argument(
                    "Offline reference maximum bounce count exceeds 16");
            if (!std::isfinite(settings.MaximumRayDistance) ||
                settings.MaximumRayDistance <= 0.0f)
            {
                throw std::invalid_argument(
                    "Offline reference maximum ray distance must be finite and positive");
            }
            if (!std::isfinite(settings.RenderScale) ||
                settings.RenderScale <= 0.0f || settings.RenderScale > 8.0f)
            {
                throw std::invalid_argument(
                    "Offline reference render scale must be in (0, 8]");
            }
            if (settings.FixedWidth > MaximumOfflineDimension ||
                settings.FixedHeight > MaximumOfflineDimension)
            {
                throw std::invalid_argument(
                    "Offline reference fixed dimensions exceed 16384");
            }
            if ((settings.FixedWidth == 0) != (settings.FixedHeight == 0) &&
                !settings.bLockAspectRatio)
            {
                throw std::invalid_argument(
                    "A single offline fixed dimension requires aspect-ratio locking");
            }
            if (!IsFinite(settings.EnvironmentRadiance))
                throw std::invalid_argument(
                    "Offline reference environment radiance must be finite");
            if (std::ranges::find(ReferenceAovs, settings.PreviewAov) ==
                ReferenceAovs.end())
            {
                throw std::invalid_argument(
                    "Offline reference preview AOV is invalid");
            }
            switch (settings.SeedMode)
            {
            case OfflineViewportSeedMode::Fixed:
            case OfflineViewportSeedMode::Random:
                break;
            default:
                throw std::invalid_argument(
                    "Offline reference seed mode is invalid");
            }
            switch (settings.OutputFormat)
            {
            case OfflineViewportOutputFormat::Pfm:
            case OfflineViewportOutputFormat::ToneMappedPng:
            case OfflineViewportOutputFormat::Raw:
                break;
            default:
                throw std::invalid_argument(
                    "Offline reference output format is invalid");
            }
        }

        [[nodiscard]] bool HaveSameIntegralSettings(
            const OfflineViewportRendererSettings& left,
            const OfflineViewportRendererSettings& right) noexcept
        {
            return left.MaxBounces == right.MaxBounces &&
                left.MaximumRayDistance == right.MaximumRayDistance &&
                left.RenderScale == right.RenderScale &&
                left.FixedWidth == right.FixedWidth &&
                left.FixedHeight == right.FixedHeight &&
                left.bLockAspectRatio == right.bLockAspectRatio &&
                left.SeedMode == right.SeedMode && left.Seed == right.Seed &&
                left.EnvironmentRadiance == right.EnvironmentRadiance &&
                left.bUseSceneEnvironment == right.bUseSceneEnvironment &&
                left.bDirectOnly == right.bDirectOnly;
        }

        [[nodiscard]] uint32_t GenerateRandomSeed() noexcept
        {
            uint64_t value = static_cast<uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count());
            try
            {
                std::random_device source;
                value ^= static_cast<uint64_t>(source()) << 32u;
                value ^= static_cast<uint64_t>(source());
            }
            catch (...)
            {
            }
            value ^= value >> 30u;
            value *= 0xbf58476d1ce4e5b9ull;
            value ^= value >> 27u;
            value *= 0x94d049bb133111ebull;
            value ^= value >> 31u;
            const uint32_t seed = static_cast<uint32_t>(value);
            return seed == 0 ? 0x48454e47u : seed;
        }

        [[nodiscard]] std::pair<uint32_t, uint32_t> ResolveOfflineDimensions(
            uint32_t viewportWidth,
            uint32_t viewportHeight,
            const OfflineViewportRendererSettings& settings)
        {
            if (viewportWidth == 0 || viewportHeight == 0)
                throw std::invalid_argument(
                    "Offline reference viewport dimensions must be non-zero");
            uint32_t width = settings.FixedWidth;
            uint32_t height = settings.FixedHeight;
            const double aspect = static_cast<double>(viewportWidth) /
                static_cast<double>(viewportHeight);
            if (width > 0 && height == 0)
            {
                height = static_cast<uint32_t>(std::clamp(
                    std::llround(static_cast<double>(width) / aspect),
                    1ll,
                    static_cast<long long>(MaximumOfflineDimension)));
            }
            else if (height > 0 && width == 0)
            {
                width = static_cast<uint32_t>(std::clamp(
                    std::llround(static_cast<double>(height) * aspect),
                    1ll,
                    static_cast<long long>(MaximumOfflineDimension)));
            }
            else if (width == 0 && height == 0)
            {
                width = static_cast<uint32_t>(std::clamp(
                    std::llround(
                        static_cast<double>(viewportWidth) * settings.RenderScale),
                    1ll,
                    static_cast<long long>(MaximumOfflineDimension)));
                height = static_cast<uint32_t>(std::clamp(
                    std::llround(
                        static_cast<double>(viewportHeight) * settings.RenderScale),
                    1ll,
                    static_cast<long long>(MaximumOfflineDimension)));
            }
            return { width, height };
        }

        [[nodiscard]] std::string_view GetAovFileStem(
            OfflineViewportAov aov)
        {
            switch (aov)
            {
            case OfflineViewportAov::Beauty: return "beauty";
            case OfflineViewportAov::DirectDiffuse: return "direct-diffuse";
            case OfflineViewportAov::IndirectDiffuse: return "indirect-diffuse";
            case OfflineViewportAov::Albedo: return "albedo";
            case OfflineViewportAov::WorldNormal: return "world-normal";
            case OfflineViewportAov::ViewDepth: return "view-depth";
            case OfflineViewportAov::MaterialId: return "material-id";
            case OfflineViewportAov::InstanceId: return "instance-id";
            case OfflineViewportAov::SampleCount: return "sample-count";
            default:
                throw std::out_of_range("Offline reference AOV is invalid");
            }
        }

        struct ReferenceExportRequest
        {
            uint64_t Generation = 0;
            OfflineViewportAov Aov = OfflineViewportAov::Beauty;
            OfflineViewportOutputFormat Format =
                OfflineViewportOutputFormat::Pfm;
            TextureReadbackHandle Handle;
        };

        struct ReferenceExportPassData
        {
            std::vector<TextureHandle> Sources;
            std::vector<TextureReadbackHandle> Handles;
        };

        [[nodiscard]] float ResolveMaximumRayDistance(
            const ReferencePathTracerFrameParameters& frame) noexcept
        {
            return std::isfinite(frame.MaximumRayDistance) &&
                    frame.MaximumRayDistance > 0.0f
                ? frame.MaximumRayDistance
                : std::max(frame.Camera.FarPlane, 1.0f);
        }

        struct alignas(16) ReferencePathTracerConstants
        {
            glm::mat4 InverseViewProjection{ 1.0f };
            glm::vec4 CameraPositionAndMaxDistance{ 0.0f, 0.0f, 0.0f, 1000.0f };
            glm::uvec4 DimensionsSampleBounces{ 0u };
            glm::uvec4 SamplingSettings{ 0u };
            glm::vec4 EnvironmentRadiance{ 0.0f };
        };

        static_assert(sizeof(ReferencePathTracerConstants) == 128);

        struct ReferenceTracePassData
        {
            BufferHandle AnalyticLights;
            std::vector<TextureHandle> MaterialTextures;
            AccelerationStructureHandle SceneTlas;
            BufferHandle Vertices;
            BufferHandle Indices;
            BufferHandle Geometries;
            BufferHandle Materials;
            BufferHandle Instances;
            BufferHandle EmissiveTriangles;
            TextureHandle Environment;
            ReferencePathTracerOutputs Outputs;
        };

        struct alignas(16) OfflineReferencePreviewConstants
        {
            glm::uvec2 Dimensions{ 0u };
            uint32_t Aov = 0;
            uint32_t TargetSamples = 1;
        };

        static_assert(sizeof(OfflineReferencePreviewConstants) == 16);

        struct OfflineReferencePreviewPassData
        {
            TextureHandle Source;
            TextureHandle Output;
        };

        [[nodiscard]] std::string ReadShaderContentHash(
            const std::filesystem::path& path)
        {
            std::ifstream stream(path);
            if (!stream)
            {
                throw std::runtime_error(
                    "Failed to open hardware RT shader cache-key sidecar: " +
                    Utils::PathToUtf8(path));
            }
            std::string line;
            while (std::getline(stream, line))
            {
                constexpr std::string_view Prefix = "key=";
                if (line.starts_with(Prefix) && line.size() > Prefix.size())
                    return line.substr(Prefix.size());
            }
            throw std::runtime_error(
                "Hardware RT shader cache-key sidecar has no key: " +
                Utils::PathToUtf8(path));
        }

        [[nodiscard]] RenderGraphTextureDesc ToGraphDesc(
            const nvrhi::TextureHandle& texture)
        {
            const nvrhi::TextureDesc& desc = texture->getDesc();
            return {
                .DebugName = desc.debugName,
                .Width = desc.width,
                .Height = desc.height,
                .Depth = desc.depth,
                .MipLevels = desc.mipLevels,
                .ArrayLayers = desc.arraySize,
                .SampleCount = desc.sampleCount,
                .Format = desc.format,
                .Dimension = desc.dimension,
            };
        }

        [[nodiscard]] bool IsFinite(const glm::mat4& matrix) noexcept
        {
            for (glm::length_t column = 0; column < 4; ++column)
            {
                for (glm::length_t row = 0; row < 4; ++row)
                {
                    if (!std::isfinite(matrix[column][row]))
                        return false;
                }
            }
            return true;
        }
    }

    uint64_t ComputeReferencePathTracerAccumulationFingerprint(
        const ReferencePathTracerFrameParameters& frame,
        const GlobalIlluminationSettings& settings) noexcept
    {
        uint64_t hash = FnvOffset;
        HashMatrix(hash, frame.Camera.View);
        HashMatrix(hash, frame.Camera.Projection);
        HashMatrix(hash, frame.Camera.ViewProjection);
        HashValue(hash, frame.Camera.Position);
        HashValue(hash, frame.Camera.NearPlane);
        HashValue(hash, frame.Camera.FarPlane);
        HashValue(hash, frame.SceneStateFingerprint);
        HashValue(hash, frame.FixedSeed);
        const float maximumRayDistance = ResolveMaximumRayDistance(frame);
        HashValue(hash, maximumRayDistance);
        const glm::vec3 environmentRadiance = glm::max(
            frame.EnvironmentRadiance,
            glm::vec3(0.0f));
        HashValue(hash, environmentRadiance);
        HashValue(hash, frame.bSampleSceneEnvironment);
        HashValue(hash, settings.SamplesPerPixel);
        HashValue(hash, settings.MaxBounces);
        HashValue(hash, settings.bDirectOnly);
        return hash == 0 ? 1 : hash;
    }

    uint32_t Detail::ResolveReferenceOfflineDispatchCount(
        const OfflineViewportRendererSettings& settings,
        OfflineViewportSessionState state,
        uint64_t accumulatedSamples,
        bool bRestartRequired,
        bool bFrozen) noexcept
    {
        if (state != OfflineViewportSessionState::Rendering ||
            bRestartRequired || bFrozen ||
            accumulatedSamples >= settings.TargetSamples)
        {
            return 0u;
        }
        // A dispatch is a budgeted row band. Legacy batch preferences cannot
        // bypass the scheduler, including in deterministic Reference mode.
        return 1u;
    }

    OfflineViewportSessionState Detail::ResolveReferenceOfflineControlState(
        OfflineViewportSessionState state,
        ReferenceOfflineControlCommand command,
        uint64_t accumulatedSamples,
        uint64_t targetSamples,
        bool bRestartRequired) noexcept
    {
        switch (command)
        {
        case ReferenceOfflineControlCommand::Pause:
            return state == OfflineViewportSessionState::Rendering ||
                    state == OfflineViewportSessionState::Idle
                ? OfflineViewportSessionState::Paused
                : state;
        case ReferenceOfflineControlCommand::Resume:
            if (bRestartRequired ||
                state == OfflineViewportSessionState::Cancelled ||
                state == OfflineViewportSessionState::Failed)
            {
                return state;
            }
            if (accumulatedSamples >= targetSamples)
                return OfflineViewportSessionState::Complete;
            return state == OfflineViewportSessionState::Paused ||
                    state == OfflineViewportSessionState::Idle
                ? OfflineViewportSessionState::Rendering
                : state;
        case ReferenceOfflineControlCommand::Restart:
            return OfflineViewportSessionState::Rendering;
        case ReferenceOfflineControlCommand::Cancel:
            return OfflineViewportSessionState::Cancelled;
        case ReferenceOfflineControlCommand::SamplesCompleted:
            return accumulatedSamples >= targetSamples
                ? OfflineViewportSessionState::Complete
                : state;
        default:
            return state;
        }
    }

    void Detail::ValidateReferenceOfflineExportRequest(
        OfflineViewportAov aov,
        OfflineViewportOutputFormat format)
    {
        if (std::ranges::find(ReferenceAovs, aov) == ReferenceAovs.end())
            throw std::out_of_range("Offline reference export AOV is invalid");
        if (format != OfflineViewportOutputFormat::Pfm &&
            format != OfflineViewportOutputFormat::Raw &&
            format != OfflineViewportOutputFormat::ToneMappedPng)
        {
            throw std::invalid_argument(
                "Offline reference export format is invalid");
        }
        if (format == OfflineViewportOutputFormat::ToneMappedPng &&
            aov != OfflineViewportAov::Beauty && aov != OfflineViewportAov::DenoisedBeauty)
        {
            throw std::invalid_argument(
                "Tone-mapped PNG export currently requires the Beauty AOV");
        }
        const bool bIntegerAov = aov == OfflineViewportAov::MaterialId ||
            aov == OfflineViewportAov::InstanceId ||
            aov == OfflineViewportAov::SampleCount;
        if (format == OfflineViewportOutputFormat::Pfm && bIntegerAov)
            throw std::invalid_argument(
                "Integer offline reference AOVs require Raw export");
    }

    struct ReferencePathTracer::Impl
    {
        struct RetiredBindingSet
        {
            uint64_t Submission = 0;
            nvrhi::BindingSetHandle BindingSet;
        };

        struct RetiredOutputs
        {
            uint64_t Submission = 0;
            ReferencePathTracerNativeOutputs Outputs;
            nvrhi::BindingSetHandle BindingSet;
        };

        explicit Impl(const ReferencePathTracerDesc& desc)
            : Device(desc.Device)
            , OwnedShaderManager(desc.SharedShaderManager
                ? nullptr
                : CreateScope<ShaderManager>(desc.Device))
            , ShaderManagerInstance(desc.SharedShaderManager
                ? *desc.SharedShaderManager
                : *OwnedShaderManager)
            , Scene(desc.Scene)
            , ShaderArtifactCacheRoot(desc.ShaderArtifactCacheRoot)
            , OfflineSettingsPath(desc.OfflineSettingsPath)
            , Backend(desc.Backend)
            , BinaryFormat(GetShaderBinaryFormat(desc.Backend))
            , FramesInFlight(desc.FramesInFlight)
        {
            if (!Device)
                throw std::invalid_argument("ReferencePathTracer requires a valid NVRHI device");
            if (!Scene)
                throw std::invalid_argument("ReferencePathTracer requires a RayTracingScene");
            if (ShaderArtifactCacheRoot.empty())
                throw std::invalid_argument("ReferencePathTracer requires a shader artifact cache root");
            if (FramesInFlight == 0)
                throw std::invalid_argument("ReferencePathTracer requires at least one frame in flight");
            ReadbackQueue = CreateScope<TextureReadbackQueue>(Device);
            TraceTimer = Device->createTimerQuery();
            bOfflineControlEnabled = desc.bOfflineViewportSession;
            ResolvedSeed = OfflineSettings.Seed;
            CreatePipeline();
            if (bOfflineControlEnabled)
                SetOfflineState(OfflineViewportSessionState::Rendering);
        }

        using Clock = std::chrono::steady_clock;

        void SetOfflineState(OfflineViewportSessionState state) noexcept
        {
            // Graph construction omits tracing in non-rendering states. Any
            // transition (including raising a completed target) must rebuild it.
            if (state != OfflineState) bGraphRebuildRequested = true;
            const Clock::time_point now = Clock::now();
            if (OfflineState == OfflineViewportSessionState::Rendering)
            {
                ElapsedRenderingSeconds +=
                    std::chrono::duration<double>(now - RenderingStartedAt)
                        .count();
            }
            OfflineState = state;
            if (OfflineState == OfflineViewportSessionState::Rendering)
                RenderingStartedAt = now;
        }

        void EnableOfflineControl()
        {
            if (bOfflineControlEnabled)
                return;
            bOfflineControlEnabled = true;
            ElapsedRenderingSeconds = 0.0;
            OfflineError.clear();
            SetOfflineState(AccumulatedSamples >= OfflineSettings.TargetSamples
                ? OfflineViewportSessionState::Complete
                : OfflineViewportSessionState::Rendering);
        }

        [[nodiscard]] double GetElapsedSeconds() const noexcept
        {
            if (OfflineState != OfflineViewportSessionState::Rendering)
                return ElapsedRenderingSeconds;
            return ElapsedRenderingSeconds + std::chrono::duration<double>(
                Clock::now() - RenderingStartedAt).count();
        }

        void ResetOfflineProgressClock() noexcept
        {
            ElapsedRenderingSeconds = 0.0;
            if (OfflineState == OfflineViewportSessionState::Rendering)
                RenderingStartedAt = Clock::now();
        }

        void MarkOfflineFailed(std::string error)
        {
            OfflineError = std::move(error);
            SetOfflineState(OfflineViewportSessionState::Failed);
            Stats.Status = "Offline reference failed";
        }

        void ResetAccumulation(
            GlobalIlluminationHistoryResetReason reason)
        {
            HistoryTracker.Reset(reason);
            AccumulatedSamples = 0;
            Schedule.Reset(NativeOutputs.Height);
            bClearAccumulation = true;
            PendingDenoisedUpload.reset();
            RecordedSamples = 0;
            ++Stats.HistoryResetCount;
            Stats.LastResetReasons = reason;
            bGraphRebuildRequested = true;
        }

        [[nodiscard]] uint32_t ResolveDispatchCount() const noexcept
        {
            if (!bOfflineControlEnabled)
                return 1u;
            return Detail::ResolveReferenceOfflineDispatchCount(
                OfflineSettings,
                OfflineState,
                AccumulatedSamples,
                bRestartRequired,
                false);
        }

        [[nodiscard]] nvrhi::TextureHandle GetNativeAovTexture(
            OfflineViewportAov aov) const
        {
            switch (aov)
            {
            case OfflineViewportAov::DenoisedBeauty:
                return DenoisedGeneration == Schedule.Generation() ? NativeOutputs.DenoisedRadiance : nullptr;
            case OfflineViewportAov::Beauty:
                return NativeOutputs.AccumulatedRadiance;
            case OfflineViewportAov::DirectDiffuse:
                return NativeOutputs.DirectDiffuse;
            case OfflineViewportAov::IndirectDiffuse:
                return NativeOutputs.IndirectDiffuse;
            case OfflineViewportAov::Albedo:
                return NativeOutputs.Albedo;
            case OfflineViewportAov::WorldNormal:
                return NativeOutputs.WorldNormal;
            case OfflineViewportAov::ViewDepth:
                return NativeOutputs.ViewDepth;
            case OfflineViewportAov::MaterialId:
                return NativeOutputs.MaterialId;
            case OfflineViewportAov::InstanceId:
                return NativeOutputs.InstanceId;
            case OfflineViewportAov::SampleCount:
                return NativeOutputs.SampleCount;
            default:
                throw std::out_of_range("Offline reference AOV is invalid");
            }
        }

        [[nodiscard]] TextureHandle GetGraphAovTexture(
            OfflineViewportAov aov) const
        {
            switch (aov)
            {
            case OfflineViewportAov::DenoisedBeauty:
                return DenoisedGeneration == Schedule.Generation() ? GraphOutputs.DenoisedRadiance : GraphOutputs.AccumulatedRadiance;
            case OfflineViewportAov::Beauty:
                return GraphOutputs.AccumulatedRadiance;
            case OfflineViewportAov::DirectDiffuse:
                return GraphOutputs.DirectDiffuse;
            case OfflineViewportAov::IndirectDiffuse:
                return GraphOutputs.IndirectDiffuse;
            case OfflineViewportAov::Albedo:
                return GraphOutputs.Albedo;
            case OfflineViewportAov::WorldNormal:
                return GraphOutputs.WorldNormal;
            case OfflineViewportAov::ViewDepth:
                return GraphOutputs.ViewDepth;
            case OfflineViewportAov::MaterialId:
                return GraphOutputs.MaterialId;
            case OfflineViewportAov::InstanceId:
                return GraphOutputs.InstanceId;
            case OfflineViewportAov::SampleCount:
                return GraphOutputs.SampleCount;
            default:
                throw std::out_of_range("Offline reference AOV is invalid");
            }
        }

        void CancelUnsubmittedExports() noexcept
        {
            const auto cancel = [this](const ReferenceExportRequest& request)
            {
                try
                {
                    if (ReadbackQueue->IsValid(request.Handle) &&
                        ReadbackQueue->GetResult(request.Handle).Status ==
                            TextureReadbackStatus::PendingSubmission)
                    {
                        ReadbackQueue->CancelPending(request.Handle);
                    }
                }
                catch (const std::exception& exception)
                {
                    HE_CORE_ERROR(
                        "Failed to cancel offline reference export: {0}",
                        exception.what());
                }
            };
            for (const ReferenceExportRequest& request : PendingExports)
                cancel(request);
            for (const ReferenceExportRequest& request : ScheduledExports)
                cancel(request);
            PendingExports.clear();
            ScheduledExports.clear();
        }

        [[nodiscard]] nvrhi::ShaderHandle LoadPreviewVariant(
            std::string_view variant)
        {
            const std::string fileName = "OfflineReferencePreview_" +
                std::string(variant) + ".cs";
            const std::filesystem::path path = ShaderArtifactCacheRoot /
                std::string(GetShaderArtifactDirectoryName(BinaryFormat)) /
                fileName;
            return ShaderManagerInstance.LoadShaderFile(
                ShaderCacheKey{
                    .Source = "OfflineReferencePreview.hlsl",
                    .Variant = std::string(variant),
                    .Stage = nvrhi::ShaderType::Compute,
                    .EntryPoint = "CS",
                    .TargetProfile = "cs_6_0",
                    .ContentHash = ReadShaderContentHash(
                        MakeShaderCacheKeyPath(path)),
                    .Backend = Backend,
                    .BinaryFormat = BinaryFormat,
                },
                path,
                "Offline Reference Preview " + std::string(variant));
        }

        [[nodiscard]] nvrhi::ComputePipelineHandle CreatePreviewPipeline(
            nvrhi::IShader* shader)
        {
            nvrhi::ComputePipelineDesc desc;
            desc.setComputeShader(shader).addBindingLayout(PreviewBindingLayout);
            nvrhi::ComputePipelineHandle pipeline =
                Device->createComputePipeline(desc);
            if (!pipeline)
            {
                throw std::runtime_error(
                    "NVRHI failed to create an offline reference preview pipeline");
            }
            return pipeline;
        }

        void CreatePreviewPipelines()
        {
            nvrhi::BindingLayoutDesc layoutDesc;
            layoutDesc.setVisibility(nvrhi::ShaderType::Compute)
                .setRegisterSpaceAndDescriptorSet(3)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(
                    0,
                    sizeof(OfflineReferencePreviewConstants)))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(0));
            PreviewBindingLayout = Device->createBindingLayout(layoutDesc);
            if (!PreviewBindingLayout)
            {
                throw std::runtime_error(
                    "NVRHI failed to create the offline reference preview binding layout");
            }
            PreviewFloat4Shader = LoadPreviewVariant("FLOAT4");
            PreviewScalarShader = LoadPreviewVariant("SCALAR");
            PreviewUintShader = LoadPreviewVariant("UINT");
            PreviewToneMapShader = LoadPreviewVariant("TONEMAP");
            PreviewFloat4Pipeline = CreatePreviewPipeline(PreviewFloat4Shader);
            PreviewScalarPipeline = CreatePreviewPipeline(PreviewScalarShader);
            PreviewUintPipeline = CreatePreviewPipeline(PreviewUintShader);
            PreviewToneMapPipeline = CreatePreviewPipeline(PreviewToneMapShader);
        }

        [[nodiscard]] nvrhi::IComputePipeline* SelectPreviewPipeline(
            OfflineViewportAov aov,
            bool bToneMapped) const
        {
            if (bToneMapped)
                return PreviewToneMapPipeline;
            if (aov == OfflineViewportAov::ViewDepth)
                return PreviewScalarPipeline;
            if (aov == OfflineViewportAov::MaterialId ||
                aov == OfflineViewportAov::InstanceId ||
                aov == OfflineViewportAov::SampleCount)
            {
                return PreviewUintPipeline;
            }
            return PreviewFloat4Pipeline;
        }

        void RecordPreview(
            nvrhi::ICommandList* commandList,
            nvrhi::ITexture* source,
            nvrhi::ITexture* output,
            OfflineViewportAov aov,
            bool bToneMapped)
        {
            nvrhi::BindingSetDesc bindingDesc;
            bindingDesc
                .addItem(nvrhi::BindingSetItem::PushConstants(
                    0,
                    sizeof(OfflineReferencePreviewConstants)))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(0, source))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(0, output));
            nvrhi::BindingSetHandle bindingSet = Device->createBindingSet(
                bindingDesc,
                PreviewBindingLayout);
            if (!bindingSet)
            {
                throw std::runtime_error(
                    "NVRHI failed to bind offline reference preview resources");
            }
            const OfflineReferencePreviewConstants constants{
                .Dimensions = glm::uvec2(
                    NativeOutputs.Width,
                    NativeOutputs.Height),
                .Aov = static_cast<uint32_t>(aov),
                .TargetSamples = std::max(OfflineSettings.TargetSamples, 1u),
            };
            nvrhi::ComputeState state;
            state.setPipeline(SelectPreviewPipeline(aov, bToneMapped))
                .addBindingSet(bindingSet);
            commandList->setComputeState(state);
            commandList->setPushConstants(&constants, sizeof(constants));
            commandList->dispatch(
                (NativeOutputs.Width + 7u) / 8u,
                (NativeOutputs.Height + 7u) / 8u);
        }

        void CreatePipeline()
        {
            const std::filesystem::path libraryPath =
                ShaderArtifactCacheRoot /
                std::string(GetShaderArtifactDirectoryName(BinaryFormat)) /
                "HardwareRayTracingReference__lib_6_5.lib";
            const ShaderLibraryCacheKey libraryKey{
                .Source = "HardwareRayTracingReference.hlsl",
                .Variant = "BASE",
                .TargetProfile = "lib_6_5",
                .ContentHash = ReadShaderContentHash(
                    MakeShaderCacheKeyPath(libraryPath)),
                .Backend = Backend,
                .BinaryFormat = BinaryFormat,
            };
            ShaderLibrary = ShaderManagerInstance.LoadShaderLibraryFile(
                libraryKey,
                libraryPath);
            RayGenerationShader = ShaderManagerInstance.GetShaderFromLibrary(
                libraryKey,
                "ReferenceRayGeneration",
                nvrhi::ShaderType::RayGeneration);
            MissShader = ShaderManagerInstance.GetShaderFromLibrary(
                libraryKey,
                "ReferenceMiss",
                nvrhi::ShaderType::Miss);
            ClosestHitShader = ShaderManagerInstance.GetShaderFromLibrary(
                libraryKey,
                "ReferenceClosestHit",
                nvrhi::ShaderType::ClosestHit);
            AnyHitShader = ShaderManagerInstance.GetShaderFromLibrary(libraryKey,
                "ReferenceAnyHit", nvrhi::ShaderType::AnyHit);
            if (!RayGenerationShader || !MissShader || !ClosestHitShader || !AnyHitShader)
                throw std::runtime_error("Reference path tracer shader library is incomplete");

            nvrhi::BindingLayoutDesc frameLayoutDesc;
            frameLayoutDesc
                .setVisibility(nvrhi::ShaderType::AllRayTracing)
                .setRegisterSpaceAndDescriptorSet(0)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(
                    0,
                    sizeof(ReferencePathTracerConstants)));
            FrameBindingLayout = Device->createBindingLayout(frameLayoutDesc);

            nvrhi::BindingLayoutDesc sceneLayoutDesc;
            sceneLayoutDesc
                .setVisibility(nvrhi::ShaderType::AllRayTracing)
                .setRegisterSpaceAndDescriptorSet(2)
                .addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1))
                .addItem(nvrhi::BindingLayoutItem::RawBuffer_SRV(2))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(6))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(7))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(8).setSize(512))
                .addItem(nvrhi::BindingLayoutItem::Sampler(0));
            SceneBindingLayout = Device->createBindingLayout(sceneLayoutDesc);

            nvrhi::BindingLayoutDesc outputLayoutDesc;
            outputLayoutDesc
                .setVisibility(nvrhi::ShaderType::RayGeneration)
                .setRegisterSpaceAndDescriptorSet(3);
            for (uint32_t slot = 0; slot < 9; ++slot)
                outputLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(slot));
            OutputBindingLayout = Device->createBindingLayout(outputLayoutDesc);

            nvrhi::BindingLayoutDesc environmentLayoutDesc;
            environmentLayoutDesc
                .setVisibility(nvrhi::ShaderType::RayGeneration)
                .setRegisterSpaceAndDescriptorSet(4)
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
                .addItem(nvrhi::BindingLayoutItem::Sampler(0));
            EnvironmentBindingLayout = Device->createBindingLayout(
                environmentLayoutDesc);

            nvrhi::SamplerDesc environmentSamplerDesc;
            environmentSamplerDesc.setAllFilters(true)
                .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
            EnvironmentSampler = Device->createSampler(environmentSamplerDesc);
            MaterialSampler = Device->createSampler(nvrhi::SamplerDesc().setAllFilters(true)
                .setAllAddressModes(nvrhi::SamplerAddressMode::Wrap));
            nvrhi::TextureDesc unusedTextureDesc;
            unusedTextureDesc.width = unusedTextureDesc.height = 1;
            unusedTextureDesc.format = nvrhi::Format::RGBA8_UNORM;
            unusedTextureDesc.initialState = nvrhi::ResourceStates::ShaderResource;
            unusedTextureDesc.keepInitialState = true;
            unusedTextureDesc.debugName = "Unused PT material descriptor";
            UnusedMaterialTexture = Device->createTexture(unusedTextureDesc);

            nvrhi::TextureDesc fallbackEnvironmentDesc;
            fallbackEnvironmentDesc.width = 1;
            fallbackEnvironmentDesc.height = 1;
            fallbackEnvironmentDesc.arraySize = 6;
            fallbackEnvironmentDesc.mipLevels = 1;
            fallbackEnvironmentDesc.format = nvrhi::Format::RGBA16_FLOAT;
            fallbackEnvironmentDesc.dimension =
                nvrhi::TextureDimension::TextureCube;
            fallbackEnvironmentDesc.isShaderResource = true;
            fallbackEnvironmentDesc.initialState =
                nvrhi::ResourceStates::ShaderResource;
            fallbackEnvironmentDesc.keepInitialState = true;
            fallbackEnvironmentDesc.debugName =
                "Offline Reference Fallback Environment";
            FallbackEnvironment = Device->createTexture(
                fallbackEnvironmentDesc);

            if (!FrameBindingLayout || !SceneBindingLayout ||
                !OutputBindingLayout || !EnvironmentBindingLayout ||
                !EnvironmentSampler || !FallbackEnvironment)
                throw std::runtime_error("NVRHI failed to create reference path tracer binding layouts");

            nvrhi::BindingSetDesc fallbackEnvironmentBindingDesc;
            fallbackEnvironmentBindingDesc
                .addItem(nvrhi::BindingSetItem::Texture_SRV(
                    0,
                    FallbackEnvironment))
                .addItem(nvrhi::BindingSetItem::Sampler(
                    0,
                    EnvironmentSampler));
            FallbackEnvironmentBindingSet = Device->createBindingSet(
                fallbackEnvironmentBindingDesc,
                EnvironmentBindingLayout);
            if (!FallbackEnvironmentBindingSet)
            {
                throw std::runtime_error(
                    "NVRHI failed to bind the offline reference fallback environment");
            }

            nvrhi::BindingSetDesc frameBindingDesc;
            frameBindingDesc.addItem(nvrhi::BindingSetItem::PushConstants(
                0,
                sizeof(ReferencePathTracerConstants)));
            FrameBindingSet = Device->createBindingSet(
                frameBindingDesc,
                FrameBindingLayout);
            if (!FrameBindingSet)
                throw std::runtime_error("NVRHI failed to create reference path tracer frame binding set");

            nvrhi::rt::PipelineDesc pipelineDesc;
            pipelineDesc
                .addShader(nvrhi::rt::PipelineShaderDesc()
                    .setExportName("ReferenceRayGeneration")
                    .setShader(RayGenerationShader))
                .addShader(nvrhi::rt::PipelineShaderDesc()
                    .setExportName("ReferenceMiss")
                    .setShader(MissShader))
                .addHitGroup(nvrhi::rt::PipelineHitGroupDesc()
                    .setExportName("ReferenceHitGroup")
                    .setClosestHitShader(ClosestHitShader).setAnyHitShader(AnyHitShader))
                .addBindingLayout(FrameBindingLayout)
                .addBindingLayout(SceneBindingLayout)
                .addBindingLayout(OutputBindingLayout)
                .addBindingLayout(EnvironmentBindingLayout)
                .setMaxPayloadSize(96)
                .setMaxAttributeSize(sizeof(float) * 2u)
                .setMaxRecursionDepth(1);
            Pipeline = Device->createRayTracingPipeline(pipelineDesc);
            if (!Pipeline)
                throw std::runtime_error("NVRHI failed to create the reference path tracer pipeline");

            ShaderTable = Pipeline->createShaderTable(
                nvrhi::rt::ShaderTableDesc().enableCaching(3)
                    .setDebugName("Reference Path Tracer Shader Table"));
            if (!ShaderTable)
                throw std::runtime_error("NVRHI failed to create the reference path tracer shader table");
            ShaderTable->setRayGenerationShader("ReferenceRayGeneration");
            if (ShaderTable->addMissShader("ReferenceMiss") != 0 ||
                ShaderTable->addHitGroup("ReferenceHitGroup") != 0)
            {
                throw std::runtime_error("Reference path tracer shader table indices are not deterministic");
            }
            CreatePreviewPipelines();
        }

        [[nodiscard]] nvrhi::TextureHandle CreateOutputTexture(
            uint32_t width,
            uint32_t height,
            nvrhi::Format format,
            std::string debugName)
        {
            nvrhi::TextureDesc desc;
            desc.setWidth(width)
                .setHeight(height)
                .setFormat(format)
                .setIsUAV(true)
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setDebugName(debugName);
            nvrhi::TextureHandle texture = Device->createTexture(desc);
            if (!texture)
                throw std::runtime_error("NVRHI failed to create " + debugName);
            return texture;
        }

        void RetireOutputs()
        {
            if (!NativeOutputs.AccumulatedRadiance)
                return;
            RetiredOutputSets.push_back(RetiredOutputs{
                .Submission = LastSubmittedSubmission,
                .Outputs = std::move(NativeOutputs),
                .BindingSet = std::move(OutputBindingSet),
            });
            NativeOutputs = {};
            OutputBindingSet = nullptr;
        }

        void CreateOutputs(uint32_t width, uint32_t height)
        {
            RetireOutputs();
            NativeOutputs.AccumulatedRadiance = CreateOutputTexture(
                width, height, nvrhi::Format::RGBA32_FLOAT,
                "Reference PT Accumulated Radiance");
            NativeOutputs.DirectDiffuse = CreateOutputTexture(
                width, height, nvrhi::Format::RGBA32_FLOAT,
                "Reference PT Direct Diffuse");
            NativeOutputs.IndirectDiffuse = CreateOutputTexture(
                width, height, nvrhi::Format::RGBA32_FLOAT,
                "Reference PT Indirect Diffuse");
            NativeOutputs.Albedo = CreateOutputTexture(
                width, height, nvrhi::Format::RGBA32_FLOAT,
                "Reference PT Albedo");
            NativeOutputs.WorldNormal = CreateOutputTexture(
                width, height, nvrhi::Format::RGBA32_FLOAT,
                "Reference PT World Normal");
            NativeOutputs.ViewDepth = CreateOutputTexture(
                width, height, nvrhi::Format::R32_FLOAT,
                "Reference PT View Depth");
            NativeOutputs.MaterialId = CreateOutputTexture(
                width, height, nvrhi::Format::R32_UINT,
                "Reference PT Material ID");
            NativeOutputs.InstanceId = CreateOutputTexture(
                width, height, nvrhi::Format::R32_UINT,
                "Reference PT Instance ID");
            NativeOutputs.SampleCount = CreateOutputTexture(
                width, height, nvrhi::Format::R32_UINT,
                "Reference PT Sample Count");
            NativeOutputs.Width = width;
            NativeOutputs.DenoisedRadiance = CreateOutputTexture(
                width, height, nvrhi::Format::RGBA32_FLOAT, "Reference PT Denoised Beauty");
            NativeOutputs.Height = height;
            NativeOutputs.Generation = ++OutputGeneration;

            nvrhi::BindingSetDesc outputDesc;
            outputDesc
                .addItem(nvrhi::BindingSetItem::Texture_UAV(
                    0, NativeOutputs.AccumulatedRadiance))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(
                    1, NativeOutputs.DirectDiffuse))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(
                    2, NativeOutputs.IndirectDiffuse))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(
                    3, NativeOutputs.Albedo))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(
                    4, NativeOutputs.WorldNormal))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(
                    5, NativeOutputs.ViewDepth))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(
                    6, NativeOutputs.MaterialId))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(
                    7, NativeOutputs.InstanceId))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(
                    8, NativeOutputs.SampleCount));
            OutputBindingSet = Device->createBindingSet(
                outputDesc,
                OutputBindingLayout);
            if (!OutputBindingSet)
                throw std::runtime_error("NVRHI failed to bind reference path tracer outputs");
            bGraphRebuildRequested = true;
        }

        void UpdateSceneBindingSet()
        {
            const RayTracingSceneGpuResources& sceneResources =
                Scene->GetGpuResources();
            if (SceneResourceGeneration == sceneResources.Generation &&
                MaterialTextureGeneration == Scene->GetMaterialTextureGeneration() &&
                SceneBindingSet)
            {
                return;
            }
            if (SceneBindingSet)
            {
                RetiredSceneBindings.push_back(RetiredBindingSet{
                    .Submission = LastSubmittedSubmission,
                    .BindingSet = std::move(SceneBindingSet),
                });
            }
            nvrhi::BindingSetDesc sceneDesc;
            sceneDesc
                .addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(
                    0, sceneResources.SceneTlas))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
                    1, sceneResources.Vertices))
                .addItem(nvrhi::BindingSetItem::RawBuffer_SRV(
                    2, sceneResources.Indices))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
                    3, sceneResources.Geometries))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
                    4, sceneResources.Materials))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
                    5, sceneResources.Instances))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
                    6, sceneResources.EmissiveTriangles))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(7, sceneResources.AnalyticLights))
                .addItem(nvrhi::BindingSetItem::Sampler(0, MaterialSampler));
            const auto textures = Scene->GetMaterialTextures();
            for (uint32_t i = 0; i < 512; ++i)
                sceneDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(8,
                    i < textures.size() ? textures[i].Get() : UnusedMaterialTexture.Get()).setArrayElement(i));
            SceneBindingSet = Device->createBindingSet(
                sceneDesc,
                SceneBindingLayout);
            if (!SceneBindingSet)
                throw std::runtime_error("NVRHI failed to bind reference path tracer scene tables");
            SceneResourceGeneration = sceneResources.Generation;
            MaterialTextureGeneration = Scene->GetMaterialTextureGeneration();
            bGraphRebuildRequested = true;
        }

        [[nodiscard]] nvrhi::IBindingSet* GetOrCreateEnvironmentBindingSet(
            nvrhi::ITexture* environment)
        {
            if (!environment)
                return FallbackEnvironmentBindingSet;
            if (BoundEnvironment == environment && EnvironmentBindingSet)
                return EnvironmentBindingSet;
            if (EnvironmentBindingSet)
            {
                RetiredEnvironmentBindings.push_back(RetiredBindingSet{
                    .Submission = LastSubmittedSubmission,
                    .BindingSet = std::move(EnvironmentBindingSet),
                });
            }
            nvrhi::BindingSetDesc bindingDesc;
            bindingDesc
                .addItem(nvrhi::BindingSetItem::Texture_SRV(0, environment))
                .addItem(nvrhi::BindingSetItem::Sampler(
                    0,
                    EnvironmentSampler));
            EnvironmentBindingSet = Device->createBindingSet(
                bindingDesc,
                EnvironmentBindingLayout);
            if (!EnvironmentBindingSet)
            {
                throw std::runtime_error(
                    "NVRHI failed to bind the offline reference scene environment");
            }
            BoundEnvironment = environment;
            return EnvironmentBindingSet;
        }

        [[nodiscard]] TextureHandle ImportOutput(
            RenderGraph& graph,
            const nvrhi::TextureHandle& texture)
        {
            return graph.ImportTexture(
                ToGraphDesc(texture),
                texture,
                nvrhi::ResourceStates::UnorderedAccess,
                nvrhi::ResourceStates::UnorderedAccess);
        }

        [[nodiscard]] TextureHandle AddPreviewPass(
            RenderGraph& renderGraph,
            TextureHandle source,
            OfflineViewportAov aov,
            bool bToneMapped)
        {
            TextureHandle output;
            const nvrhi::Format outputFormat = bToneMapped
                ? nvrhi::Format::RGBA8_UNORM
                : nvrhi::Format::RGBA16_FLOAT;
            const std::string passName = bToneMapped
                ? "OfflineReferenceToneMappedPng"
                : "OfflineReferencePreview[" +
                    std::string(GetAovFileStem(aov)) + "]";
            renderGraph.AddPass<OfflineReferencePreviewPassData>(
                passName,
                RenderGraphQueue::Compute,
                [this, source, outputFormat, passName, &output](
                    RenderGraphBuilder& builder,
                    OfflineReferencePreviewPassData& data)
                {
                    data.Source = builder.ShaderResource(source);
                    data.Output = builder.Write(
                        builder.CreateTexture(RenderGraphTextureDesc{
                            .DebugName = passName + " Output",
                            .Width = NativeOutputs.Width,
                            .Height = NativeOutputs.Height,
                            .Format = outputFormat,
                        }));
                    output = data.Output;
                },
                [this, aov, bToneMapped](
                    RenderGraphContext& context,
                    const OfflineReferencePreviewPassData& data)
                {
                    RecordPreview(
                        context.GetCommandList(),
                        context.GetTexture(
                            data.Source,
                            RenderGraphAccess::Read),
                        context.GetTexture(
                            data.Output,
                            RenderGraphAccess::Write),
                        aov,
                        bToneMapped);
                });
            return output;
        }

        nvrhi::DeviceHandle Device;
        Scope<ShaderManager> OwnedShaderManager;
        ShaderManager& ShaderManagerInstance;
        RayTracingScene* Scene = nullptr;
        std::filesystem::path ShaderArtifactCacheRoot;
        std::filesystem::path OfflineSettingsPath;
        RenderBackend Backend = RenderBackend::DX12;
        ShaderBinaryFormat BinaryFormat = ShaderBinaryFormat::Dxil;
        uint32_t FramesInFlight = 0;
        nvrhi::ShaderLibraryHandle ShaderLibrary;
        nvrhi::ShaderHandle RayGenerationShader;
        nvrhi::ShaderHandle MissShader;
        nvrhi::ShaderHandle ClosestHitShader;
        nvrhi::ShaderHandle AnyHitShader;
        nvrhi::SamplerHandle MaterialSampler;
        nvrhi::TextureHandle UnusedMaterialTexture;
        uint64_t MaterialTextureGeneration = 0;
        nvrhi::BindingLayoutHandle FrameBindingLayout;
        nvrhi::BindingLayoutHandle SceneBindingLayout;
        nvrhi::BindingLayoutHandle OutputBindingLayout;
        nvrhi::BindingLayoutHandle EnvironmentBindingLayout;
        nvrhi::BindingLayoutHandle PreviewBindingLayout;
        nvrhi::BindingSetHandle FrameBindingSet;
        nvrhi::BindingSetHandle SceneBindingSet;
        nvrhi::BindingSetHandle OutputBindingSet;
        nvrhi::BindingSetHandle EnvironmentBindingSet;
        nvrhi::BindingSetHandle FallbackEnvironmentBindingSet;
        nvrhi::SamplerHandle EnvironmentSampler;
        nvrhi::TextureHandle FallbackEnvironment;
        nvrhi::ITexture* BoundEnvironment = nullptr;
        nvrhi::rt::PipelineHandle Pipeline;
        nvrhi::rt::ShaderTableHandle ShaderTable;
        nvrhi::ShaderHandle PreviewFloat4Shader;
        nvrhi::ShaderHandle PreviewScalarShader;
        nvrhi::ShaderHandle PreviewUintShader;
        nvrhi::ShaderHandle PreviewToneMapShader;
        nvrhi::ComputePipelineHandle PreviewFloat4Pipeline;
        nvrhi::ComputePipelineHandle PreviewScalarPipeline;
        nvrhi::ComputePipelineHandle PreviewUintPipeline;
        nvrhi::ComputePipelineHandle PreviewToneMapPipeline;
        ReferencePathTracerNativeOutputs NativeOutputs;
        ReferencePathTracerOutputs GraphOutputs;
        Scope<TextureReadbackQueue> ReadbackQueue;
        OfflineViewportRendererSettings OfflineSettings;
        OfflineViewportSessionState OfflineState =
            OfflineViewportSessionState::Idle;
        Clock::time_point RenderingStartedAt{};
        double ElapsedRenderingSeconds = 0.0;
        std::string OfflineError;
        bool bSceneUnavailable = false;
        std::vector<ReferenceExportRequest> PendingExports;
        std::vector<ReferenceExportRequest> ScheduledExports;
        ReferencePathTracerConstants Constants;
        GlobalIlluminationSettings Settings;
        GlobalIlluminationHistoryTracker HistoryTracker;
        GlobalIlluminationStats Stats{
            .Technique = GlobalIlluminationTechniqueKind::ExternalViewportRenderer,
            .Status = "Unprepared",
        };
        std::vector<RetiredBindingSet> RetiredSceneBindings;
        std::vector<RetiredBindingSet> RetiredEnvironmentBindings;
        std::vector<RetiredOutputs> RetiredOutputSets;
        uint64_t SceneResourceGeneration = 0;
        uint64_t OutputGeneration = 0;
        uint64_t LastSubmittedSubmission = 0;
        uint64_t AccumulatedSamples = 0;
        Detail::ProgressiveSchedule Schedule;
        ProgressiveDenoiser Denoiser;
        std::array<nvrhi::StagingTextureHandle, 3> DenoiseStaging;
        DenoiseSnapshot Snapshot;
        std::unique_ptr<DenoiseSnapshot> PendingDenoisedUpload;
        uint64_t SnapshotSubmission = 0;
        bool bSnapshotRecorded = false;
        bool bNeedSnapshot = false;
        bool bDenoiseUploadRecorded = false;
        uint64_t DenoiseUploadSubmission = 0;
        uint64_t UploadedGeneration = 0, UploadedSamples = 0;
        uint64_t DenoisedGeneration = 0;
        uint64_t DenoisedSamples = 0;
        uint64_t LastSnapshotSamples = 0;
        uint64_t LastSnapshotGeneration = 0;
        double DenoiseMilliseconds = 0;
        std::string DenoiserStatus = ProgressiveDenoiser::Available() ? "OIDN CPU ready" : "OIDN unavailable: build with HE_OFFLINE_OIDN=ON";
        Clock::time_point LastDenoiseAt{};
        nvrhi::TimerQueryHandle TraceTimer;
        uint64_t TimerSubmission = 0;
        uint64_t TimerPixels = 0;
        double TraceMillisecondsPerPixel = 0.0;
        double LastTraceMilliseconds = 0.0;
        double OtherGpuMilliseconds = 0.0;
        bool bClearAccumulation = true;
        bool bRecordedClear = false;
        bool bInteractivePreview = true;
        Clock::time_point LastMotionAt = Clock::now();
        uint64_t LastCameraFingerprint = 0;
        uint64_t LastSceneStateFingerprint = 0;
        uint32_t ResolvedSeed = 0x48454e47u;
        uint32_t RecordedSamples = 0;
        bool bOfflineControlEnabled = false;
        bool bRestartRequired = false;
        bool bSceneInvalidated = false;
        bool bCameraInvalidated = false;
        bool bHasPreparedFingerprint = false;
        bool bFramePrepared = false;
        bool bGraphRebuildRequested = true;
    };

    ReferencePathTracer::ReferencePathTracer(const ReferencePathTracerDesc& desc)
        : mImpl(CreateScope<Impl>(desc))
    {
        if (desc.bOfflineViewportSession &&
            !desc.OfflineSettingsPath.empty() &&
            std::filesystem::is_regular_file(desc.OfflineSettingsPath))
        {
            ApplyOfflineSettings(DeserializeOfflineReferenceRendererSettings(
                Utils::LoadJsonFile(desc.OfflineSettingsPath),
                mImpl->OfflineSettings));
        }
    }

    ReferencePathTracer::~ReferencePathTracer() = default;

    std::string_view ReferencePathTracer::GetProviderId() const noexcept
    {
        return ReferenceProviderId;
    }

    const OfflineViewportRendererSettings&
        ReferencePathTracer::GetOfflineSettings() const noexcept
    {
        return mImpl->OfflineSettings;
    }

    void ReferencePathTracer::ApplyOfflineSettings(
        const OfflineViewportRendererSettings& settings)
    {
        ValidateOfflineSettings(settings);
        OfflineViewportRendererSettings normalized = settings;
        const bool bWasOffline = mImpl->bOfflineControlEnabled;
        if (normalized.SeedMode == OfflineViewportSeedMode::Random)
        {
            const bool bKeepResolvedSeed = bWasOffline &&
                mImpl->OfflineSettings.SeedMode ==
                    OfflineViewportSeedMode::Random &&
                normalized.Seed == mImpl->OfflineSettings.Seed;
            normalized.Seed = bKeepResolvedSeed
                ? mImpl->OfflineSettings.Seed
                : GenerateRandomSeed();
        }
        const bool bIntegralChanged = !bWasOffline ||
            !HaveSameIntegralSettings(mImpl->OfflineSettings, normalized);
        const bool bPreviewChanged = !bWasOffline ||
            mImpl->OfflineSettings.PreviewAov != normalized.PreviewAov ||
            mImpl->OfflineSettings.bDenoise != normalized.bDenoise;
        const uint32_t previousTarget = mImpl->OfflineSettings.TargetSamples;
        mImpl->EnableOfflineControl();
        mImpl->OfflineSettings = std::move(normalized);
        mImpl->ResolvedSeed = mImpl->OfflineSettings.Seed;
        if (bIntegralChanged)
        {
            mImpl->ResetAccumulation(
                GlobalIlluminationHistoryResetReason::Explicit);
            mImpl->ResetOfflineProgressClock();
            mImpl->bSceneInvalidated = false;
            mImpl->bCameraInvalidated = false;
            if (mImpl->OfflineSettings.bAutoReset)
            {
                mImpl->bRestartRequired = false;
                mImpl->OfflineError.clear();
                mImpl->SetOfflineState(
                    OfflineViewportSessionState::Rendering);
            }
            else
            {
                mImpl->bRestartRequired = true;
                mImpl->OfflineError =
                    "Offline settings changed; restart is required.";
                mImpl->SetOfflineState(OfflineViewportSessionState::Paused);
            }
        }
        else if (mImpl->OfflineState != OfflineViewportSessionState::Paused &&
            mImpl->OfflineState != OfflineViewportSessionState::Cancelled && mImpl->AccumulatedSamples >=
            mImpl->OfflineSettings.TargetSamples)
        {
            mImpl->SetOfflineState(OfflineViewportSessionState::Complete);
        }
        else if (mImpl->OfflineState == OfflineViewportSessionState::Complete &&
            mImpl->OfflineSettings.TargetSamples > previousTarget)
        {
            mImpl->SetOfflineState(OfflineViewportSessionState::Rendering);
        }
        if (bPreviewChanged)
            mImpl->bGraphRebuildRequested = true;
        if (!mImpl->OfflineSettingsPath.empty())
        {
            Utils::SaveJsonToFile(
                SerializeOfflineReferenceRendererSettings(
                    mImpl->OfflineSettings),
                mImpl->OfflineSettingsPath);
        }
    }

    OfflineViewportRendererProgress
        ReferencePathTracer::GetOfflineProgress() const
    {
        return {
            .State = mImpl->OfflineState,
            .AccumulatedSamples = mImpl->AccumulatedSamples,
            .SubmittedSamples = mImpl->Schedule.Submitted(),
            .Generation = mImpl->Schedule.Generation(),
            .MaximumSamples = mImpl->Schedule.Maximum(),
            .AverageSamples = mImpl->Schedule.Average(),
            .InFlight = mImpl->Schedule.InFlight() ? 1u : 0u,
            .StaleResults = mImpl->Schedule.Stale(),
            .bTimingAvailable = mImpl->TraceMillisecondsPerPixel > 0.0,
            .TracingMilliseconds = mImpl->LastTraceMilliseconds,
            .IncludedInstances = mImpl->Scene->GetStats().TlasInstanceCount,
            .ExcludedInstances = mImpl->Scene->GetStats().ExcludedInstanceCount,
            .RetiredOutputSets = static_cast<uint32_t>(mImpl->RetiredOutputSets.size()),
            .Phase = mImpl->OfflineState == OfflineViewportSessionState::Failed ? "Failed" :
                !mImpl->bHasPreparedFingerprint ? "Preparing" :
                mImpl->OfflineState == OfflineViewportSessionState::Complete ? "Converged (sample target)" :
                mImpl->OfflineState == OfflineViewportSessionState::Paused ? "Paused" :
                mImpl->OfflineState == OfflineViewportSessionState::Cancelled ? "Cancelled" :
                mImpl->bInteractivePreview ? "Interactive / Settling" : "Accumulating",
            .DenoiserStatus = mImpl->DenoiserStatus,
            .DenoiseMilliseconds = mImpl->DenoiseMilliseconds,
            .DenoisedSamples = mImpl->DenoisedGeneration == mImpl->Schedule.Generation() ? mImpl->DenoisedSamples : 0,
            .TargetSamples = mImpl->OfflineSettings.TargetSamples,
            .ElapsedSeconds = mImpl->GetElapsedSeconds(),
            .LastGpuMilliseconds = mImpl->Stats.LastGpuMilliseconds,
            .Width = mImpl->NativeOutputs.Width,
            .Height = mImpl->NativeOutputs.Height,
            .LastResetReason = mImpl->Stats.LastResetReasons,
            .bSceneInvalidated = mImpl->bSceneInvalidated,
            .bCameraInvalidated = mImpl->bCameraInvalidated,
            .Error = mImpl->OfflineError,
        };
    }

    void ReferencePathTracer::SetLastGpuMilliseconds(
        double milliseconds) noexcept
    {
        if (std::isfinite(milliseconds) && milliseconds > 0)
            mImpl->OtherGpuMilliseconds = std::max(0.0, milliseconds - mImpl->LastTraceMilliseconds);
        mImpl->Stats.LastGpuMilliseconds =
            std::isfinite(milliseconds) ? std::max(milliseconds, 0.0) : 0.0;
    }

    std::span<const OfflineViewportAov>
        ReferencePathTracer::GetAvailableAovs() const noexcept
    {
        return ReferenceAovs;
    }

    OfflineViewportNativeAov ReferencePathTracer::GetNativeAov(
        OfflineViewportAov aov) const
    {
        const nvrhi::TextureHandle texture = mImpl->GetNativeAovTexture(aov);
        if (!texture)
            return {};
        const nvrhi::TextureDesc& desc = texture->getDesc();
        const bool bSceneLinear = aov == OfflineViewportAov::Beauty || aov == OfflineViewportAov::DenoisedBeauty ||
            aov == OfflineViewportAov::DirectDiffuse ||
            aov == OfflineViewportAov::IndirectDiffuse ||
            aov == OfflineViewportAov::Albedo;
        return {
            .Texture = texture,
            .Format = desc.format,
            .Width = desc.width,
            .Height = desc.height,
            .bSceneLinear = bSceneLinear,
        };
    }

    void ReferencePathTracer::Pause() noexcept
    {
        if (!mImpl->bOfflineControlEnabled)
        {
            mImpl->bOfflineControlEnabled = true;
            mImpl->OfflineError.clear();
        }
        const OfflineViewportSessionState nextState =
            Detail::ResolveReferenceOfflineControlState(
                mImpl->OfflineState,
                Detail::ReferenceOfflineControlCommand::Pause,
                mImpl->AccumulatedSamples,
                mImpl->OfflineSettings.TargetSamples,
                mImpl->bRestartRequired);
        if (nextState != mImpl->OfflineState)
        {
            mImpl->SetOfflineState(nextState);
            mImpl->bGraphRebuildRequested = true;
        }
    }

    void ReferencePathTracer::Resume() noexcept
    {
        if (!mImpl->bOfflineControlEnabled)
            mImpl->bOfflineControlEnabled = true;
        const OfflineViewportSessionState nextState =
            Detail::ResolveReferenceOfflineControlState(
                mImpl->OfflineState,
                Detail::ReferenceOfflineControlCommand::Resume,
                mImpl->AccumulatedSamples,
                mImpl->OfflineSettings.TargetSamples,
                mImpl->bRestartRequired);
        if (nextState != mImpl->OfflineState)
        {
            mImpl->SetOfflineState(nextState);
            mImpl->bGraphRebuildRequested = true;
        }
    }

    void ReferencePathTracer::Restart()
    {
        mImpl->EnableOfflineControl();
        if (mImpl->OfflineSettings.SeedMode ==
            OfflineViewportSeedMode::Random)
        {
            mImpl->ResolvedSeed = GenerateRandomSeed();
            mImpl->OfflineSettings.Seed = mImpl->ResolvedSeed;
        }
        else
        {
            mImpl->ResolvedSeed = mImpl->OfflineSettings.Seed;
        }
        mImpl->ResetAccumulation(
            GlobalIlluminationHistoryResetReason::Explicit);
        mImpl->bRestartRequired = false;
        mImpl->bSceneInvalidated = false;
        mImpl->bCameraInvalidated = false;
        mImpl->OfflineError.clear();
        mImpl->ResetOfflineProgressClock();
        mImpl->SetOfflineState(Detail::ResolveReferenceOfflineControlState(
            mImpl->OfflineState,
            Detail::ReferenceOfflineControlCommand::Restart,
            0,
            mImpl->OfflineSettings.TargetSamples,
            false));
    }

    void ReferencePathTracer::Cancel() noexcept
    {
        mImpl->bInteractivePreview = false;
        mImpl->bNeedSnapshot = false;
        mImpl->PendingDenoisedUpload.reset();
        if (!mImpl->bOfflineControlEnabled)
            mImpl->bOfflineControlEnabled = true;
        mImpl->RecordedSamples = 0;
        mImpl->CancelUnsubmittedExports();
        mImpl->SetOfflineState(Detail::ResolveReferenceOfflineControlState(
            mImpl->OfflineState,
            Detail::ReferenceOfflineControlCommand::Cancel,
            mImpl->AccumulatedSamples,
            mImpl->OfflineSettings.TargetSamples,
            mImpl->bRestartRequired));
        mImpl->bGraphRebuildRequested = true;
    }

    bool ReferencePathTracer::HasPendingWork() const noexcept
    {
        return mImpl->Denoiser.Busy() || mImpl->SnapshotSubmission || mImpl->DenoiseUploadSubmission || mImpl->Schedule.InFlight() ||
            mImpl->ReadbackQueue->GetPendingCount() != 0;
    }

    TextureReadbackHandle ReferencePathTracer::RequestExport(
        OfflineViewportAov aov,
        std::filesystem::path outputPath,
        OfflineViewportOutputFormat format)
    {
        Detail::ValidateReferenceOfflineExportRequest(aov, format);
        if (mImpl->bSceneUnavailable)
            throw std::runtime_error(mImpl->OfflineError);
        if (aov == OfflineViewportAov::DenoisedBeauty && !mImpl->GetNativeAovTexture(aov))
            throw std::runtime_error("No completed denoised result is available for this generation");

        nvrhi::Format nativeFormat = nvrhi::Format::UNKNOWN;
        switch (aov)
        {
        case OfflineViewportAov::DenoisedBeauty:
        case OfflineViewportAov::Beauty:
        case OfflineViewportAov::DirectDiffuse:
        case OfflineViewportAov::IndirectDiffuse:
            nativeFormat = nvrhi::Format::RGBA32_FLOAT;
            break;
        case OfflineViewportAov::Albedo:
        case OfflineViewportAov::WorldNormal:
            nativeFormat = nvrhi::Format::RGBA32_FLOAT;
            break;
        case OfflineViewportAov::ViewDepth:
            nativeFormat = nvrhi::Format::R32_FLOAT;
            break;
        case OfflineViewportAov::MaterialId:
        case OfflineViewportAov::InstanceId:
        case OfflineViewportAov::SampleCount:
            nativeFormat = nvrhi::Format::R32_UINT;
            break;
        default:
            throw std::out_of_range("Offline reference export AOV is invalid");
        }
        if (outputPath.empty())
        {
            if (mImpl->OfflineSettings.OutputDirectory.empty())
                throw std::invalid_argument(
                    "Offline reference export path and output directory are empty");
            const std::string baseName =
                mImpl->OfflineSettings.OutputFileName.empty()
                ? "Reference"
                : mImpl->OfflineSettings.OutputFileName;
            outputPath = mImpl->OfflineSettings.OutputDirectory /
                (baseName + "-" + std::string(GetAovFileStem(aov)) +
                    (format == OfflineViewportOutputFormat::Pfm
                        ? ".pfm"
                        : format == OfflineViewportOutputFormat::ToneMappedPng
                            ? ".png"
                            : ".raw"));
        }

        mImpl->EnableOfflineControl();
        const TextureReadbackDesc desc{
            .OutputPath = std::move(outputPath),
            .OutputFormat = format == OfflineViewportOutputFormat::Pfm
                ? TextureReadbackOutputFormat::Pfm
                : format == OfflineViewportOutputFormat::ToneMappedPng
                    ? TextureReadbackOutputFormat::Png
                    : TextureReadbackOutputFormat::Raw,
            .AlphaMode = format == OfflineViewportOutputFormat::Pfm &&
                    (nativeFormat == nvrhi::Format::RGBA16_FLOAT ||
                        nativeFormat == nvrhi::Format::RGBA32_FLOAT)
                ? TextureReadbackAlphaMode::Discard
                : TextureReadbackAlphaMode::Preserve,
            .NonFinitePolicy = TextureReadbackNonFinitePolicy::Reject,
            .bReplaceExisting = true,
        };
        const TextureReadbackHandle handle = mImpl->ReadbackQueue->Reserve(desc);
        try
        {
            mImpl->PendingExports.push_back({
                .Generation = mImpl->Schedule.Generation(),
                .Aov = aov,
                .Format = format,
                .Handle = handle,
            });
        }
        catch (...)
        {
            mImpl->ReadbackQueue->Release(handle);
            throw;
        }
        mImpl->bGraphRebuildRequested = true;
        return handle;
    }

    bool ReferencePathTracer::IsExportValid(
        TextureReadbackHandle handle) const noexcept
    {
        return mImpl->ReadbackQueue->IsValid(handle);
    }

    TextureReadbackResult ReferencePathTracer::GetExportResult(
        TextureReadbackHandle handle) const
    {
        return mImpl->ReadbackQueue->GetResult(handle);
    }

    void ReferencePathTracer::ReleaseExport(TextureReadbackHandle handle)
    {
        std::erase_if(mImpl->PendingExports,
            [handle](const ReferenceExportRequest& request)
            {
                return request.Handle == handle;
            });
        std::erase_if(mImpl->ScheduledExports,
            [handle](const ReferenceExportRequest& request)
            {
                return request.Handle == handle;
            });
        mImpl->ReadbackQueue->Release(handle);
    }

    void ReferencePathTracer::PrepareFrame(
        const ReferencePathTracerFrameParameters& frame,
        const GlobalIlluminationSettings& settings)
    {
        if (frame.Width == 0 || frame.Height == 0)
            throw std::invalid_argument("ReferencePathTracer frame dimensions must be non-zero");
        if (!IsFinite(frame.Camera.ViewProjection))
            throw std::invalid_argument("ReferencePathTracer camera matrix is not finite");
        if (settings.Technique !=
            GlobalIlluminationTechniqueKind::ExternalViewportRenderer)
        {
            throw std::invalid_argument("ReferencePathTracer requires the ReferencePathTracer technique setting");
        }
        if (settings.Denoiser != GlobalIlluminationDenoiserKind::None)
            throw std::invalid_argument("ReferencePathTracer forbids denoising");
        if (settings.SamplesPerPixel != 1)
            throw std::invalid_argument("ReferencePathTracer currently records exactly one sample per pixel per frame");
        if (settings.MaxBounces > 16)
            throw std::invalid_argument("ReferencePathTracer maximum bounce count exceeds 16");
        Collect(frame.CompletedSubmission);
        if (!mImpl->Scene->IsReady())
        {
            // Empty or fully excluded worlds are valid editor states. Do not
            // bind a null TLAS or present a previous generation as this world.
            if (!mImpl->bSceneUnavailable)
            {
                mImpl->ResetAccumulation(GlobalIlluminationHistoryResetReason::SceneReload);
                mImpl->bSceneUnavailable = true;
                for (const auto& request : mImpl->PendingExports)
                    mImpl->ReadbackQueue->CancelPending(request.Handle);
                mImpl->PendingExports.clear();
            }
            mImpl->bNeedSnapshot = false;
            mImpl->PendingDenoisedUpload.reset();
            mImpl->bInteractivePreview = false;
            if (mImpl->NativeOutputs.Width != 1 || mImpl->NativeOutputs.Height != 1)
                mImpl->CreateOutputs(1, 1);
            const auto& stats = mImpl->Scene->GetStats();
            std::string error = "Path tracing has no supported geometry (excluded " +
                std::to_string(stats.ExcludedInstanceCount) + " sections).";
            for (size_t index = 0; index < stats.ExcludedBySupport.size(); ++index)
            {
                if (stats.ExcludedBySupport[index] != 0)
                    error += " " + std::string(ToString(static_cast<RayTracingGeometrySupport>(index))) +
                        ": " + std::to_string(stats.ExcludedBySupport[index]) + ".";
            }
            error += " Use Raster 3D for this scene; supported static opaque geometry will resume PT automatically.";
            if (error != mImpl->OfflineError) HE_CORE_WARN("{0}", error);
            mImpl->MarkOfflineFailed(std::move(error));
            mImpl->Stats.RaysPerPixel = 0;
            mImpl->Stats.AccumulatedSamples = 0;
            mImpl->bFramePrepared = true;
            return;
        }
        if (std::exchange(mImpl->bSceneUnavailable, false))
        {
            mImpl->OfflineError.clear();
            mImpl->ResetAccumulation(GlobalIlluminationHistoryResetReason::SceneReload);
            if (mImpl->OfflineState == OfflineViewportSessionState::Failed)
                mImpl->SetOfflineState(OfflineViewportSessionState::Rendering);
        }
        const uint64_t cameraFingerprint =
            ComputeCameraFingerprint(frame.Camera);
        const bool bCameraChanged = mImpl->bHasPreparedFingerprint &&
            mImpl->LastCameraFingerprint != cameraFingerprint;
        const bool bSceneChanged = mImpl->bHasPreparedFingerprint &&
            mImpl->LastSceneStateFingerprint != frame.SceneStateFingerprint;
        mImpl->LastCameraFingerprint = cameraFingerprint;
        mImpl->LastSceneStateFingerprint = frame.SceneStateFingerprint;
        mImpl->bHasPreparedFingerprint = true;
        mImpl->bCameraInvalidated = false;
        mImpl->bSceneInvalidated = false;
        if (mImpl->NativeOutputs.Width != frame.Width ||
            mImpl->NativeOutputs.Height != frame.Height)
        {
            mImpl->CreateOutputs(frame.Width, frame.Height);
        }
        mImpl->UpdateSceneBindingSet();

        GlobalIlluminationHistorySignature signature = frame.HistorySignature;
        signature.Width = frame.Width;
        signature.Height = frame.Height;
        signature.RenderScale = settings.RenderScale;
        signature.Technique = settings.Technique;
        signature.Denoiser = settings.Denoiser;
        signature.SamplesPerPixel = settings.SamplesPerPixel;
        signature.MaxBounces = settings.MaxBounces;
        signature.bHalfResolution = false;
        signature.TechniqueStateFingerprint =
            ComputeReferencePathTracerAccumulationFingerprint(frame, settings);
        const GlobalIlluminationHistoryResetReason resetReasons =
            mImpl->HistoryTracker.Update(signature);
        if (resetReasons != GlobalIlluminationHistoryResetReason::None)
        {
            mImpl->AccumulatedSamples = 0;
            mImpl->Schedule.Reset(frame.Height);
            mImpl->bClearAccumulation = true;
            mImpl->PendingDenoisedUpload.reset();
            ++mImpl->Stats.HistoryResetCount;
            mImpl->Stats.LastResetReasons = resetReasons;
            mImpl->bCameraInvalidated = bCameraChanged ||
                HasHistoryResetReason(
                    resetReasons,
                    GlobalIlluminationHistoryResetReason::CameraCut) ||
                HasHistoryResetReason(
                    resetReasons,
                    GlobalIlluminationHistoryResetReason::Projection);
            mImpl->bSceneInvalidated = bSceneChanged ||
                HasHistoryResetReason(
                    resetReasons,
                    GlobalIlluminationHistoryResetReason::SceneReload) ||
                HasHistoryResetReason(
                    resetReasons,
                    GlobalIlluminationHistoryResetReason::GeometryTopology) ||
                HasHistoryResetReason(
                    resetReasons,
                    GlobalIlluminationHistoryResetReason::Material) ||
                HasHistoryResetReason(
                    resetReasons,
                    GlobalIlluminationHistoryResetReason::Light);
            const bool bOnlyFirstFrame = resetReasons ==
                GlobalIlluminationHistoryResetReason::FirstFrame;
            if (mImpl->bOfflineControlEnabled && !bOnlyFirstFrame &&
                mImpl->OfflineState != OfflineViewportSessionState::Cancelled &&
                mImpl->OfflineState != OfflineViewportSessionState::Failed)
            {
                mImpl->ResetOfflineProgressClock();
                if (mImpl->OfflineSettings.bAutoReset)
                {
                    mImpl->bRestartRequired = false;
                    mImpl->OfflineError.clear();
                    if (mImpl->OfflineState != OfflineViewportSessionState::Paused)
                        mImpl->SetOfflineState(OfflineViewportSessionState::Rendering);
                }
                else
                {
                    mImpl->bRestartRequired = true;
                    mImpl->OfflineError =
                        "Camera, scene, or integral settings changed; restart is required.";
                    mImpl->SetOfflineState(
                        OfflineViewportSessionState::Paused);
                }
            }
        }

        mImpl->Settings = settings;
        mImpl->Constants.InverseViewProjection = glm::inverse(
            frame.Camera.ViewProjection);
        const float maximumDistance = ResolveMaximumRayDistance(frame);
        mImpl->Constants.CameraPositionAndMaxDistance = glm::vec4(
            frame.Camera.Position,
            maximumDistance);
        mImpl->Constants.DimensionsSampleBounces = glm::uvec4(
            frame.Width,
            frame.Height,
            static_cast<uint32_t>(std::min<uint64_t>(
                mImpl->AccumulatedSamples,
                std::numeric_limits<uint32_t>::max())),
            settings.MaxBounces);
        mImpl->Constants.SamplingSettings = glm::uvec4(
            frame.FixedSeed,
            settings.bDirectOnly ? 1u : 0u,
            mImpl->Scene->GetStats().EmissiveTriangleCount,
            frame.bSampleSceneEnvironment ? 1u : 0u);
        mImpl->Constants.EnvironmentRadiance = glm::vec4(
            glm::max(frame.EnvironmentRadiance, glm::vec3(0.0f)),
            0.0f);
        mImpl->Stats.RaysPerPixel = 1;
        if (mImpl->bOfflineControlEnabled)
        {
            mImpl->Stats.RaysPerPixel = mImpl->ResolveDispatchCount() > 0 ? 1 : 0;
            if (mImpl->AccumulatedSamples >=
                    mImpl->OfflineSettings.TargetSamples &&
                mImpl->OfflineState == OfflineViewportSessionState::Rendering)
            {
                mImpl->SetOfflineState(OfflineViewportSessionState::Complete);
            }
        }
        mImpl->Stats.BounceCount = settings.MaxBounces;
        mImpl->Stats.AccumulatedSamples = mImpl->AccumulatedSamples;
        mImpl->Stats.Status = mImpl->bOfflineControlEnabled
            ? "Offline reference ready"
            : "Ready";
        mImpl->bFramePrepared = true;
    }

    GlobalIlluminationPrepareResult ReferencePathTracer::PrepareFrame(
        const GlobalIlluminationFrameContext& frame,
        const GlobalIlluminationSettings& settings)
    {
        ReferencePathTracerFrameParameters referenceFrame{
            .Camera = frame.Camera,
            .HistorySignature = frame.HistorySignature,
            .Width = frame.Width,
            .Height = frame.Height,
            .MaximumRayDistance = frame.MaximumRayDistance,
            .EnvironmentRadiance = frame.EnvironmentRadiance,
            .bSampleSceneEnvironment = false,
            .SceneStateFingerprint = frame.SceneStateFingerprint,
            .CompletedSubmission = frame.CompletedSubmission,
        };
        GlobalIlluminationSettings effectiveSettings = settings;
        if (mImpl->bOfflineControlEnabled)
        {
            auto [width, height] = ResolveOfflineDimensions(
                frame.Width,
                frame.Height,
                mImpl->OfflineSettings);
            const auto now = Impl::Clock::now();
            if (!mImpl->bHasPreparedFingerprint || mImpl->LastCameraFingerprint != ComputeCameraFingerprint(frame.Camera))
                mImpl->LastMotionAt = now;
            mImpl->bInteractivePreview = mImpl->OfflineSettings.bInteractive &&
                std::chrono::duration<double, std::milli>(now - mImpl->LastMotionAt).count() <
                    mImpl->OfflineSettings.SettleMilliseconds;
            if (mImpl->bInteractivePreview)
            {
                const double budget = std::min<double>(mImpl->OfflineSettings.WorkBudgetMilliseconds,
                    500.0 / mImpl->OfflineSettings.TargetFramesPerSecond) * 0.65;
                const double previewPixels = mImpl->TraceMillisecondsPerPixel > 0
                    ? std::clamp(budget / mImpl->TraceMillisecondsPerPixel, 1024.0, 65536.0) : 4096.0;
                const float scale = std::min({mImpl->OfflineSettings.InteractiveScale,
                    320.0f / static_cast<float>(width),
                    static_cast<float>(std::sqrt(previewPixels / (double(width) * height)))});
                width = std::max(1u, static_cast<uint32_t>(width * scale));
                height = std::max(1u, static_cast<uint32_t>(height * scale));
            }
            referenceFrame.Width = width;
            referenceFrame.Height = height;
            referenceFrame.FixedSeed = mImpl->ResolvedSeed;
            referenceFrame.MaximumRayDistance =
                mImpl->OfflineSettings.MaximumRayDistance;
            referenceFrame.EnvironmentRadiance =
                mImpl->OfflineSettings.bUseSceneEnvironment
                ? frame.EnvironmentRadiance
                : mImpl->OfflineSettings.EnvironmentRadiance;
            referenceFrame.bSampleSceneEnvironment =
                mImpl->OfflineSettings.bUseSceneEnvironment;
            effectiveSettings.Technique =
                GlobalIlluminationTechniqueKind::ExternalViewportRenderer;
            effectiveSettings.Denoiser =
                GlobalIlluminationDenoiserKind::None;
            effectiveSettings.RenderScale = mImpl->OfflineSettings.RenderScale;
            effectiveSettings.SamplesPerPixel = 1;
            effectiveSettings.MaxBounces = mImpl->OfflineSettings.MaxBounces;
            effectiveSettings.bDirectOnly =
                mImpl->OfflineSettings.bDirectOnly;
        }
        PrepareFrame(
            referenceFrame,
            effectiveSettings);
        return {
            .bGraphRebuildRequested = ConsumeGraphRebuildRequest(),
        };
    }

    bool ReferencePathTracer::IsSupported(
        const RenderCapabilities& capabilities) const
    {
        return QueryHardwareRayTracingSupport(capabilities).bRealtimeRtgi;
    }

    GlobalIlluminationOutputs ReferencePathTracer::AddPasses(
        RenderGraph& renderGraph,
        const GlobalIlluminationInputs& inputs,
        const GlobalIlluminationSettings& settings)
    {
        if (!mImpl->bFramePrepared)
            throw std::logic_error("ReferencePathTracer AddPasses requires PrepareFrame");
        if (settings.Technique !=
                GlobalIlluminationTechniqueKind::ExternalViewportRenderer ||
            settings.Denoiser != GlobalIlluminationDenoiserKind::None)
        {
            throw std::invalid_argument("ReferencePathTracer received incompatible settings");
        }
        if (mImpl->bSceneUnavailable)
        {
            const auto output = mImpl->ImportOutput(renderGraph, mImpl->NativeOutputs.AccumulatedRadiance);
            renderGraph.AddPass<TextureHandle>("OfflineUnavailablePreview", RenderGraphQueue::Graphics,
                [output](RenderGraphBuilder& builder, TextureHandle& data)
                {
                    data = builder.UnorderedAccess(output);
                },
                [](RenderGraphContext& context, const TextureHandle& data)
                {
                    context.GetCommandList()->clearTextureFloat(
                        context.GetTexture(data, RenderGraphAccess::ReadWrite),
                        nvrhi::AllSubresources, nvrhi::Color(0.f));
                });
            return {.CompositeRadiance = output};
        }
        if (!inputs.SceneTlas || !inputs.GeometryBuffer ||
            !inputs.MaterialBuffer || !inputs.RayTracingVertexBuffer ||
            !inputs.RayTracingIndexBuffer ||
            !inputs.RayTracingInstanceBuffer || !inputs.LightBuffer)
        {
            throw std::invalid_argument("ReferencePathTracer inputs are incomplete");
        }

        mImpl->GraphOutputs = {
            .DenoisedRadiance = mImpl->ImportOutput(renderGraph, mImpl->NativeOutputs.DenoisedRadiance),
            .AccumulatedRadiance = mImpl->ImportOutput(
                renderGraph, mImpl->NativeOutputs.AccumulatedRadiance),
            .DirectDiffuse = mImpl->ImportOutput(
                renderGraph, mImpl->NativeOutputs.DirectDiffuse),
            .IndirectDiffuse = mImpl->ImportOutput(
                renderGraph, mImpl->NativeOutputs.IndirectDiffuse),
            .Albedo = mImpl->ImportOutput(
                renderGraph, mImpl->NativeOutputs.Albedo),
            .WorldNormal = mImpl->ImportOutput(
                renderGraph, mImpl->NativeOutputs.WorldNormal),
            .ViewDepth = mImpl->ImportOutput(
                renderGraph, mImpl->NativeOutputs.ViewDepth),
            .MaterialId = mImpl->ImportOutput(
                renderGraph, mImpl->NativeOutputs.MaterialId),
            .InstanceId = mImpl->ImportOutput(
                renderGraph, mImpl->NativeOutputs.InstanceId),
            .SampleCount = mImpl->ImportOutput(
                renderGraph, mImpl->NativeOutputs.SampleCount),
        };

        const bool bRecordTracePass =
            !mImpl->bOfflineControlEnabled ||
            mImpl->OfflineState == OfflineViewportSessionState::Rendering;
        if (bRecordTracePass)
        {
            std::vector<TextureHandle> materialTextures;
            for (const auto& texture : mImpl->Scene->GetMaterialTextures())
                materialTextures.push_back(renderGraph.ImportTexture(ToGraphDesc(texture), texture,
                    nvrhi::ResourceStates::ShaderResource, nvrhi::ResourceStates::ShaderResource));
            renderGraph.AddPass<ReferenceTracePassData>(
            "ReferencePathTrace",
            RenderGraphQueue::Graphics,
            [inputs, materialTextures, outputs = mImpl->GraphOutputs](
                RenderGraphBuilder& builder,
                ReferenceTracePassData& data)
            {
                data.AnalyticLights = builder.ShaderResource(inputs.AnalyticLightBuffer);
                for (auto texture : materialTextures) data.MaterialTextures.push_back(builder.ShaderResource(texture));
                data.SceneTlas = builder.AccelerationStructureRead(inputs.SceneTlas);
                data.Vertices = builder.ShaderResource(
                    inputs.RayTracingVertexBuffer);
                data.Indices = builder.ShaderResource(
                    inputs.RayTracingIndexBuffer);
                data.Geometries = builder.ShaderResource(inputs.GeometryBuffer);
                data.Materials = builder.ShaderResource(inputs.MaterialBuffer);
                data.Instances = builder.ShaderResource(
                    inputs.RayTracingInstanceBuffer);
                data.EmissiveTriangles = builder.ShaderResource(inputs.LightBuffer);
                if (inputs.Environment)
                {
                    data.Environment = builder.ShaderResource(
                        inputs.Environment);
                }
                data.Outputs.AccumulatedRadiance = builder.UnorderedAccess(
                    outputs.AccumulatedRadiance);
                data.Outputs.DirectDiffuse = builder.UnorderedAccess(outputs.DirectDiffuse);
                data.Outputs.IndirectDiffuse = builder.UnorderedAccess(outputs.IndirectDiffuse);
                data.Outputs.Albedo = builder.UnorderedAccess(outputs.Albedo);
                data.Outputs.WorldNormal = builder.UnorderedAccess(outputs.WorldNormal);
                data.Outputs.ViewDepth = builder.UnorderedAccess(outputs.ViewDepth);
                data.Outputs.MaterialId = builder.UnorderedAccess(outputs.MaterialId);
                data.Outputs.InstanceId = builder.UnorderedAccess(outputs.InstanceId);
                data.Outputs.SampleCount = builder.UnorderedAccess(outputs.SampleCount);
            },
            [impl = mImpl.get()](
                RenderGraphContext& context,
                const ReferenceTracePassData& data)
            {
                try
                {
                    (void)context.GetBuffer(data.AnalyticLights, RenderGraphAccess::Read);
                    for (auto texture : data.MaterialTextures) (void)context.GetTexture(texture, RenderGraphAccess::Read);
                    (void)context.GetAccelerationStructure(
                        data.SceneTlas,
                        RenderGraphAccess::Read);
                    (void)context.GetBuffer(
                        data.Vertices,
                        RenderGraphAccess::Read);
                    (void)context.GetBuffer(
                        data.Indices,
                        RenderGraphAccess::Read);
                    (void)context.GetBuffer(
                        data.Geometries,
                        RenderGraphAccess::Read);
                    (void)context.GetBuffer(
                        data.Materials,
                        RenderGraphAccess::Read);
                    (void)context.GetBuffer(
                        data.Instances,
                        RenderGraphAccess::Read);
                    (void)context.GetBuffer(
                        data.EmissiveTriangles,
                        RenderGraphAccess::Read);
                    nvrhi::ITexture* environment = data.Environment
                        ? context.GetTexture(
                            data.Environment,
                            RenderGraphAccess::Read)
                        : nullptr;
                    const std::array<nvrhi::ITexture*, 9> outputTextures = {
                    context.GetTexture(
                        data.Outputs.AccumulatedRadiance,
                        RenderGraphAccess::ReadWrite),
                    context.GetTexture(
                        data.Outputs.DirectDiffuse,
                        RenderGraphAccess::ReadWrite),
                    context.GetTexture(
                        data.Outputs.IndirectDiffuse,
                        RenderGraphAccess::ReadWrite),
                    context.GetTexture(
                        data.Outputs.Albedo,
                        RenderGraphAccess::ReadWrite),
                    context.GetTexture(
                        data.Outputs.WorldNormal,
                        RenderGraphAccess::ReadWrite),
                    context.GetTexture(
                        data.Outputs.ViewDepth,
                        RenderGraphAccess::ReadWrite),
                    context.GetTexture(
                        data.Outputs.MaterialId,
                        RenderGraphAccess::ReadWrite),
                    context.GetTexture(
                        data.Outputs.InstanceId,
                        RenderGraphAccess::ReadWrite),
                    context.GetTexture(
                        data.Outputs.SampleCount,
                        RenderGraphAccess::ReadWrite),
                };

                    nvrhi::rt::State state;
                    state.setShaderTable(impl->ShaderTable)
                        .addBindingSet(impl->FrameBindingSet)
                        .addBindingSet(impl->SceneBindingSet)
                        .addBindingSet(impl->OutputBindingSet)
                        .addBindingSet(
                            impl->GetOrCreateEnvironmentBindingSet(
                                environment));
                    nvrhi::ICommandList* commandList = context.GetCommandList();
                    commandList->setRayTracingState(state);
                    const double budget = Detail::ResolveProgressiveTraceBudget(
                        impl->OfflineSettings.WorkBudgetMilliseconds,
                        impl->OfflineSettings.TargetFramesPerSecond, impl->OtherGpuMilliseconds);
                    const auto band = impl->Schedule.Reserve(impl->NativeOutputs.Width,
                        impl->bOfflineControlEnabled ? impl->OfflineSettings.TargetSamples : 1048576u,
                        budget, impl->TraceMillisecondsPerPixel,
                        impl->ResolveDispatchCount() > 0 && !impl->bNeedSnapshot);
                    const uint32_t dispatchCount = band.Rows ? 1u : 0u;
                    impl->RecordedSamples = 0;
                    if (dispatchCount && impl->bClearAccumulation)
                    {
                        for (size_t index = 0; index < outputTextures.size(); ++index)
                        {
                            if (index >= 6)
                                commandList->clearTextureUInt(outputTextures[index], nvrhi::AllSubresources, 0);
                            else
                                commandList->clearTextureFloat(outputTextures[index], nvrhi::AllSubresources, nvrhi::Color(0.f));
                        }
                        impl->bRecordedClear = true;
                    }
                    if (dispatchCount) commandList->setRayTracingState(state);
                    if (dispatchCount && impl->TraceTimer && !impl->TimerSubmission)
                        commandList->beginTimerQuery(impl->TraceTimer);
                    for (uint32_t dispatchIndex = 0;
                        dispatchIndex < dispatchCount;
                        ++dispatchIndex)
                    {
                        ReferencePathTracerConstants constants = impl->Constants;
                        if (!environment)
                        {
                            constants.SamplingSettings.w = 0u;
                            if (impl->Constants.SamplingSettings.w != 0u)
                                constants.EnvironmentRadiance = glm::vec4(0.0f);
                        }
                        const uint64_t sampleIndex = band.Sample;
                        uint32_t stride = 17;
                        while (std::gcd(stride, impl->NativeOutputs.Height) != 1u) ++stride;
                        constants.EnvironmentRadiance.w = std::bit_cast<float>(
                            band.FirstRow | (stride << 16u));
                        constants.DimensionsSampleBounces.z =
                            static_cast<uint32_t>(std::min<uint64_t>(
                                sampleIndex,
                                std::numeric_limits<uint32_t>::max()));
                        commandList->setPushConstants(
                            &constants,
                            sizeof(constants));
                        commandList->dispatchRays(
                            nvrhi::rt::DispatchRaysArguments().setDimensions(
                                impl->NativeOutputs.Width,
                                band.Rows));
                        if (dispatchIndex + 1u < dispatchCount)
                        {
                            for (nvrhi::ITexture* texture : outputTextures)
                            {
                                nvrhi::utils::TextureUavBarrier(
                                    commandList,
                                    texture);
                            }
                            commandList->commitBarriers();
                        }
                    }
                    impl->RecordedSamples = dispatchCount;
                    if (dispatchCount && impl->TraceTimer && !impl->TimerSubmission)
                    {
                        commandList->endTimerQuery(impl->TraceTimer);
                        impl->TimerPixels = uint64_t(band.Rows) * impl->NativeOutputs.Width;
                    }
                }
                catch (const std::exception& exception)
                {
                    impl->RecordedSamples = 0;
                    if (impl->bOfflineControlEnabled)
                    {
                        impl->MarkOfflineFailed(
                            "Offline reference trace recording failed: " +
                            std::string(exception.what()));
                    }
                    throw;
                }
                catch (...)
                {
                    impl->RecordedSamples = 0;
                    if (impl->bOfflineControlEnabled)
                    {
                        impl->MarkOfflineFailed(
                            "Offline reference trace recording failed with an unknown error");
                    }
                    throw;
                }
            });
        }
        else
        {
            mImpl->RecordedSamples = 0;
        }

        TextureHandle previewComposite =
            mImpl->GraphOutputs.AccumulatedRadiance;
        struct DenoisePassData { std::array<TextureHandle, 3> Inputs; TextureHandle Output; };
        renderGraph.AddPass<DenoisePassData>("OfflineDenoiseTransfer", RenderGraphQueue::Graphics,
            [outputs = mImpl->GraphOutputs](RenderGraphBuilder& builder, DenoisePassData& data)
            {
                data.Inputs = {builder.CopySource(outputs.AccumulatedRadiance),
                    builder.CopySource(outputs.Albedo), builder.CopySource(outputs.WorldNormal)};
                data.Output = builder.CopyDest(outputs.DenoisedRadiance);
                builder.SetSideEffect();
            },
            [impl = mImpl.get()](RenderGraphContext& context, const DenoisePassData& data)
            {
                auto* commandList = context.GetCommandList();
                if (impl->bNeedSnapshot && !impl->SnapshotSubmission && !impl->Denoiser.Busy())
                {
                    impl->Snapshot = {};
                    impl->Snapshot.Generation = impl->Schedule.Generation();
                    impl->Snapshot.Samples = impl->Schedule.Completed();
                    impl->Snapshot.Width = impl->NativeOutputs.Width;
                    impl->Snapshot.Height = impl->NativeOutputs.Height;
                    for (size_t index = 0; index < 3; ++index)
                    {
                        auto* source = context.GetTexture(data.Inputs[index], RenderGraphAccess::Read);
                        nvrhi::TextureDesc desc;
                        desc.width = impl->Snapshot.Width;
                        desc.height = impl->Snapshot.Height;
                        desc.format = source->getDesc().format;
                        desc.debugName = "Offline OIDN snapshot";
                        impl->DenoiseStaging[index] = impl->Device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read);
                        if (!impl->DenoiseStaging[index]) throw std::runtime_error("OIDN staging allocation failed");
                        const auto slice = nvrhi::TextureSlice().setSize(desc.width, desc.height, 1);
                        commandList->copyTexture(impl->DenoiseStaging[index], slice, source, slice);
                    }
                    impl->bSnapshotRecorded = true;
                }
                if (impl->PendingDenoisedUpload &&
                    impl->PendingDenoisedUpload->Generation == impl->Schedule.Generation())
                {
                    const auto& snapshot = *impl->PendingDenoisedUpload;
                    commandList->writeTexture(context.GetTexture(data.Output, RenderGraphAccess::Write),
                        0, 0, snapshot.Output.data(), size_t(snapshot.Width) * 4 * sizeof(float));
                    impl->bDenoiseUploadRecorded = true;
                }
            });
        if (mImpl->OfflineSettings.bDenoise && mImpl->DenoisedGeneration == mImpl->Schedule.Generation() &&
            mImpl->OfflineSettings.PreviewAov == OfflineViewportAov::DenoisedBeauty)
            previewComposite = mImpl->GraphOutputs.DenoisedRadiance;
        if (mImpl->bOfflineControlEnabled &&
            mImpl->OfflineSettings.PreviewAov != OfflineViewportAov::Beauty &&
            mImpl->OfflineSettings.PreviewAov != OfflineViewportAov::DenoisedBeauty)
        {
            previewComposite = mImpl->AddPreviewPass(
                renderGraph,
                mImpl->GetGraphAovTexture(
                    mImpl->OfflineSettings.PreviewAov),
                mImpl->OfflineSettings.PreviewAov,
                false);
        }

        const bool bToneMappedPngRequested = std::ranges::any_of(
            mImpl->PendingExports,
            [](const ReferenceExportRequest& request)
            {
                return request.Format ==
                    OfflineViewportOutputFormat::ToneMappedPng;
            });
        TextureHandle toneMappedPng;
        TextureHandle denoisedToneMappedPng;
        if (bToneMappedPngRequested)
        {
            toneMappedPng = mImpl->AddPreviewPass(
                renderGraph,
                mImpl->GraphOutputs.AccumulatedRadiance,
                OfflineViewportAov::Beauty,
                true);
            if (std::ranges::any_of(mImpl->PendingExports, [](const ReferenceExportRequest& request)
                { return request.Aov == OfflineViewportAov::DenoisedBeauty && request.Format == OfflineViewportOutputFormat::ToneMappedPng; }))
                denoisedToneMappedPng = mImpl->AddPreviewPass(renderGraph,
                    mImpl->GraphOutputs.DenoisedRadiance, OfflineViewportAov::DenoisedBeauty, true);
        }

        if (!mImpl->PendingExports.empty())
        {
            const std::vector<ReferenceExportRequest> requests =
                mImpl->PendingExports;
            renderGraph.AddPass<ReferenceExportPassData>(
                "ReferencePathTracerExport",
                RenderGraphQueue::Copy,
                [impl = mImpl.get(), requests, toneMappedPng, denoisedToneMappedPng](
                    RenderGraphBuilder& builder,
                    ReferenceExportPassData& data)
                {
                    data.Sources.reserve(requests.size());
                    data.Handles.reserve(requests.size());
                    for (const ReferenceExportRequest& request : requests)
                    {
                        data.Sources.push_back(builder.CopySource(
                            request.Format ==
                                    OfflineViewportOutputFormat::ToneMappedPng
                                ? (request.Aov == OfflineViewportAov::DenoisedBeauty ? denoisedToneMappedPng : toneMappedPng)
                                : impl->GetGraphAovTexture(request.Aov)));
                        data.Handles.push_back(request.Handle);
                    }
                    builder.SetSideEffect();
                },
                [impl = mImpl.get(), requests](
                    RenderGraphContext& context,
                    const ReferenceExportPassData& data)
                {
                    for (size_t index = 0; index < data.Handles.size(); ++index)
                    {
                        const TextureReadbackHandle handle = data.Handles[index];
                        if (!impl->ReadbackQueue->IsValid(handle) ||
                            impl->ReadbackQueue->GetResult(handle).Status !=
                                TextureReadbackStatus::PendingSubmission)
                        {
                            continue;
                        }
                        if (requests[index].Generation != impl->Schedule.Generation())
                        {
                            impl->ReadbackQueue->CancelPending(handle);
                            continue;
                        }
                        try
                        {
                            impl->ReadbackQueue->RecordReserved(
                                handle,
                                context.GetCommandList(),
                                context.GetTexture(
                                    data.Sources[index],
                                    RenderGraphAccess::Read));
                        }
                        catch (const std::exception& exception)
                        {
                            try
                            {
                                if (impl->ReadbackQueue->IsValid(handle))
                                    impl->ReadbackQueue->CancelPending(handle);
                            }
                            catch (...)
                            {
                            }
                            impl->MarkOfflineFailed(
                                "Offline reference export recording failed: " +
                                std::string(exception.what()));
                            throw;
                        }
                    }
                });
            mImpl->ScheduledExports.insert(
                mImpl->ScheduledExports.end(),
                requests.begin(),
                requests.end());
            mImpl->PendingExports.clear();
        }

        return {
            .CompositeRadiance = previewComposite,
            .DiffuseIndirect = mImpl->GraphOutputs.IndirectDiffuse,
            .DirectDiffuse = mImpl->GraphOutputs.DirectDiffuse,
            .HitDistance = mImpl->GraphOutputs.ViewDepth,
            .HistoryLength = mImpl->GraphOutputs.SampleCount,
        };
    }

    void ReferencePathTracer::ResetHistory(
        GlobalIlluminationHistoryResetReason reason)
    {
        mImpl->ResetAccumulation(reason);
        if (mImpl->bOfflineControlEnabled &&
            mImpl->OfflineState != OfflineViewportSessionState::Cancelled &&
            mImpl->OfflineState != OfflineViewportSessionState::Failed)
        {
            mImpl->bRestartRequired = false;
            mImpl->OfflineError.clear();
            mImpl->ResetOfflineProgressClock();
            mImpl->SetOfflineState(OfflineViewportSessionState::Rendering);
        }
    }

    bool ReferencePathTracer::RequiresContinuousUpdate() const noexcept
    {
        if (mImpl->bGraphRebuildRequested) return true;
        if (mImpl->bInteractivePreview || mImpl->Schedule.InFlight() || mImpl->Denoiser.Busy() ||
            mImpl->SnapshotSubmission || mImpl->DenoiseUploadSubmission || mImpl->bNeedSnapshot || mImpl->PendingDenoisedUpload)
            return true;
        if (!mImpl->PendingExports.empty() || !mImpl->ScheduledExports.empty())
            return true;
        if (!mImpl->bOfflineControlEnabled)
            return true;
        return mImpl->OfflineState == OfflineViewportSessionState::Rendering &&
            !mImpl->bRestartRequired &&
            mImpl->AccumulatedSamples < mImpl->OfflineSettings.TargetSamples;
    }

    GlobalIlluminationStats ReferencePathTracer::GetStats() const
    {
        GlobalIlluminationStats stats = mImpl->Stats;
        stats.AccumulatedSamples = mImpl->AccumulatedSamples;
        if (mImpl->bOfflineControlEnabled)
        {
            switch (mImpl->OfflineState)
            {
            case OfflineViewportSessionState::Idle:
                stats.Status = "Offline reference idle";
                break;
            case OfflineViewportSessionState::Rendering:
                stats.Status = "Offline reference rendering";
                break;
            case OfflineViewportSessionState::Paused:
                stats.Status = "Offline reference paused";
                break;
            case OfflineViewportSessionState::Complete:
                stats.Status = "Offline reference complete";
                break;
            case OfflineViewportSessionState::Cancelled:
                stats.Status = "Offline reference cancelled";
                break;
            case OfflineViewportSessionState::Failed:
                stats.Status = "Offline reference failed";
                break;
            }
        }
        return stats;
    }

    const ReferencePathTracerOutputs& ReferencePathTracer::GetOutputs() const noexcept
    {
        return mImpl->GraphOutputs;
    }

    const ReferencePathTracerNativeOutputs&
        ReferencePathTracer::GetNativeOutputs() const noexcept
    {
        return mImpl->NativeOutputs;
    }

    bool ReferencePathTracer::ConsumeGraphRebuildRequest() noexcept
    {
        return std::exchange(mImpl->bGraphRebuildRequested, false);
    }

    void ReferencePathTracer::UpdateImportedResources(RenderGraph& renderGraph)
    {
        (void)renderGraph;
    }

    void ReferencePathTracer::NotifyFrameSubmitted(uint64_t submission) noexcept
    {
        if (mImpl->bSnapshotRecorded)
        {
            mImpl->SnapshotSubmission = submission;
            mImpl->bSnapshotRecorded = false;
            mImpl->bNeedSnapshot = false;
            mImpl->LastSnapshotGeneration = mImpl->Snapshot.Generation;
            mImpl->LastSnapshotSamples = mImpl->Snapshot.Samples;
            mImpl->LastDenoiseAt = Impl::Clock::now();
        }
        if (mImpl->bDenoiseUploadRecorded && mImpl->PendingDenoisedUpload)
        {
            mImpl->UploadedGeneration = mImpl->PendingDenoisedUpload->Generation;
            mImpl->UploadedSamples = mImpl->PendingDenoisedUpload->Samples;
            mImpl->DenoiseUploadSubmission = submission;
            mImpl->PendingDenoisedUpload.reset();
            mImpl->bDenoiseUploadRecorded = false;
            mImpl->bGraphRebuildRequested = true;
        }
        mImpl->LastSubmittedSubmission = std::max(
            mImpl->LastSubmittedSubmission,
            submission);
        for (const ReferenceExportRequest& request : mImpl->ScheduledExports)
        {
            try
            {
                if (mImpl->ReadbackQueue->IsValid(request.Handle) &&
                    mImpl->ReadbackQueue->GetResult(request.Handle).Status ==
                        TextureReadbackStatus::PendingSubmission)
                {
                    mImpl->ReadbackQueue->NotifySubmitted(
                        request.Handle,
                        submission);
                }
            }
            catch (const std::exception& exception)
            {
                HE_CORE_ERROR(
                    "Failed to submit offline reference export: {0}",
                    exception.what());
                try
                {
                    if (mImpl->ReadbackQueue->IsValid(request.Handle) &&
                        mImpl->ReadbackQueue->GetResult(request.Handle).Status ==
                            TextureReadbackStatus::PendingSubmission)
                    {
                        mImpl->ReadbackQueue->CancelPending(request.Handle);
                    }
                }
                catch (...)
                {
                }
            }
        }
        if (!mImpl->ScheduledExports.empty())
            mImpl->bGraphRebuildRequested = true;
        mImpl->ScheduledExports.clear();
        if (mImpl->RecordedSamples > 0)
        {
            mImpl->Schedule.Submit(submission);
            if (mImpl->TraceTimer && !mImpl->TimerSubmission)
                mImpl->TimerSubmission = submission;
            if (mImpl->bRecordedClear)
                mImpl->bClearAccumulation = false;
            mImpl->bRecordedClear = false;
            mImpl->RecordedSamples = 0;
        }
    }

    void ReferencePathTracer::AbortFrame() noexcept
    {
        mImpl->bSnapshotRecorded = false;
        mImpl->bDenoiseUploadRecorded = false;
        mImpl->Schedule.Abort();
        mImpl->bRecordedClear = false;
        if (mImpl->TraceTimer && !mImpl->TimerSubmission)
            mImpl->Device->resetTimerQuery(mImpl->TraceTimer);
        mImpl->RecordedSamples = 0;
        for (const ReferenceExportRequest& request : mImpl->ScheduledExports)
        {
            try
            {
                if (mImpl->ReadbackQueue->IsValid(request.Handle) &&
                    mImpl->ReadbackQueue->GetResult(request.Handle).Status ==
                        TextureReadbackStatus::PendingSubmission)
                {
                    mImpl->ReadbackQueue->CancelPending(request.Handle);
                }
            }
            catch (const std::exception& exception)
            {
                HE_CORE_ERROR(
                    "Failed to abort offline reference export: {0}",
                    exception.what());
            }
        }
        if (!mImpl->ScheduledExports.empty())
            mImpl->bGraphRebuildRequested = true;
        mImpl->ScheduledExports.clear();
    }

    void ReferencePathTracer::Collect(uint64_t completedSubmission)
    {
        if (mImpl->DenoiseUploadSubmission && mImpl->DenoiseUploadSubmission <= completedSubmission)
        {
            if (mImpl->UploadedGeneration == mImpl->Schedule.Generation() &&
                mImpl->OfflineState != OfflineViewportSessionState::Cancelled)
            {
                mImpl->DenoisedGeneration = mImpl->UploadedGeneration;
                mImpl->DenoisedSamples = mImpl->UploadedSamples;
                mImpl->bGraphRebuildRequested = true;
            }
            mImpl->DenoiseUploadSubmission = 0;
        }
        if (auto result = mImpl->Denoiser.Poll())
        {
            mImpl->DenoiseMilliseconds = result->Milliseconds;
            mImpl->DenoiserStatus = result->Error.empty() ? "OIDN CPU ready" : result->Error;
            if (result->Error.empty() && mImpl->OfflineState != OfflineViewportSessionState::Cancelled &&
                result->Generation == mImpl->Schedule.Generation() &&
                (mImpl->DenoisedGeneration != result->Generation || result->Samples >= mImpl->DenoisedSamples))
                mImpl->PendingDenoisedUpload = std::move(result);
        }
        if (mImpl->SnapshotSubmission && mImpl->SnapshotSubmission <= completedSubmission)
        {
            if (mImpl->Snapshot.Generation == mImpl->Schedule.Generation() &&
                mImpl->OfflineState != OfflineViewportSessionState::Cancelled)
            {
                try
                {
                    const std::array<std::vector<float>*, 3> destinations = {
                        &mImpl->Snapshot.Color, &mImpl->Snapshot.Albedo, &mImpl->Snapshot.Normal};
                    for (size_t index = 0; index < 3; ++index)
                    {
                        const uint32_t width = mImpl->Snapshot.Width, height = mImpl->Snapshot.Height;
                        auto& destination = *destinations[index];
                        destination.resize(size_t(width) * height * 4);
                        size_t rowPitch = 0;
                        const auto slice = nvrhi::TextureSlice().setSize(width, height, 1);
                        const auto* mapped = static_cast<const uint8_t*>(mImpl->Device->mapStagingTexture(
                            mImpl->DenoiseStaging[index], slice, nvrhi::CpuAccessMode::Read, &rowPitch));
                        if (!mapped) throw std::runtime_error("OIDN completed snapshot map failed");
                        for (uint32_t y = 0; y < height; ++y)
                            std::memcpy(destination.data() + size_t(y) * width * 4,
                                mapped + y * rowPitch, size_t(width) * 4 * sizeof(float));
                        mImpl->Device->unmapStagingTexture(mImpl->DenoiseStaging[index]);
                    }
                    mImpl->Denoiser.Submit(std::move(mImpl->Snapshot));
                    mImpl->DenoiserStatus = "OIDN CPU working";
                }
                catch (const std::exception& error) { mImpl->DenoiserStatus = error.what(); }
            }
            mImpl->DenoiseStaging = {};
            mImpl->SnapshotSubmission = 0;
        }
        if (mImpl->TimerSubmission && mImpl->TimerSubmission <= completedSubmission &&
            mImpl->Device->pollTimerQuery(mImpl->TraceTimer))
        {
            const double milliseconds = 1000.0 * mImpl->Device->getTimerQueryTime(mImpl->TraceTimer);
            if (std::isfinite(milliseconds) && milliseconds > 0 && mImpl->TimerPixels)
            {
                mImpl->LastTraceMilliseconds = milliseconds;
                // Fast response to slow work; cautious recovery to larger bands.
                mImpl->TraceMillisecondsPerPixel = std::max(milliseconds / mImpl->TimerPixels,
                    mImpl->TraceMillisecondsPerPixel * 0.95);
            }
            mImpl->Device->resetTimerQuery(mImpl->TraceTimer);
            mImpl->TimerSubmission = 0;
        }
        if (mImpl->Schedule.Collect(completedSubmission))
        {
            mImpl->AccumulatedSamples = mImpl->Schedule.Completed();
            mImpl->Stats.AccumulatedSamples = mImpl->AccumulatedSamples;
            if (mImpl->bOfflineControlEnabled && mImpl->OfflineState == OfflineViewportSessionState::Rendering &&
                mImpl->AccumulatedSamples >= mImpl->OfflineSettings.TargetSamples)
            {
                mImpl->SetOfflineState(OfflineViewportSessionState::Complete);
                mImpl->bGraphRebuildRequested = true;
            }
        }
        const bool bNewSnapshot = mImpl->LastSnapshotGeneration != mImpl->Schedule.Generation() ||
            mImpl->LastSnapshotSamples < mImpl->Schedule.Completed();
        mImpl->bNeedSnapshot = mImpl->OfflineSettings.bDenoise && ProgressiveDenoiser::Available() &&
            !mImpl->bInteractivePreview && !mImpl->Denoiser.Busy() && !mImpl->SnapshotSubmission &&
            !mImpl->PendingDenoisedUpload && !mImpl->DenoiseUploadSubmission && !mImpl->Schedule.InFlight() && bNewSnapshot &&
            mImpl->Schedule.Completed() > 0 && mImpl->Schedule.Average() == double(mImpl->Schedule.Completed()) &&
            mImpl->OfflineState != OfflineViewportSessionState::Cancelled &&
            std::chrono::duration<double, std::milli>(Impl::Clock::now() - mImpl->LastDenoiseAt).count() >=
                mImpl->OfflineSettings.DenoiseIntervalMilliseconds;
        if (uint64_t(mImpl->NativeOutputs.Width) * mImpl->NativeOutputs.Height > 4194304u)
        {
            mImpl->bNeedSnapshot = false;
            mImpl->DenoiserStatus = "OIDN snapshot exceeds the 4 megapixel memory limit";
        }
        mImpl->ReadbackQueue->Collect(completedSubmission);
        std::erase_if(mImpl->RetiredSceneBindings,
            [completedSubmission](const Impl::RetiredBindingSet& retired)
            {
                return retired.Submission <= completedSubmission;
            });
        std::erase_if(mImpl->RetiredEnvironmentBindings,
            [completedSubmission](const Impl::RetiredBindingSet& retired)
            {
                return retired.Submission <= completedSubmission;
            });
        std::erase_if(mImpl->RetiredOutputSets,
            [completedSubmission](const Impl::RetiredOutputs& retired)
            {
                return retired.Submission <= completedSubmission;
            });
    }

    void ReferencePathTracer::Clear(uint64_t completedSubmission)
    {
        Collect(completedSubmission);
        mImpl->ReadbackQueue->Clear();
        mImpl->PendingExports.clear();
        mImpl->ScheduledExports.clear();
        mImpl->RetireOutputs();
        mImpl->GraphOutputs = {};
        mImpl->HistoryTracker.Reset(
            GlobalIlluminationHistoryResetReason::Explicit);
        mImpl->AccumulatedSamples = 0;
        mImpl->Schedule.Reset(mImpl->NativeOutputs.Height);
        mImpl->bClearAccumulation = true;
        mImpl->RecordedSamples = 0;
        mImpl->LastCameraFingerprint = 0;
        mImpl->LastSceneStateFingerprint = 0;
        mImpl->bHasPreparedFingerprint = false;
        mImpl->bSceneUnavailable = false;
        mImpl->bRestartRequired = false;
        mImpl->bSceneInvalidated = false;
        mImpl->bCameraInvalidated = false;
        mImpl->SetOfflineState(OfflineViewportSessionState::Idle);
        mImpl->OfflineError.clear();
        mImpl->ElapsedRenderingSeconds = 0.0;
        mImpl->bFramePrepared = false;
        mImpl->bGraphRebuildRequested = true;
        Collect(completedSubmission);
    }
}
