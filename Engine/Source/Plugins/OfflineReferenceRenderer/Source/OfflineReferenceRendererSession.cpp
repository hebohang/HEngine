#include "Hepch.h"

#include "Plugins/OfflineReferenceRenderer/Source/OfflineReferenceRendererSession.h"
#include "Plugins/OfflineReferenceRenderer/Source/OfflineReferenceRendererSettingsJson.h"

#include "Runtime/Graphics/RHI/ShaderManager.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <nvrhi/utils.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
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
        constexpr std::array<OfflineViewportAov, 9> ReferenceAovs = {
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
        const uint64_t remaining =
            static_cast<uint64_t>(settings.TargetSamples) -
            accumulatedSamples;
        return static_cast<uint32_t>(std::min<uint64_t>(
            remaining,
            settings.SamplesPerFrame));
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
        case ReferenceOfflineControlCommand::SamplesSubmitted:
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
            aov != OfflineViewportAov::Beauty)
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
            bOfflineControlEnabled = desc.bOfflineViewportSession;
            ResolvedSeed = OfflineSettings.Seed;
            CreatePipeline();
            if (bOfflineControlEnabled)
                SetOfflineState(OfflineViewportSessionState::Rendering);
        }

        using Clock = std::chrono::steady_clock;

        void SetOfflineState(OfflineViewportSessionState state) noexcept
        {
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
            if (!RayGenerationShader || !MissShader || !ClosestHitShader)
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
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(6));
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
                    .setClosestHitShader(ClosestHitShader))
                .addBindingLayout(FrameBindingLayout)
                .addBindingLayout(SceneBindingLayout)
                .addBindingLayout(OutputBindingLayout)
                .addBindingLayout(EnvironmentBindingLayout)
                .setMaxPayloadSize(64)
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
                width, height, nvrhi::Format::RGBA16_FLOAT,
                "Reference PT Albedo");
            NativeOutputs.WorldNormal = CreateOutputTexture(
                width, height, nvrhi::Format::RGBA16_FLOAT,
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
                    6, sceneResources.EmissiveTriangles));
            SceneBindingSet = Device->createBindingSet(
                sceneDesc,
                SceneBindingLayout);
            if (!SceneBindingSet)
                throw std::runtime_error("NVRHI failed to bind reference path tracer scene tables");
            SceneResourceGeneration = sceneResources.Generation;
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
            mImpl->OfflineSettings.PreviewAov != normalized.PreviewAov;
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
        else if (mImpl->AccumulatedSamples >=
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
        const bool bSceneLinear = aov == OfflineViewportAov::Beauty ||
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

    TextureReadbackHandle ReferencePathTracer::RequestExport(
        OfflineViewportAov aov,
        std::filesystem::path outputPath,
        OfflineViewportOutputFormat format)
    {
        Detail::ValidateReferenceOfflineExportRequest(aov, format);

        nvrhi::Format nativeFormat = nvrhi::Format::UNKNOWN;
        switch (aov)
        {
        case OfflineViewportAov::Beauty:
        case OfflineViewportAov::DirectDiffuse:
        case OfflineViewportAov::IndirectDiffuse:
            nativeFormat = nvrhi::Format::RGBA32_FLOAT;
            break;
        case OfflineViewportAov::Albedo:
        case OfflineViewportAov::WorldNormal:
            nativeFormat = nvrhi::Format::RGBA16_FLOAT;
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
        if (!mImpl->Scene->IsReady())
            throw std::logic_error("ReferencePathTracer requires a prepared RayTracingScene");

        Collect(frame.CompletedSubmission);
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
                    mImpl->SetOfflineState(
                        OfflineViewportSessionState::Rendering);
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
            mImpl->Stats.RaysPerPixel = mImpl->ResolveDispatchCount();
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
            const auto [width, height] = ResolveOfflineDimensions(
                frame.Width,
                frame.Height,
                mImpl->OfflineSettings);
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
        if (!inputs.SceneTlas || !inputs.GeometryBuffer ||
            !inputs.MaterialBuffer || !inputs.RayTracingVertexBuffer ||
            !inputs.RayTracingIndexBuffer ||
            !inputs.RayTracingInstanceBuffer || !inputs.LightBuffer)
        {
            throw std::invalid_argument("ReferencePathTracer inputs are incomplete");
        }

        mImpl->GraphOutputs = {
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
            renderGraph.AddPass<ReferenceTracePassData>(
            "ReferencePathTrace",
            RenderGraphQueue::Graphics,
            [inputs, outputs = mImpl->GraphOutputs](
                RenderGraphBuilder& builder,
                ReferenceTracePassData& data)
            {
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
                    const uint32_t dispatchCount = impl->ResolveDispatchCount();
                    impl->RecordedSamples = 0;
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
                        const uint64_t sampleIndex = impl->AccumulatedSamples +
                            dispatchIndex;
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
                                impl->NativeOutputs.Height));
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
        if (mImpl->bOfflineControlEnabled &&
            mImpl->OfflineSettings.PreviewAov != OfflineViewportAov::Beauty)
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
        if (bToneMappedPngRequested)
        {
            toneMappedPng = mImpl->AddPreviewPass(
                renderGraph,
                mImpl->GraphOutputs.AccumulatedRadiance,
                OfflineViewportAov::Beauty,
                true);
        }

        if (!mImpl->PendingExports.empty())
        {
            const std::vector<ReferenceExportRequest> requests =
                mImpl->PendingExports;
            renderGraph.AddPass<ReferenceExportPassData>(
                "ReferencePathTracerExport",
                RenderGraphQueue::Copy,
                [impl = mImpl.get(), requests, toneMappedPng](
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
                                ? toneMappedPng
                                : impl->GetGraphAovTexture(request.Aov)));
                        data.Handles.push_back(request.Handle);
                    }
                    builder.SetSideEffect();
                },
                [impl = mImpl.get()](
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
            mImpl->AccumulatedSamples += mImpl->RecordedSamples;
            mImpl->Stats.AccumulatedSamples = mImpl->AccumulatedSamples;
            mImpl->RecordedSamples = 0;
            if (mImpl->bOfflineControlEnabled)
            {
                const OfflineViewportSessionState nextState =
                    Detail::ResolveReferenceOfflineControlState(
                        mImpl->OfflineState,
                        Detail::ReferenceOfflineControlCommand::SamplesSubmitted,
                        mImpl->AccumulatedSamples,
                        mImpl->OfflineSettings.TargetSamples,
                        mImpl->bRestartRequired);
                if (nextState != mImpl->OfflineState)
                    mImpl->SetOfflineState(nextState);
            }
        }
    }

    void ReferencePathTracer::AbortFrame() noexcept
    {
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
        mImpl->RecordedSamples = 0;
        mImpl->LastCameraFingerprint = 0;
        mImpl->LastSceneStateFingerprint = 0;
        mImpl->bHasPreparedFingerprint = false;
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
