#include "Runtime/Core/Log/Log.h"
#include "Runtime/Core/Modules/ModuleManager.h"
#include "Runtime/EcsFramework/Serializer/SceneSerializer.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Level/Level.h"
#include "Runtime/EcsFramework/System/Render/RenderSystem3D.h"
#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/System/Render/RenderWorldExtractor.h"
#include "Runtime/Core/Window/Window.h"
#include "Runtime/Graphics/Material/Material.h"
#include "Runtime/Graphics/Material/MaterialShaderManifest.h"
#include "Runtime/Graphics/GlobalIllumination/DepthPyramidPass.h"
#if HE_WITH_OFFLINE_REFERENCE_RENDERER_SMOKE
#include "Plugins/OfflineReferenceRenderer/Source/OfflineReferenceRendererSession.h"
#endif
#include "Runtime/Graphics/GlobalIllumination/RealtimeRayTracedGlobalIllumination.h"
#include "Runtime/Graphics/GlobalIllumination/SceneSurfaceDataPass.h"
#include "Runtime/Graphics/GlobalIllumination/SvgfDenoiser.h"
#include "Runtime/Graphics/Mesh/SubMesh.h"
#include "Runtime/Graphics/Mesh/Mesh.h"
#include "Runtime/Graphics/RayTracing/RayTracingScene.h"
#include "Runtime/Graphics/RHI/RenderDevice.h"
#include "Runtime/Graphics/RHI/TextureReadback.h"
#include "Runtime/Graphics/RenderGraph/RenderGraph.h"
#include "Runtime/Graphics/RenderGraph/RenderGraphResourcePool.h"
#include "Runtime/Graphics/Renderer/SceneRenderer.h"
#include "Runtime/Graphics/Renderer2D.h"
#include "Runtime/Graphics/VFX/EffekseerRuntime.h"
#include "Runtime/Graphics/VFX/EffekseerRenderGraphPass.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"

#include <HEngine/ProjectBuildConfig.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace HEngine
{
    void RegisterStaticPluginsForRenderSmoke(PluginManager& pluginManager);
}

namespace
{
    constexpr uint32_t c_InitialWidth = 640;
    constexpr uint32_t c_InitialHeight = 360;
    constexpr uint32_t c_ResizedWidth = 800;
    constexpr uint32_t c_ResizedHeight = 450;
    constexpr uint32_t c_FramesPerSize = 6;

    // Independent raster fixture: no user scene or offline renderer dependency.
    void RunMaterialDepthShadowSmoke(HEngine::RenderDevice& device)
    {
        constexpr uint32_t size = 256;
        const std::string runName = std::string(HEngine::ToString(device.GetBackend())) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto directory = std::filesystem::path(HE_DEFAULT_PROJECT_DIR).parent_path() /
            "out/agent/captures/material-depth-shadow" / runName;
        std::filesystem::create_directories(directory);
        const std::string texturePath = "Saved/RenderingTests/MaterialDepthShadow/" + runName + "/alpha.tga";
        const auto textureFullPath = HEngine::ConfigManager::GetProjectFullPath(texturePath);
        std::filesystem::create_directories(std::filesystem::path(textureFullPath).parent_path());
        {
            std::ofstream stream(textureFullPath, std::ios::binary);
            std::array<uint8_t, 18> header{};
            header[2] = 2; header[12] = 2; header[14] = 2; header[16] = 32; header[17] = 0x28;
            stream.write(reinterpret_cast<const char*>(header.data()), header.size());
            // Both rows identical: left opaque, right transparent; no origin ambiguity.
            for (uint8_t alpha : {uint8_t(255), uint8_t(0), uint8_t(255), uint8_t(0)})
            {
                const std::array<uint8_t, 4> pixel{255,255,255,alpha};
                stream.write(reinterpret_cast<const char*>(pixel.data()), pixel.size());
            }
            if (!stream) throw std::runtime_error("Depth/shadow alpha texture write failed");
        }
        HEngine::RenderWorld world;
        world.Camera.Position = {0,0,3};
        world.Camera.NearPlane = .1f; world.Camera.FarPlane = 6.f;
        world.Camera.View = glm::lookAt(world.Camera.Position, glm::vec3(0), glm::vec3(0,1,0));
        world.Camera.Projection = glm::perspective(glm::radians(45.f), 1.f, .1f, 6.f);
        world.Camera.ViewProjection = world.Camera.Projection * world.Camera.View;
        world.Camera.NonJitteredViewProjection = world.Camera.PreviousViewProjection =
            world.Camera.PreviousNonJitteredViewProjection = world.Camera.ViewProjection;
        world.DirectionalLights.push_back({.Direction = {0,0,-1}, .Intensity = 1,
            .Color = {1,1,1}, .bCastShadows = true, .CascadeCount = 1,
            .ShadowMapSize = size, .ShadowDistance = 6});
        HEngine::RenderObject object;
        object.EntityId = 1; object.MeshAssetPath = "EngineOwnedMaterialDepthShadowQuad";
        auto materialResource = HEngine::CreateRef<HEngine::Material>();
        object.Sections.push_back({.MaterialResource = materialResource,
            .MaterialInstance = materialResource->BuildRenderSnapshot(),
            .Path = HEngine::RenderPath::CpuOpaque,
            .AlphaMode = HEngine::RenderMaterialAlphaMode::Mask, .AlphaCutoff = .5f, .bDoubleSided = true});
        world.Objects.push_back(std::move(object));
        auto& section = world.Objects[0].Sections[0];
        auto& material = section.MaterialInstance;
        material.AlphaMode = HEngine::RenderMaterialAlphaMode::Mask;
        material.AlphaCutoff = .5f; material.bDoubleSided = true; material.BaseColor = glm::vec4(1);
        material.LegacyTextures[0] = {.AssetPath = texturePath, .bEnabled = true};
        auto& sampling = material.LegacyTextures[0].Sampling;
        sampling.magFilter = sampling.minFilter = 9728;
        sampling.wrapS = sampling.wrapT = 33071;
        const char* shaderFormat = device.GetBackend() == HEngine::RenderBackend::Vulkan ? "SPIRV" : "DXIL";
        HEngine::SceneRenderer renderer({.Device = device.GetDevice(), .FramesInFlight = device.GetCapabilities().FramesInFlight,
            .ShaderCacheDirectory = HEngine::ConfigManager::GetEngineGeneratedContentFullPath(std::string("Shaders/CACHES/") + shaderFormat),
            .AssetRootDirectory = HEngine::ConfigManager::GetInstance().GetRootFolder(),
            .EngineAssetRootDirectory = HEngine::ConfigManager::GetInstance().GetEngineContentFolder(),
            .Backend = device.GetBackend(), .Capabilities = device.GetCapabilities(), .bShadowsEnabled = true});
        renderer.ConfigureOffscreenOutput(size, size, "Engine material depth/shadow fixture");
        HEngine::TextureReadbackQueue readback(device.GetDevice());
        std::cout << "MATERIAL_DEPTH_SHADOW_OUTPUT: " << directory.string() << std::endl;
        const auto capture = [&](HEngine::FrameCaptureSource source, const char* name, float vertexAlpha)
        {
            std::vector<HEngine::StaticVertex> vertices;
            for (glm::vec3 position : {glm::vec3(-.9f,-.9f,0), glm::vec3(.9f,-.9f,0),
                                      glm::vec3(.9f,.9f,0), glm::vec3(-.9f,.9f,0)})
            {
                HEngine::StaticVertex vertex(position, {0,0,1}, {.25f,.25f}, {1,0,0}, {0,1,0});
                vertex.TexCoord1 = {.75f,.25f}; vertex.Color = {1,1,1,vertexAlpha};
                vertices.push_back(vertex);
            }
            section.SubMeshResource = HEngine::CreateRef<HEngine::SubMesh>(vertices, std::vector<uint32_t>{0,1,2,0,2,3});
            ++world.SceneGeneration; ++world.FrameNumber; ++material.Revision;
            const auto path = directory / (std::string(HEngine::FrameCaptureSourceName(source)) + "-" + name + ".raw");
            HEngine::TextureReadbackHandle handle;
            auto& frame = device.BeginFrame();
            try
            {
                HEngine::EffekseerRuntime::BeginHostFrame(frame.FrameIndex, frame.CompletedSubmissionInstance, frame.GraphicsCommandList);
                renderer.RecordFrame(world, {.CommandList = frame.GraphicsCommandList, .Width = size, .Height = size,
                    .FrameSlot = frame.FrameIndex, .CompletedSubmission = frame.CompletedSubmissionInstance,
                    .OutputMode = HEngine::SceneRendererOutputMode::OffscreenOnly,
                    .LinearHdrCaptureRecorder = [&](const HEngine::SceneRendererLinearHdrCaptureContext& context)
                    {
                        if (!context.SelectedNativeInput) throw HEngine::CaptureSourceUnavailable(source);
                        const auto& desc = context.SelectedNativeInput->getDesc();
                        if (desc.format != nvrhi::Format::D32 || desc.width != size || desc.height != size || desc.arraySize != 1)
                            throw std::runtime_error("Depth/shadow fixture requires a real 256x256 single-slice D32 attachment");
                        handle = readback.Record(context.CommandList, context.SelectedNativeInput,
                            {.OutputPath = path, .SourceSlice = nvrhi::TextureSlice().setArraySlice(0),
                             .OutputFormat = HEngine::TextureReadbackOutputFormat::Raw});
                    }, .CaptureSource = source});
                if (world.Objects[0].bVisible)
                {
                    const auto& stats = renderer.GetStats();
                    if (stats.DepthPrepassAlphaMaskDrawCount != 1 || stats.DirectionalShadowDrawCount != 1 ||
                        stats.DirectionalShadowCascadeCount != 1)
                        throw std::runtime_error("Depth/shadow fixture did not execute exactly one masked prepass and one shadow caster");
                }
                device.SubmitAndPresent(frame, false);
                HEngine::EffekseerRuntime::NotifyHostFrameSubmitted(frame.FrameIndex, frame.SubmissionInstance);
                renderer.NotifyFrameSubmitted(frame.FrameIndex, frame.SubmissionInstance);
                if (!handle) throw std::runtime_error("Depth/shadow capture callback did not record readback");
                readback.NotifySubmitted(handle, frame.SubmissionInstance);
            }
            catch (...)
            {
                if (frame.bSubmissionIssued)
                {
                    HEngine::EffekseerRuntime::NotifyHostFrameSubmitted(frame.FrameIndex, frame.SubmissionInstance);
                    renderer.NotifyFrameSubmitted(frame.FrameIndex, frame.SubmissionInstance);
                    if (handle && readback.GetResult(handle).Status == HEngine::TextureReadbackStatus::PendingSubmission)
                        readback.NotifySubmitted(handle, frame.SubmissionInstance);
                    device.WaitForIdle();
                }
                else
                {
                    device.AbortFrame(frame); HEngine::EffekseerRuntime::AbortHostFrame(frame.FrameIndex);
                    renderer.AbortFrame(frame.FrameIndex);
                    if (handle) readback.CancelPending(handle);
                }
                throw;
            }
            device.WaitForIdle();
            const auto started = std::chrono::steady_clock::now();
            while (true)
            {
                // WaitForIdle proves this actual submitted frame completed.
                readback.Collect(frame.SubmissionInstance);
                const auto result = readback.GetResult(handle);
                if (result.Status == HEngine::TextureReadbackStatus::Failed) throw std::runtime_error(result.Error);
                if (result.Status == HEngine::TextureReadbackStatus::Complete) break;
                if (std::chrono::steady_clock::now() - started > std::chrono::seconds(10))
                    throw std::runtime_error("Depth/shadow readback writer timeout");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            readback.Release(handle);
            if (std::filesystem::file_size(path) != size * size * sizeof(float))
                throw std::runtime_error("Depth/shadow raw readback byte count mismatch");
            std::vector<float> pixels(size * size);
            std::ifstream stream(path, std::ios::binary);
            stream.read(reinterpret_cast<char*>(pixels.data()), pixels.size() * sizeof(float));
            if (!stream || !std::all_of(pixels.begin(), pixels.end(), [](float v){return std::isfinite(v) && v >= 0 && v <= 1;}))
                throw std::runtime_error("Depth/shadow raw readback contains invalid depth");
            return pixels;
        };
        for (auto source : {HEngine::FrameCaptureSource::Depth, HEngine::FrameCaptureSource::DirectionalShadow})
        {
            sampling.texCoord = 0; sampling.scale = {1,1}; sampling.rotation = 0; sampling.offset = {0,0};
            world.Objects[0].bVisible = false;
            const auto empty = capture(source, "empty", 1);
            world.Objects[0].bVisible = true;
            const auto hit = capture(source, "uv0-hit", 1);
            size_t covered = 0;
            for (size_t i = 0; i < hit.size(); ++i) if (std::abs(hit[i] - empty[i]) > 1e-5f) ++covered;
            if (covered < 128) throw std::runtime_error("Depth/shadow opaque control has insufficient actual caster coverage");
            const auto compare = [&](const std::vector<float>& pixels, const std::vector<float>& expected, const char* name)
            {
                float error = 0;
                for (size_t i = 0; i < pixels.size(); ++i) error = std::max(error, std::abs(pixels[i] - expected[i]));
                if (error > 1e-6f) throw std::runtime_error(std::string("Depth/shadow sampling mismatch: ") + name + " error=" + std::to_string(error));
                std::cout << "MATERIAL_DEPTH_SHADOW_CHECK_PASS: source=" << HEngine::FrameCaptureSourceName(source)
                    << " case=" << name << " slice=0 covered=" << covered << " maxError=" << error << " tolerance=1e-6\n";
            };
            sampling.texCoord = 1;
            compare(capture(source, "uv1-hole", 1), empty, "uv1-hole");
            // (0.75,0.25) -> scale(2,1) -> rotate(pi/2) -> offset(0.5,-1.25) -> (0.25,0.25).
            sampling.scale = {2,1}; sampling.rotation = glm::radians(90.f); sampling.offset = {.5f,-1.25f};
            compare(capture(source, "uv1-transform-hit", 1), hit, "uv1-transform-hit");
            compare(capture(source, "vertex-alpha-hole", .25f), empty, "vertex-alpha-hole");
            compare(capture(source, "vertex-alpha-hit", .75f), hit, "vertex-alpha-hit");
            material.BaseColor.a = .25f;
            compare(capture(source, "factor-alpha-hole", 1), empty, "factor-alpha-hole");
            material.BaseColor.a = 1;
        }
        std::cout << "MATERIAL_DEPTH_SHADOW_SMOKE_PASS: actual D32 main-depth and directional-shadow slice0; "
                     "UV1/scale/rotation/offset/texture-alpha/vertex-alpha/factor-alpha; "
                     "NOT_VERIFIED=skinning,gpu-driven,multiple-cascades,RT\n";
    }

    [[nodiscard]] HEngine::RenderWorld BuildSyntheticWorld()
    {
        using HEngine::StaticVertex;
        const std::vector<StaticVertex> vertices = {
            StaticVertex(
                { -0.8f, -0.6f, 0.0f },
                { 0.0f, 0.0f, 1.0f },
                { 0.0f, 1.0f },
                { 1.0f, 0.0f, 0.0f },
                { 0.0f, 1.0f, 0.0f }),
            StaticVertex(
                { 0.0f, 0.75f, 0.0f },
                { 0.0f, 0.0f, 1.0f },
                { 0.5f, 0.0f },
                { 1.0f, 0.0f, 0.0f },
                { 0.0f, 1.0f, 0.0f }),
            StaticVertex(
                { 0.8f, -0.6f, 0.0f },
                { 0.0f, 0.0f, 1.0f },
                { 1.0f, 1.0f },
                { 1.0f, 0.0f, 0.0f },
                { 0.0f, 1.0f, 0.0f }),
        };
        // SceneRenderer uses frontCounterClockwise=true; keep this synthetic
        // triangle front-facing so pixel captures cover real material output.
        const std::vector<uint32_t> indices = { 0, 2, 1 };
        const std::vector<HEngine::SkinnedVertex> skinnedVertices = {
            {
                .Pos = { -0.4f, -0.3f, 0.0f },
                .Normal = { 0.0f, 0.0f, 1.0f },
                .TexCoord = { 0.0f, 1.0f },
                .Tangent = { 1.0f, 0.0f, 0.0f },
                .Bitangent = { 0.0f, 1.0f, 0.0f },
                .mBoneIDs = { 0, -1, -1, -1 },
                .mWeights = { 1.0f, 0.0f, 0.0f, 0.0f },
            },
            {
                .Pos = { 0.0f, 0.4f, 0.0f },
                .Normal = { 0.0f, 0.0f, 1.0f },
                .TexCoord = { 0.5f, 0.0f },
                .Tangent = { 1.0f, 0.0f, 0.0f },
                .Bitangent = { 0.0f, 1.0f, 0.0f },
                .mBoneIDs = { 0, -1, -1, -1 },
                .mWeights = { 1.0f, 0.0f, 0.0f, 0.0f },
            },
            {
                .Pos = { 0.4f, -0.3f, 0.0f },
                .Normal = { 0.0f, 0.0f, 1.0f },
                .TexCoord = { 1.0f, 1.0f },
                .Tangent = { 1.0f, 0.0f, 0.0f },
                .Bitangent = { 0.0f, 1.0f, 0.0f },
                .mBoneIDs = { 0, -1, -1, -1 },
                .mWeights = { 1.0f, 0.0f, 0.0f, 0.0f },
            },
        };

        HEngine::RenderWorld world;
        world.Camera.View = glm::mat4(1.0f);
        world.Camera.Projection = glm::mat4(1.0f);
        world.Camera.ViewProjection = glm::mat4(1.0f);
        world.Camera.PreviousViewProjection = glm::mat4(1.0f);
        world.Camera.Position = { 0.0f, 0.0f, 2.0f };
        world.Environment.Exposure = 1.0f;
        world.PostProcess.Exposure = 1.0f;
        world.Environment.HdrTexturePath =
            "Assets/TestData/Smoke/Renderer/VerdantRift.hdr";
        world.Environment.IblIntensity = 0.75f;
        world.DirectionalLights.push_back({
            .Direction = { 0.25f, -0.5f, -1.0f },
            .Intensity = 4.0f,
            .Color = { 1.0f, 0.75f, 0.55f },
            .bCastShadows = true,
            .CascadeCount = 4,
            .ShadowMapSize = 1024,
            .ShadowDistance = 20.0f,
            .CascadeLambda = 0.7f,
            .PcfRadius = 2.0f,
        });
        world.PointLights.push_back({
            .Position = { -0.5f, 0.2f, 1.5f },
            .Intensity = 7.0f,
            .Color = { 0.25f, 0.45f, 1.0f },
            .Radius = 5.0f,
        });

        const HEngine::Ref<HEngine::SubMesh> sharedMesh =
            HEngine::CreateRef<HEngine::SubMesh>(vertices, indices);
        const HEngine::Ref<HEngine::SubMesh> skinnedMesh =
            HEngine::CreateRef<HEngine::SubMesh>(skinnedVertices, indices);
        const HEngine::Ref<HEngine::Material> gpuMaterial =
            HEngine::CreateRef<HEngine::Material>();
        gpuMaterial->baseColorFactor = { 1.0f, 0.9f, 0.8f, 1.0f };
        gpuMaterial->SetTexturePath(
            HEngine::MaterialTextureSemantic::BaseColor,
            "Assets/Textures/DefaultTexture.png");
        const HEngine::Ref<HEngine::Material> cpuMaterial =
            HEngine::CreateRef<HEngine::Material>();
        cpuMaterial->baseColorFactor = { 0.8f, 0.9f, 1.0f, 1.0f };
        cpuMaterial->SetTexturePath(
            HEngine::MaterialTextureSemantic::BaseColor,
            "Assets/TestData/Smoke/Renderer/SilverAlbedo.png");
        const HEngine::Ref<HEngine::Material> customMaterialA =
            HEngine::CreateRef<HEngine::Material>("Assets/Material/FresnelRim.hmat");
        const HEngine::Ref<HEngine::Material> customMaterialB =
            HEngine::CreateRef<HEngine::Material>("Assets/Material/FresnelRim.hmat");
        const HEngine::Ref<HEngine::Material> customMaskMaterial =
            HEngine::CreateRef<HEngine::Material>("Assets/Material/ProceduralChecker.hmat");
        const HEngine::Ref<HEngine::Material> customTransparentMaterial =
            HEngine::CreateRef<HEngine::Material>("Assets/Material/TransparentPulse.hmat");
        for (HEngine::MaterialRuntimeProperty& property :
            customMaterialB->GetExposedProperties())
        {
            if (property.Decl.Name == "g_BaseColor")
                property.ColorValue = { 0.32f, 0.035f, 0.08f, 1.0f };
        }
        customMaterialB->ApplyRuntimePropertyChanges();

        HEngine::RenderObject gpuVisible;
        gpuVisible.EntityId = 1;
        gpuVisible.WorldTransform = glm::mat4(1.0f);
        gpuVisible.PreviousWorldTransform = gpuVisible.WorldTransform;
        gpuVisible.Path = HEngine::RenderPath::GpuDrivenOpaque;
        gpuVisible.bVisible = true;
        gpuVisible.ObjectGpuIndex = 0;
        gpuVisible.Bounds = {
            .Center = { 0.0f, 0.0f, 0.0f },
            .Radius = 1.0f,
            .Extents = { 0.8f, 0.75f, 0.01f },
            .bValid = true,
        };
        gpuVisible.Sections.push_back({
            .SubMeshResource = sharedMesh,
            .MaterialResource = gpuMaterial,
            .MaterialInstance = gpuMaterial->BuildRenderSnapshot(),
            .MaterialGpuIndex = 7,
        });
        world.Objects.push_back(std::move(gpuVisible));

        HEngine::RenderObject gpuVisibleInstance;
        gpuVisibleInstance.EntityId = 4;
        gpuVisibleInstance.WorldTransform = glm::scale(
            glm::translate(glm::mat4(1.0f), glm::vec3(-0.35f, -0.25f, -0.05f)),
            glm::vec3(0.45f));
        gpuVisibleInstance.PreviousWorldTransform = gpuVisibleInstance.WorldTransform;
        gpuVisibleInstance.Path = HEngine::RenderPath::GpuDrivenOpaque;
        gpuVisibleInstance.bVisible = true;
        gpuVisibleInstance.ObjectGpuIndex = 1;
        gpuVisibleInstance.Bounds = {
            .Center = { -0.35f, -0.25f, -0.05f },
            .Radius = 0.5f,
            .Extents = { 0.36f, 0.34f, 0.01f },
            .bValid = true,
        };
        gpuVisibleInstance.Sections.push_back({
            .SubMeshResource = sharedMesh,
            .MaterialResource = gpuMaterial,
            .MaterialInstance = gpuMaterial->BuildRenderSnapshot(),
            .MaterialGpuIndex = 7,
        });
        world.Objects.push_back(std::move(gpuVisibleInstance));

        HEngine::RenderObject gpuCulled;
        gpuCulled.EntityId = 2;
        gpuCulled.WorldTransform = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f));
        gpuCulled.PreviousWorldTransform = gpuCulled.WorldTransform;
        gpuCulled.Path = HEngine::RenderPath::GpuDrivenOpaque;
        gpuCulled.bVisible = true;
        gpuCulled.ObjectGpuIndex = 2;
        gpuCulled.Bounds = {
            .Center = { 10.0f, 0.0f, 0.0f },
            .Radius = 1.0f,
            .Extents = { 0.8f, 0.75f, 0.01f },
            .bValid = true,
        };
        gpuCulled.Sections.push_back({
            .SubMeshResource = sharedMesh,
            .MaterialResource = gpuMaterial,
            .MaterialInstance = gpuMaterial->BuildRenderSnapshot(),
            .MaterialGpuIndex = 7,
        });
        world.Objects.push_back(std::move(gpuCulled));

        HEngine::RenderObject cpuFallback;
        cpuFallback.EntityId = 3;
        cpuFallback.WorldTransform = glm::scale(
            glm::translate(glm::mat4(1.0f), glm::vec3(0.45f, -0.35f, -0.1f)),
            glm::vec3(0.35f));
        cpuFallback.PreviousWorldTransform = cpuFallback.WorldTransform;
        cpuFallback.Path = HEngine::RenderPath::CpuOpaque;
        cpuFallback.bVisible = true;
        cpuFallback.bSelected = true;
        cpuFallback.ObjectGpuIndex = 3;
        cpuFallback.Sections.push_back({
            .SubMeshResource = sharedMesh,
            .MaterialResource = cpuMaterial,
            .MaterialInstance = cpuMaterial->BuildRenderSnapshot(),
            .MaterialGpuIndex = 8,
        });
        world.Objects.push_back(std::move(cpuFallback));

        const auto appendCustomMaterialObject = [&](int entityId,
            uint32_t materialIndex,
            float x,
            const HEngine::Ref<HEngine::Material>& material)
        {
            HEngine::RenderObject custom;
            custom.EntityId = entityId;
            custom.WorldTransform = glm::translate(glm::mat4(1.0f), glm::vec3(x, -0.55f, 0.0f));
            custom.PreviousWorldTransform = custom.WorldTransform;
            custom.Bounds = {
                .Center = { x, -0.55f, 0.0f },
                .Radius = 0.3f,
                .Extents = { 0.3f, 0.3f, 0.01f },
                .bValid = true,
            };
            custom.Path = HEngine::RenderPath::CpuOpaque;
            custom.bVisible = true;
            custom.ObjectGpuIndex = static_cast<uint32_t>(entityId);
            custom.Sections.push_back({
                .SubMeshResource = sharedMesh,
                .MaterialResource = material,
                .MaterialInstance = material->BuildRenderSnapshot(),
                .MaterialGpuIndex = materialIndex,
                .Path = HEngine::RenderPath::CpuOpaque,
                .AlphaMode = HEngine::RenderMaterialAlphaMode::Opaque,
                .AlphaCutoff = 0.5f,
                .bDoubleSided = false,
            });
            world.Objects.push_back(std::move(custom));
        };
        appendCustomMaterialObject(11, 11, -0.5f, customMaterialA);
        appendCustomMaterialObject(12, 12, 0.5f, customMaterialB);

        HEngine::RenderObject alphaMask;
        alphaMask.EntityId = 5;
        alphaMask.WorldTransform = glm::scale(
            glm::translate(glm::mat4(1.0f), glm::vec3(0.7f, -0.1f, -0.15f)),
            glm::vec3(0.22f));
        alphaMask.PreviousWorldTransform = alphaMask.WorldTransform;
        alphaMask.Path = HEngine::RenderPath::GpuDrivenOpaque;
        alphaMask.bVisible = true;
        alphaMask.bSelected = true;
        alphaMask.ObjectGpuIndex = 4;
        alphaMask.Bounds = {
            .Center = { 0.7f, -0.1f, -0.15f },
            .Radius = 0.3f,
            .Extents = { 0.2f, 0.2f, 0.01f },
            .bValid = true,
        };
        alphaMask.Sections.push_back({
            .SubMeshResource = sharedMesh,
            .MaterialResource = customMaskMaterial,
            .MaterialInstance = customMaskMaterial->BuildRenderSnapshot(),
            .MaterialGpuIndex = 9,
            .Path = HEngine::RenderPath::GpuDrivenOpaque,
            .AlphaMode = HEngine::RenderMaterialAlphaMode::Mask,
            .AlphaCutoff = 0.5f,
            .bDoubleSided = true,
        });
        world.Objects.push_back(std::move(alphaMask));

        HEngine::RenderObject transparent;
        transparent.EntityId = 6;
        transparent.WorldTransform = glm::scale(
            glm::translate(glm::mat4(1.0f), glm::vec3(-0.6f, 0.05f, -0.3f)),
            glm::vec3(0.3f));
        transparent.PreviousWorldTransform = transparent.WorldTransform;
        transparent.Path = HEngine::RenderPath::Transparent;
        transparent.bVisible = true;
        transparent.ObjectGpuIndex = 5;
        transparent.Sections.push_back({
            .SubMeshResource = sharedMesh,
            .MaterialResource = customTransparentMaterial,
            .MaterialInstance = customTransparentMaterial->BuildRenderSnapshot(),
            .MaterialGpuIndex = 10,
            .Path = HEngine::RenderPath::Transparent,
            .AlphaMode = HEngine::RenderMaterialAlphaMode::Blend,
            .AlphaCutoff = 0.5f,
            .bDoubleSided = true,
        });
        world.Objects.push_back(std::move(transparent));

        HEngine::RenderObject skinned;
        skinned.EntityId = 7;
        skinned.WorldTransform = glm::translate(
            glm::mat4(1.0f), glm::vec3(0.05f, 0.3f, -0.2f));
        skinned.PreviousWorldTransform = skinned.WorldTransform;
        skinned.Path = HEngine::RenderPath::CpuSkinned;
        skinned.bVisible = true;
        skinned.ObjectGpuIndex = 6;
        // The shader must consume this owning snapshot; the non-identity
        // translation proves the draw is not exercising an identity fallback.
        skinned.BonePalette.push_back(glm::translate(
            glm::mat4(1.0f), glm::vec3(0.2f, 0.0f, 0.0f)));
        skinned.Sections.push_back({
            .SubMeshResource = skinnedMesh,
            .MaterialResource = customMaterialA,
            .MaterialInstance = customMaterialA->BuildRenderSnapshot(),
            .MaterialGpuIndex = 11,
            .Path = HEngine::RenderPath::CpuSkinned,
        });
        world.Objects.push_back(std::move(skinned));

        world.Primitives2D.push_back({
            .Kind = HEngine::RenderPrimitive2DKind::Quad,
            .Category = HEngine::RenderPrimitive2DCategory::Scene,
            .DepthMode = HEngine::RenderPrimitiveDepthMode::DepthTest,
            .Transform = glm::scale(
                glm::translate(glm::mat4(1.0f), glm::vec3(-0.55f, 0.45f, -0.2f)),
                glm::vec3(0.3f)),
            .Color = { 1.0f, 0.65f, 0.4f, 0.85f },
            .TextureAssetPath = "Assets/Textures/DefaultTexture.png",
            .TilingFactor = 1.0f,
            .EntityId = 11,
            .Layer = 0,
        });
        world.Primitives2D.push_back({
            .Kind = HEngine::RenderPrimitive2DKind::Circle,
            .Category = HEngine::RenderPrimitive2DCategory::Scene,
            .DepthMode = HEngine::RenderPrimitiveDepthMode::DepthTest,
            .TextureFallback = HEngine::RenderPrimitiveTextureFallback::White,
            .Transform = glm::scale(
                glm::translate(glm::mat4(1.0f), glm::vec3(0.58f, 0.42f, -0.2f)),
                glm::vec3(0.28f)),
            .Color = { 0.25f, 0.75f, 1.0f, 0.8f },
            .Thickness = 0.35f,
            .Fade = 0.02f,
            .EntityId = 12,
            .Layer = 1,
        });
        world.Primitives2D.push_back({
            .Kind = HEngine::RenderPrimitive2DKind::Line,
            .Category = HEngine::RenderPrimitive2DCategory::Editor,
            .DepthMode = HEngine::RenderPrimitiveDepthMode::Overlay,
            .TextureFallback = HEngine::RenderPrimitiveTextureFallback::White,
            .LineStart = { -0.75f, -0.75f, 0.0f },
            .LineEnd = { 0.75f, -0.6f, 0.0f },
            .Color = { 1.0f, 1.0f, 0.15f, 1.0f },
            .LineWidth = 3.0f,
            .EntityId = 13,
            .Layer = 2,
        });
        return world;
    }

    void RenderFrames(
        HEngine::Window& window,
        HEngine::RenderDevice& renderDevice,
        HEngine::SceneRenderer& sceneRenderer,
        HEngine::RenderWorld& world,
        uint32_t frameCount,
        HEngine::SceneRendererOutputMode outputMode =
            HEngine::SceneRendererOutputMode::SwapChain,
        HEngine::RuntimeUiRenderMode sceneOverlayMode =
            HEngine::RuntimeUiRenderMode::Direct,
        HEngine::TextureReadbackQueue* readbackQueue = nullptr,
        HEngine::TextureReadbackHandle* captureHandle = nullptr,
        const std::filesystem::path& capturePath = {})
    {
        for (uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
        {
            window.OnUpdate();
            world.FrameNumber++;
            HEngine::FrameContext& frame = renderDevice.BeginFrame();
            if (readbackQueue)
                readbackQueue->Collect(frame.CompletedSubmissionInstance);
            HEngine::SwapChain& swapChain = renderDevice.GetSwapChain();
            try
            {
                HEngine::EffekseerRuntime::BeginHostFrame(frame.FrameIndex, frame.CompletedSubmissionInstance, frame.GraphicsCommandList);
                bool bSceneOverlayRecorded = false;
                bool bWindowOverlayRecorded = false;
                bool bCaptureRecorded = false;
                sceneRenderer.RecordFrame(world, HEngine::SceneRendererFrameDesc{
                    .CommandList = frame.GraphicsCommandList,
                    .BackBuffer = swapChain.GetCurrentBackBuffer(),
                    .BackBufferFormat = nvrhi::Format::RGBA8_UNORM,
                    .Width = swapChain.GetWidth(),
                    .Height = swapChain.GetHeight(),
                    .FrameSlot = frame.FrameIndex,
                    .CompletedSubmission = frame.CompletedSubmissionInstance,
                    .OutputMode = outputMode,
                    .SceneOverlayMode = sceneOverlayMode,
                    .SceneOverlayTransition = {
                        .Effect = HEngine::RuntimeUiTransitionEffect::StarPetalReveal,
                        .Progress = 0.65f,
                        .EdgeSoftnessPixels = 3.f,
                        .CenterX = 0.48f,
                        .CenterY = 0.52f,
                    },
                    .SceneOverlayRecorder = [&bSceneOverlayRecorded, sceneOverlayMode](
                        const HEngine::SceneRendererOverlayContext& context)
                    {
                        if (!context.CommandList || !context.Framebuffer || !context.BackBuffer)
                            throw std::runtime_error("Scene smoke received invalid scene UI graph context");

                        if (sceneOverlayMode == HEngine::RuntimeUiRenderMode::Offscreen)
                        {
                            const nvrhi::FramebufferDesc& framebufferDesc =
                                context.Framebuffer->getDesc();
                            if (!context.StencilTarget || !context.BackdropTexture
                                || framebufferDesc.colorAttachments.empty()
                                || framebufferDesc.colorAttachments[0].texture->getDesc().format
                                    != nvrhi::Format::RGBA8_UNORM
                                || !framebufferDesc.depthAttachment.valid()
                                || framebufferDesc.depthAttachment.texture->getDesc().format
                                    != nvrhi::Format::D24S8)
                            {
                                throw std::runtime_error(
                                    "Scene smoke runtime UI target is not RGBA8+D24S8 with a backdrop");
                            }
                        }
                        else if (context.StencilTarget || context.BackdropTexture)
                        {
                            throw std::runtime_error(
                                "Scene smoke direct UI path unexpectedly received offscreen resources");
                        }
                        bSceneOverlayRecorded = true;
                    },
                    .WindowOverlayRecorder = [&bWindowOverlayRecorded](
                        const HEngine::SceneRendererOverlayContext& context)
                    {
                        if (!context.CommandList || !context.Framebuffer || !context.BackBuffer)
                            throw std::runtime_error("Scene smoke received invalid window UI graph context");
                        bWindowOverlayRecorded = true;
                    },
                    .CaptureRecorder = [
                        &bCaptureRecorded,
                        readbackQueue,
                        captureHandle,
                        capturePath](
                        const HEngine::SceneRendererCaptureContext& context)
                    {
                        if (!context.CommandList || !context.BackBuffer)
                            throw std::runtime_error("Scene smoke received invalid capture graph context");
                        if (readbackQueue && captureHandle && !*captureHandle)
                        {
                            *captureHandle = readbackQueue->Record(
                                context.CommandList,
                                context.BackBuffer,
                                HEngine::TextureReadbackDesc{
                                    .OutputPath = capturePath,
                                    .bReplaceExisting = true,
                                });
                        }
                        bCaptureRecorded = true;
                    },
                });
                if (outputMode != HEngine::SceneRendererOutputMode::OffscreenOnly &&
                    (!bSceneOverlayRecorded || !bWindowOverlayRecorded || !bCaptureRecorded))
                {
                    throw std::runtime_error(
                        "Scene graph did not execute scene UI, window UI, and capture callbacks");
                }
                renderDevice.SubmitAndPresent(frame, false);
                HEngine::EffekseerRuntime::NotifyHostFrameSubmitted(frame.FrameIndex, frame.SubmissionInstance);
                if (readbackQueue && captureHandle && *captureHandle &&
                    readbackQueue->GetResult(*captureHandle).Status ==
                        HEngine::TextureReadbackStatus::PendingSubmission)
                {
                    readbackQueue->NotifySubmitted(
                        *captureHandle,
                        frame.SubmissionInstance);
                }
                sceneRenderer.NotifyFrameSubmitted(frame.FrameIndex, frame.SubmissionInstance);
            }
            catch (...)
            {
                if (frame.bSubmissionIssued)
                {
                    HEngine::EffekseerRuntime::NotifyHostFrameSubmitted(frame.FrameIndex, frame.SubmissionInstance);
                    sceneRenderer.NotifyFrameSubmitted(
                        frame.FrameIndex,
                        frame.SubmissionInstance);
                }
                else
                {
                    renderDevice.AbortFrame(frame);
                    HEngine::EffekseerRuntime::AbortHostFrame(frame.FrameIndex);
                    sceneRenderer.AbortFrame(frame.FrameIndex);
                }
                throw;
            }
        }
    }

    void RenderOffscreenFrames(
        HEngine::RenderDevice& renderDevice,
        HEngine::SceneRenderer& sceneRenderer,
        HEngine::RenderWorld& world,
        uint32_t width,
        uint32_t height,
        uint32_t frameCount,
        HEngine::TextureReadbackQueue* linearReadback = nullptr,
        HEngine::TextureReadbackHandle* linearCapture = nullptr,
        const std::filesystem::path& linearCapturePath = {},
        HEngine::TextureReadbackQueue* ldrReadback = nullptr,
        HEngine::TextureReadbackHandle* ldrCapture = nullptr,
        const std::filesystem::path& ldrCapturePath = {})
    {
        for (uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
        {
            ++world.FrameNumber;
            HEngine::FrameContext& frame = renderDevice.BeginFrame();
            const bool bCaptureThisFrame = linearReadback && linearCapture &&
                frameIndex + 1u == frameCount;
            const bool bCaptureLdrThisFrame = ldrReadback && ldrCapture &&
                frameIndex + 1u == frameCount;
            try
            {
                HEngine::EffekseerRuntime::BeginHostFrame(frame.FrameIndex, frame.CompletedSubmissionInstance, frame.GraphicsCommandList);
                sceneRenderer.RecordFrame(
                    world,
                    HEngine::SceneRendererFrameDesc{
                        .CommandList = frame.GraphicsCommandList,
                        .Width = width,
                        .Height = height,
                        .FrameSlot = frame.FrameIndex,
                        .CompletedSubmission = frame.CompletedSubmissionInstance,
                        .OutputMode = HEngine::SceneRendererOutputMode::OffscreenOnly,
                        .CaptureRecorder = bCaptureLdrThisFrame
                            ? HEngine::SceneRendererCaptureRecorder(
                                [ldrReadback, ldrCapture, ldrCapturePath](
                                    const HEngine::SceneRendererCaptureContext& context)
                                {
                                    *ldrCapture = ldrReadback->Record(
                                        context.CommandList,
                                        context.BackBuffer,
                                        HEngine::TextureReadbackDesc{
                                            .OutputPath = ldrCapturePath,
                                            .OutputFormat =
                                                HEngine::TextureReadbackOutputFormat::Png,
                                            .AlphaMode =
                                                HEngine::TextureReadbackAlphaMode::Preserve,
                                            .bReplaceExisting = true,
                                        });
                                })
                            : HEngine::SceneRendererCaptureRecorder{},
                        .LinearHdrCaptureRecorder = bCaptureThisFrame
                            ? HEngine::SceneRendererLinearHdrCaptureRecorder(
                                [linearReadback, linearCapture, linearCapturePath](
                                    const HEngine::SceneRendererLinearHdrCaptureContext& context)
                                {
                                    *linearCapture = linearReadback->Record(
                                        context.CommandList,
                                        context.LinearHdr,
                                        HEngine::TextureReadbackDesc{
                                            .OutputPath = linearCapturePath,
                                            .OutputFormat =
                                                HEngine::TextureReadbackOutputFormat::Pfm,
                                            .AlphaMode =
                                                HEngine::TextureReadbackAlphaMode::Discard,
                                            .NonFinitePolicy =
                                                HEngine::TextureReadbackNonFinitePolicy::Reject,
                                            .bReplaceExisting = true,
                                        });
                                })
                            : HEngine::SceneRendererLinearHdrCaptureRecorder{},
                    });
                renderDevice.SubmitAndPresent(frame, false);
                HEngine::EffekseerRuntime::NotifyHostFrameSubmitted(frame.FrameIndex, frame.SubmissionInstance);
                sceneRenderer.NotifyFrameSubmitted(
                    frame.FrameIndex,
                    frame.SubmissionInstance);
                if (bCaptureThisFrame && *linearCapture)
                {
                    linearReadback->NotifySubmitted(
                        *linearCapture,
                        frame.SubmissionInstance);
                }
                if (bCaptureLdrThisFrame && *ldrCapture)
                {
                    ldrReadback->NotifySubmitted(
                        *ldrCapture,
                        frame.SubmissionInstance);
                }
            }
            catch (...)
            {
                if (frame.bSubmissionIssued)
                {
                    HEngine::EffekseerRuntime::NotifyHostFrameSubmitted(frame.FrameIndex, frame.SubmissionInstance);
                    sceneRenderer.NotifyFrameSubmitted(
                        frame.FrameIndex,
                        frame.SubmissionInstance);
                    if (bCaptureThisFrame && *linearCapture &&
                        linearReadback->IsValid(*linearCapture) &&
                        linearReadback->GetResult(*linearCapture).Status ==
                            HEngine::TextureReadbackStatus::PendingSubmission)
                    {
                        linearReadback->NotifySubmitted(
                            *linearCapture,
                            frame.SubmissionInstance);
                    }
                    if (bCaptureLdrThisFrame && *ldrCapture &&
                        ldrReadback->IsValid(*ldrCapture) &&
                        ldrReadback->GetResult(*ldrCapture).Status ==
                            HEngine::TextureReadbackStatus::PendingSubmission)
                    {
                        ldrReadback->NotifySubmitted(
                            *ldrCapture,
                            frame.SubmissionInstance);
                    }
                }
                else
                {
                    renderDevice.AbortFrame(frame);
                    HEngine::EffekseerRuntime::AbortHostFrame(frame.FrameIndex);
                    sceneRenderer.AbortFrame(frame.FrameIndex);
                    if (bCaptureThisFrame && *linearCapture &&
                        linearReadback->IsValid(*linearCapture))
                    {
                        linearReadback->CancelPending(*linearCapture);
                    }
                    if (bCaptureLdrThisFrame && *ldrCapture &&
                        ldrReadback->IsValid(*ldrCapture))
                    {
                        ldrReadback->CancelPending(*ldrCapture);
                    }
                }
                throw;
            }
        }
    }

#if HE_WITH_OFFLINE_REFERENCE_RENDERER_SMOKE
    struct HardwareRtReadbackPassData
    {
        HEngine::TextureHandle Radiance;
        HEngine::TextureHandle SampleCount;
    };
#endif

    struct HardwareRtIntentionalAbortPassData
    {
        HEngine::AccelerationStructureHandle SceneTlas;
    };

    struct RealtimeRtgiReadbackPassData
    {
        HEngine::TextureHandle DenoisedDiffuse;
        HEngine::TextureHandle HistoryLength;
        HEngine::TextureHandle Confidence;
    };

    struct SurfaceDataAovReadbackPassData
    {
        HEngine::TextureHandle BaseColorMetalness;
        HEngine::TextureHandle MaterialId;
        HEngine::TextureHandle MotionVectors;
        HEngine::TextureHandle PreviousDepth;
    };

    [[nodiscard]] HEngine::TextureReadbackResult WaitForOfflineExport(
        HEngine::IOfflineViewportRendererSession& session,
        HEngine::TextureReadbackHandle handle)
    {
        for (uint32_t attempt = 0; attempt < 2000u; ++attempt)
        {
            session.Collect(std::numeric_limits<uint64_t>::max());
            const HEngine::TextureReadbackResult result =
                session.GetExportResult(handle);
            if (result.Status == HEngine::TextureReadbackStatus::Complete ||
                result.Status == HEngine::TextureReadbackStatus::Failed)
            {
                return result;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return session.GetExportResult(handle);
    }

    std::array<float, 4> ValidatePfmQuadrantCoverage(
        const std::filesystem::path& path,
        std::string_view label,
        bool bRequireAllQuadrants = true)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
            throw std::runtime_error(std::string(label) + " PFM is missing");
        std::string magic;
        std::string dimensionsLine;
        std::string scaleLine;
        std::getline(stream, magic);
        std::getline(stream, dimensionsLine);
        std::getline(stream, scaleLine);
        uint32_t width = 0;
        uint32_t height = 0;
        std::istringstream dimensions(dimensionsLine);
        dimensions >> width >> height;
        float scale = 0.0f;
        std::istringstream scaleStream(scaleLine);
        scaleStream >> scale;
        if (magic != "PF" || width < 2u || height < 2u || scale >= 0.0f)
            throw std::runtime_error(std::string(label) + " PFM header is invalid");
        std::vector<float> pixels(
            static_cast<size_t>(width) * height * 3u);
        stream.read(
            reinterpret_cast<char*>(pixels.data()),
            static_cast<std::streamsize>(pixels.size() * sizeof(float)));
        if (!stream)
            throw std::runtime_error(std::string(label) + " PFM payload is truncated");

        std::array<float, 4> maximumLuminance{};
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                const size_t index =
                    (static_cast<size_t>(y) * width + x) * 3u;
                const float red = pixels[index + 0u];
                const float green = pixels[index + 1u];
                const float blue = pixels[index + 2u];
                if (!std::isfinite(red) || !std::isfinite(green) ||
                    !std::isfinite(blue))
                {
                    throw std::runtime_error(
                        std::string(label) + " PFM contains non-finite values");
                }
                const uint32_t quadrant =
                    (x >= width / 2u ? 1u : 0u) |
                    (y >= height / 2u ? 2u : 0u);
                maximumLuminance[quadrant] = std::max(
                    maximumLuminance[quadrant],
                    0.2126f * red + 0.7152f * green + 0.0722f * blue);
            }
        }
        if ((bRequireAllQuadrants && std::ranges::any_of(
                maximumLuminance,
                [](float value) { return value <= 1e-6f; })) ||
            (!bRequireAllQuadrants && *std::max_element(
                maximumLuminance.begin(),
                maximumLuminance.end()) <= 1e-6f))
        {
            throw std::runtime_error(
                std::string(label) +
                " did not cover all full-resolution output quadrants");
        }
        return maximumLuminance;
    }

    struct LinearRgbImage
    {
        uint32_t Width = 0;
        uint32_t Height = 0;
        // Top-to-bottom, tightly packed RGB float32 pixels.
        std::vector<float> Pixels;
    };

    [[nodiscard]] LinearRgbImage ReadLinearPfm(
        const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
            throw std::runtime_error("Failed to open linear PFM: " + path.string());

        std::string magic;
        std::string dimensionsLine;
        std::string scaleLine;
        std::getline(stream, magic);
        std::getline(stream, dimensionsLine);
        std::getline(stream, scaleLine);
        uint32_t width = 0;
        uint32_t height = 0;
        std::istringstream dimensions(dimensionsLine);
        dimensions >> width >> height;
        float scale = 0.0f;
        std::istringstream scaleStream(scaleLine);
        scaleStream >> scale;
        if (magic != "PF" || width == 0u || height == 0u || scale >= 0.0f)
            throw std::runtime_error("Linear PFM header is invalid: " + path.string());
        if (std::endian::native != std::endian::little)
            throw std::runtime_error("Linear PFM helper requires a little-endian host");
        const size_t pixelCount = static_cast<size_t>(width) * height;
        if (height != 0u && pixelCount / height != width ||
            pixelCount > std::numeric_limits<size_t>::max() / 3u)
        {
            throw std::overflow_error("Linear PFM dimensions overflow addressable memory");
        }

        std::vector<float> stored(pixelCount * 3u);
        stream.read(
            reinterpret_cast<char*>(stored.data()),
            static_cast<std::streamsize>(stored.size() * sizeof(float)));
        if (!stream)
            throw std::runtime_error("Linear PFM payload is truncated: " + path.string());

        LinearRgbImage image{
            .Width = width,
            .Height = height,
            .Pixels = std::vector<float>(stored.size()),
        };
        const float scaleMagnitude = std::abs(scale);
        for (uint32_t storedY = 0; storedY < height; ++storedY)
        {
            const uint32_t outputY = height - storedY - 1u;
            for (uint32_t x = 0; x < width; ++x)
            {
                const size_t sourceIndex =
                    (static_cast<size_t>(storedY) * width + x) * 3u;
                const size_t outputIndex =
                    (static_cast<size_t>(outputY) * width + x) * 3u;
                for (uint32_t channel = 0; channel < 3u; ++channel)
                {
                    const float value = stored[sourceIndex + channel] *
                        scaleMagnitude;
                    if (!std::isfinite(value))
                    {
                        throw std::runtime_error(
                            "Linear PFM contains non-finite radiance: " +
                            path.string());
                    }
                    image.Pixels[outputIndex + channel] = value;
                }
            }
        }
        return image;
    }

    struct BinaryImageMask
    {
        uint32_t Width = 0;
        uint32_t Height = 0;
        std::vector<uint8_t> Pixels;
    };

    [[nodiscard]] BinaryImageMask ReadBinaryPgmMask(
        const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
            throw std::runtime_error("Failed to open PGM mask: " + path.string());
        const auto readToken = [&stream]()
        {
            std::string token;
            char character = 0;
            while (stream.get(character))
            {
                if (character == '#')
                {
                    stream.ignore(
                        std::numeric_limits<std::streamsize>::max(),
                        '\n');
                    continue;
                }
                if (!std::isspace(static_cast<unsigned char>(character)))
                {
                    token.push_back(character);
                    break;
                }
            }
            while (stream.get(character))
            {
                if (character == '#')
                {
                    stream.ignore(
                        std::numeric_limits<std::streamsize>::max(),
                        '\n');
                    break;
                }
                if (std::isspace(static_cast<unsigned char>(character)))
                    break;
                token.push_back(character);
            }
            return token;
        };
        if (readToken() != "P5")
            throw std::runtime_error("PGM mask must use binary P5: " + path.string());
        const uint32_t width = static_cast<uint32_t>(std::stoul(readToken()));
        const uint32_t height = static_cast<uint32_t>(std::stoul(readToken()));
        const uint32_t maximum = static_cast<uint32_t>(std::stoul(readToken()));
        if (width == 0u || height == 0u || maximum == 0u || maximum > 255u)
            throw std::runtime_error("PGM mask header is invalid: " + path.string());
        const size_t pixelCount = static_cast<size_t>(width) * height;
        if (pixelCount / height != width)
            throw std::overflow_error("PGM mask dimensions overflow addressable memory");
        BinaryImageMask mask{
            .Width = width,
            .Height = height,
            .Pixels = std::vector<uint8_t>(pixelCount),
        };
        stream.read(
            reinterpret_cast<char*>(mask.Pixels.data()),
            static_cast<std::streamsize>(mask.Pixels.size()));
        if (!stream)
            throw std::runtime_error("PGM mask payload is truncated: " + path.string());
        return mask;
    }

    [[nodiscard]] float MeanMaskedLuminance(
        const LinearRgbImage& image,
        const BinaryImageMask& mask)
    {
        if (image.Width != mask.Width || image.Height != mask.Height ||
            image.Pixels.size() !=
                static_cast<size_t>(image.Width) * image.Height * 3u)
        {
            throw std::invalid_argument("PFM image and PGM mask dimensions differ");
        }
        double sum = 0.0;
        size_t count = 0;
        for (size_t pixel = 0; pixel < mask.Pixels.size(); ++pixel)
        {
            if (mask.Pixels[pixel] == 0u)
                continue;
            const size_t base = pixel * 3u;
            sum += 0.2126 * image.Pixels[base + 0u] +
                0.7152 * image.Pixels[base + 1u] +
                0.0722 * image.Pixels[base + 2u];
            ++count;
        }
        if (count == 0u)
            throw std::runtime_error("PGM mask does not select any pixels");
        return static_cast<float>(sum / static_cast<double>(count));
    }

    [[nodiscard]] float Sinc(float value) noexcept
    {
        constexpr float Pi = 3.14159265358979323846f;
        if (std::abs(value) < 1.0e-6f)
            return 1.0f;
        const float angle = Pi * value;
        return std::sin(angle) / angle;
    }

    [[nodiscard]] float Lanczos2(float value) noexcept
    {
        return std::abs(value) < 2.0f
            ? Sinc(value) * Sinc(value * 0.5f)
            : 0.0f;
    }

    [[nodiscard]] LinearRgbImage DownsampleLinearLanczos2(
        const LinearRgbImage& source,
        uint32_t outputWidth,
        uint32_t outputHeight)
    {
        if (source.Width == 0u || source.Height == 0u ||
            source.Pixels.size() !=
                static_cast<size_t>(source.Width) * source.Height * 3u ||
            outputWidth == 0u || outputHeight == 0u ||
            outputWidth > source.Width || outputHeight > source.Height)
        {
            throw std::invalid_argument(
                "Lanczos2 downsample dimensions or source payload are invalid");
        }

        const float horizontalScale = static_cast<float>(source.Width) /
            static_cast<float>(outputWidth);
        const float verticalScale = static_cast<float>(source.Height) /
            static_cast<float>(outputHeight);
        const float horizontalRadius = 2.0f * horizontalScale;
        const float verticalRadius = 2.0f * verticalScale;
        std::vector<float> horizontal(
            static_cast<size_t>(outputWidth) * source.Height * 3u,
            0.0f);

        for (uint32_t y = 0; y < source.Height; ++y)
        {
            for (uint32_t outputX = 0; outputX < outputWidth; ++outputX)
            {
                const float center =
                    (static_cast<float>(outputX) + 0.5f) * horizontalScale -
                    0.5f;
                const int64_t first = static_cast<int64_t>(
                    std::ceil(center - horizontalRadius));
                const int64_t last = static_cast<int64_t>(
                    std::floor(center + horizontalRadius));
                std::array<double, 3> sum{};
                double weightSum = 0.0;
                for (int64_t sampleX = first; sampleX <= last; ++sampleX)
                {
                    const float weight = Lanczos2(
                        (static_cast<float>(sampleX) - center) /
                        horizontalScale);
                    if (weight == 0.0f)
                        continue;
                    const uint32_t clampedX = static_cast<uint32_t>(std::clamp<int64_t>(
                        sampleX,
                        0,
                        static_cast<int64_t>(source.Width) - 1));
                    const size_t sourceIndex =
                        (static_cast<size_t>(y) * source.Width + clampedX) * 3u;
                    for (uint32_t channel = 0; channel < 3u; ++channel)
                    {
                        sum[channel] += static_cast<double>(
                            source.Pixels[sourceIndex + channel]) * weight;
                    }
                    weightSum += weight;
                }
                if (std::abs(weightSum) < 1.0e-10)
                    throw std::runtime_error("Lanczos2 horizontal kernel is empty");
                const size_t outputIndex =
                    (static_cast<size_t>(y) * outputWidth + outputX) * 3u;
                for (uint32_t channel = 0; channel < 3u; ++channel)
                {
                    horizontal[outputIndex + channel] = static_cast<float>(
                        sum[channel] / weightSum);
                }
            }
        }

        LinearRgbImage result{
            .Width = outputWidth,
            .Height = outputHeight,
            .Pixels = std::vector<float>(
                static_cast<size_t>(outputWidth) * outputHeight * 3u,
                0.0f),
        };
        for (uint32_t outputY = 0; outputY < outputHeight; ++outputY)
        {
            const float center =
                (static_cast<float>(outputY) + 0.5f) * verticalScale - 0.5f;
            const int64_t first = static_cast<int64_t>(
                std::ceil(center - verticalRadius));
            const int64_t last = static_cast<int64_t>(
                std::floor(center + verticalRadius));
            for (uint32_t x = 0; x < outputWidth; ++x)
            {
                std::array<double, 3> sum{};
                double weightSum = 0.0;
                for (int64_t sampleY = first; sampleY <= last; ++sampleY)
                {
                    const float weight = Lanczos2(
                        (static_cast<float>(sampleY) - center) / verticalScale);
                    if (weight == 0.0f)
                        continue;
                    const uint32_t clampedY = static_cast<uint32_t>(std::clamp<int64_t>(
                        sampleY,
                        0,
                        static_cast<int64_t>(source.Height) - 1));
                    const size_t sourceIndex =
                        (static_cast<size_t>(clampedY) * outputWidth + x) * 3u;
                    for (uint32_t channel = 0; channel < 3u; ++channel)
                    {
                        sum[channel] += static_cast<double>(
                            horizontal[sourceIndex + channel]) * weight;
                    }
                    weightSum += weight;
                }
                if (std::abs(weightSum) < 1.0e-10)
                    throw std::runtime_error("Lanczos2 vertical kernel is empty");
                const size_t outputIndex =
                    (static_cast<size_t>(outputY) * outputWidth + x) * 3u;
                for (uint32_t channel = 0; channel < 3u; ++channel)
                {
                    const float value = static_cast<float>(sum[channel] / weightSum);
                    result.Pixels[outputIndex + channel] = std::clamp(
                        std::isfinite(value) ? value : 0.0f,
                        0.0f,
                        65504.0f);
                }
            }
        }
        return result;
    }

    void WriteEncodedFile(
        const std::filesystem::path& path,
        std::span<const uint8_t> bytes)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream)
            throw std::runtime_error("Failed to create encoded output: " + path.string());
        stream.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        if (!stream)
            throw std::runtime_error("Failed to write encoded output: " + path.string());
    }

    void WriteLinearPfm(
        const std::filesystem::path& path,
        const LinearRgbImage& image)
    {
        const size_t pixelCount = static_cast<size_t>(image.Width) * image.Height;
        if (image.Width == 0u || image.Height == 0u ||
            image.Pixels.size() != pixelCount * 3u)
        {
            throw std::invalid_argument("Linear PFM image is invalid");
        }
        std::vector<float> rgba(pixelCount * 4u, 1.0f);
        for (size_t pixel = 0; pixel < pixelCount; ++pixel)
        {
            rgba[pixel * 4u + 0u] = image.Pixels[pixel * 3u + 0u];
            rgba[pixel * 4u + 1u] = image.Pixels[pixel * 3u + 1u];
            rgba[pixel * 4u + 2u] = image.Pixels[pixel * 3u + 2u];
        }
        const std::vector<uint8_t> encoded =
            HEngine::TextureReadbackQueue::EncodePfm(
                image.Width,
                image.Height,
                nvrhi::Format::RGBA32_FLOAT,
                reinterpret_cast<const uint8_t*>(rgba.data()),
                static_cast<size_t>(image.Width) * 4u * sizeof(float),
                HEngine::TextureReadbackAlphaMode::Discard,
                HEngine::TextureReadbackNonFinitePolicy::Reject);
        WriteEncodedFile(path, encoded);
    }

    [[nodiscard]] float ToneMapAces(float color) noexcept
    {
        color = std::clamp(
            std::isfinite(color) ? color : 0.0f,
            0.0f,
            65504.0f);
        return std::clamp(
            color * (2.51f * color + 0.03f) /
                (color * (2.43f * color + 0.59f) + 0.14f),
            0.0f,
            1.0f);
    }

    [[nodiscard]] float LinearToSrgb(float color) noexcept
    {
        color = std::clamp(color, 0.0f, 1.0f);
        return color <= 0.0031308f
            ? color * 12.92f
            : 1.055f * std::pow(color, 1.0f / 2.4f) - 0.055f;
    }

    [[nodiscard]] uint8_t FloatToUnorm8(float value) noexcept
    {
        const long quantized = std::lround(
            std::clamp(value, 0.0f, 1.0f) * 255.0f);
        return static_cast<uint8_t>(std::clamp(quantized, 0l, 255l));
    }

    void WriteToneMappedPng(
        const std::filesystem::path& path,
        const LinearRgbImage& image)
    {
        const size_t pixelCount = static_cast<size_t>(image.Width) * image.Height;
        if (image.Width == 0u || image.Height == 0u ||
            image.Pixels.size() != pixelCount * 3u)
        {
            throw std::invalid_argument("Tone-mapped PNG image is invalid");
        }
        std::vector<uint8_t> rgba(pixelCount * 4u, 255u);
        for (size_t pixel = 0; pixel < pixelCount; ++pixel)
        {
            for (uint32_t channel = 0; channel < 3u; ++channel)
            {
                rgba[pixel * 4u + channel] = FloatToUnorm8(LinearToSrgb(
                    ToneMapAces(image.Pixels[pixel * 3u + channel])));
            }
        }
        const std::vector<uint8_t> encoded =
            HEngine::TextureReadbackQueue::EncodeRgba8Png(
                image.Width,
                image.Height,
                rgba.data(),
                static_cast<size_t>(image.Width) * 4u,
                false);
        WriteEncodedFile(path, encoded);
    }

    [[nodiscard]] std::string ComputeFileSha256(
        const std::filesystem::path& path)
    {
        std::string hash;
        std::string error;
        if (!HEngine::ComputeMaterialShaderFileSha256(path, hash, error))
        {
            throw std::runtime_error(
                "Failed to hash capture '" + path.string() + "': " + error);
        }
        return hash;
    }

    [[nodiscard]] HEngine::RenderObject MakeHardwareRtQuad(
        int entityId,
        std::string meshPath,
        std::vector<HEngine::StaticVertex> vertices,
        std::vector<uint32_t> indices,
        const glm::vec3& baseColor,
        const glm::vec3& emissive)
    {
        HEngine::Ref<HEngine::SubMesh> mesh =
            HEngine::CreateRef<HEngine::SubMesh>(
                std::move(vertices),
                std::move(indices));
        HEngine::Ref<HEngine::Material> material =
            HEngine::CreateRef<HEngine::Material>();
        material->baseColorFactor = glm::vec4(baseColor, 1.0f);
        material->emissiveFactor = emissive;
        material->metallicFactor = 0.0f;
        material->roughnessFactor = 1.0f;

        HEngine::RenderObject object;
        object.EntityId = entityId;
        object.MeshAssetPath = std::move(meshPath);
        object.WorldTransform = glm::mat4(1.0f);
        object.PreviousWorldTransform = object.WorldTransform;
        object.Path = HEngine::RenderPath::CpuOpaque;
        object.bVisible = true;
        object.Sections.push_back({
            .SubMeshResource = std::move(mesh),
            .MaterialResource = material,
            .MaterialInstance = material->BuildRenderSnapshot(),
            .Path = HEngine::RenderPath::CpuOpaque,
            .AlphaMode = HEngine::RenderMaterialAlphaMode::Opaque,
        });
        return object;
    }

    [[nodiscard]] HEngine::RenderWorld BuildHardwareRtSmokeWorld()
    {
        using HEngine::StaticVertex;
        const auto vertex = [](glm::vec3 position, glm::vec3 normal)
        {
            return StaticVertex(
                position,
                normal,
                { 0.0f, 0.0f },
                { 1.0f, 0.0f, 0.0f },
                { 0.0f, 0.0f, 1.0f });
        };

        HEngine::RenderWorld world;
        world.Camera.Position = { 0.0f, 1.2f, 3.5f };
        world.Camera.View = glm::lookAt(
            world.Camera.Position,
            glm::vec3(0.0f, 0.0f, 0.0f),
            glm::vec3(0.0f, 1.0f, 0.0f));
        world.Camera.Projection = glm::perspective(
            glm::radians(45.0f),
            1.0f,
            0.1f,
            100.0f);
        world.Camera.ViewProjection =
            world.Camera.Projection * world.Camera.View;
        world.Camera.PreviousViewProjection = world.Camera.ViewProjection;
        world.Camera.NearPlane = 0.1f;
        world.Camera.FarPlane = 100.0f;

        world.Objects.push_back(MakeHardwareRtQuad(
            1001,
            "Synthetic/HardwareRtSmoke/Floor",
            {
                vertex({ -2.0f, 0.0f, -2.0f }, { 0.0f, 1.0f, 0.0f }),
                vertex({ -2.0f, 0.0f, 2.0f }, { 0.0f, 1.0f, 0.0f }),
                vertex({ 2.0f, 0.0f, 2.0f }, { 0.0f, 1.0f, 0.0f }),
                vertex({ 2.0f, 0.0f, -2.0f }, { 0.0f, 1.0f, 0.0f }),
            },
            { 0, 1, 2, 0, 2, 3 },
            { 0.72f, 0.72f, 0.72f },
            { 0.0f, 0.0f, 0.0f }));
        world.Objects.push_back(MakeHardwareRtQuad(
            1002,
            "Synthetic/HardwareRtSmoke/AreaLight",
            {
                vertex({ -0.5f, 2.0f, -0.5f }, { 0.0f, -1.0f, 0.0f }),
                vertex({ 0.5f, 2.0f, -0.5f }, { 0.0f, -1.0f, 0.0f }),
                vertex({ 0.5f, 2.0f, 0.5f }, { 0.0f, -1.0f, 0.0f }),
                vertex({ -0.5f, 2.0f, 0.5f }, { 0.0f, -1.0f, 0.0f }),
            },
            { 0, 1, 2, 0, 2, 3 },
            { 1.0f, 1.0f, 1.0f },
            { 20.0f, 18.0f, 14.0f }));
        return world;
    }

    struct ImportedCornellWorld
    {
        HEngine::RenderWorld World;
        size_t ShortBlockObjectIndex = std::numeric_limits<size_t>::max();
        size_t TallBlockObjectIndex = std::numeric_limits<size_t>::max();
    };

    [[nodiscard]] bool IsInsideAabb(
        const glm::vec3& position,
        const glm::vec3& minimum,
        const glm::vec3& maximum) noexcept
    {
        constexpr float Epsilon = 1.0e-6f;
        return position.x >= minimum.x - Epsilon &&
            position.y >= minimum.y - Epsilon &&
            position.z >= minimum.z - Epsilon &&
            position.x <= maximum.x + Epsilon &&
            position.y <= maximum.y + Epsilon &&
            position.z <= maximum.z + Epsilon;
    }

    [[nodiscard]] bool AreMatricesExactlyEqual(
        const glm::mat4& left,
        const glm::mat4& right) noexcept
    {
        for (glm::length_t column = 0; column < 4; ++column)
        {
            for (glm::length_t row = 0; row < 4; ++row)
            {
                if (left[column][row] != right[column][row])
                    return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::array<HEngine::Ref<HEngine::SubMesh>, 3>
    SplitImportedCornellWhiteGeometry(
        const HEngine::Ref<HEngine::SubMesh>& white)
    {
        if (!white || white->IsSkinned() || white->mStaticVertices.size() != 56u ||
            white->GetIndices().size() != 96u)
        {
            throw std::runtime_error(
                "Imported Cornell white geometry layout is not the validated 32-triangle mesh");
        }

        const glm::vec3 shortMinimum(0.082f, 0.0f, 0.065f);
        const glm::vec3 shortMaximum(0.290f, 0.165f, 0.272f);
        const glm::vec3 tallMinimum(0.265f, 0.0f, 0.247f);
        const glm::vec3 tallMaximum(0.472f, 0.330f, 0.456f);
        enum class Part : uint32_t
        {
            Room,
            ShortBlock,
            TallBlock,
        };
        std::array<std::vector<HEngine::StaticVertex>, 3> vertices;
        std::array<std::vector<uint32_t>, 3> indices;
        const std::vector<uint32_t>& sourceIndices = white->GetIndices();
        for (size_t triangleOffset = 0;
            triangleOffset < sourceIndices.size();
            triangleOffset += 3u)
        {
            std::array<const HEngine::StaticVertex*, 3> triangle{};
            for (size_t corner = 0; corner < triangle.size(); ++corner)
            {
                const uint32_t vertexIndex = sourceIndices[triangleOffset + corner];
                if (vertexIndex >= white->mStaticVertices.size())
                    throw std::runtime_error("Imported Cornell white index is out of range");
                triangle[corner] = &white->mStaticVertices[vertexIndex];
            }
            const bool bInsideShort = std::ranges::all_of(
                triangle,
                [&](const HEngine::StaticVertex* vertex)
                {
                    return IsInsideAabb(vertex->Pos, shortMinimum, shortMaximum);
                });
            const bool bInsideTall = std::ranges::all_of(
                triangle,
                [&](const HEngine::StaticVertex* vertex)
                {
                    return IsInsideAabb(vertex->Pos, tallMinimum, tallMaximum);
                });
            if (bInsideShort && bInsideTall)
            {
                throw std::runtime_error(
                    "Imported Cornell white triangle matches both official block AABBs");
            }
            const Part part = bInsideShort
                ? Part::ShortBlock
                : bInsideTall
                    ? Part::TallBlock
                    : Part::Room;
            const size_t partIndex = static_cast<size_t>(part);
            for (const HEngine::StaticVertex* vertex : triangle)
            {
                indices[partIndex].push_back(
                    static_cast<uint32_t>(vertices[partIndex].size()));
                vertices[partIndex].push_back(*vertex);
            }
        }
        if (indices[static_cast<size_t>(Part::Room)].size() != 36u ||
            indices[static_cast<size_t>(Part::ShortBlock)].size() != 30u ||
            indices[static_cast<size_t>(Part::TallBlock)].size() != 30u)
        {
            throw std::runtime_error(
                "Imported Cornell AABB split is not room/short/tall = 12/10/10 triangles");
        }

        std::array<HEngine::Ref<HEngine::SubMesh>, 3> result;
        for (size_t partIndex = 0; partIndex < result.size(); ++partIndex)
        {
            result[partIndex] = HEngine::CreateRef<HEngine::SubMesh>(
                std::move(vertices[partIndex]),
                std::move(indices[partIndex]),
                white->mMaterialIndex);
        }
        return result;
    }

    [[nodiscard]] HEngine::RenderObject MakeImportedCornellObject(
        int entityId,
        uint32_t gpuIndex,
        const HEngine::Ref<HEngine::Mesh>& mesh,
        std::string meshIdentity,
        const HEngine::Ref<HEngine::SubMesh>& subMesh)
    {
        if (!mesh || !subMesh || subMesh->mMaterialIndex >= mesh->mMaterial.size())
            throw std::runtime_error("Imported Cornell object material mapping is invalid");
        const HEngine::Ref<HEngine::Material>& material =
            mesh->mMaterial[subMesh->mMaterialIndex];
        if (!material || !subMesh->HasBounds())
            throw std::runtime_error("Imported Cornell object is missing material or bounds");

        HEngine::RenderObject object;
        object.EntityId = entityId;
        object.MeshResource = mesh;
        object.MeshAssetPath = std::move(meshIdentity);
        object.WorldTransform = glm::mat4(1.0f);
        object.PreviousWorldTransform = object.WorldTransform;
        object.ObjectGpuIndex = gpuIndex;
        object.MeshGpuIndex = gpuIndex;
        object.Path = HEngine::RenderPath::GpuDrivenOpaque;
        object.bVisible = true;
        const glm::vec3 minimum = subMesh->GetBoundsMin();
        const glm::vec3 maximum = subMesh->GetBoundsMax();
        const glm::vec3 extents = (maximum - minimum) * 0.5f;
        object.Bounds = {
            .Center = (minimum + maximum) * 0.5f,
            .Radius = glm::length(extents),
            .Extents = extents,
            .bValid = true,
        };
        HEngine::RenderMeshSection section;
        section.SubMeshResource = subMesh;
        section.MaterialResource = material;
        section.MaterialInstance = material->BuildRenderSnapshot();
        section.MaterialGpuIndex = subMesh->mMaterialIndex;
        section.Path = HEngine::RenderPath::GpuDrivenOpaque;
        section.AlphaMode = section.MaterialInstance.AlphaMode;
        section.AlphaCutoff = section.MaterialInstance.AlphaCutoff;
        section.bDoubleSided = section.MaterialInstance.bDoubleSided;
        object.Sections.push_back(std::move(section));
        return object;
    }

    [[nodiscard]] ImportedCornellWorld BuildImportedCornellWorld()
    {
        ImportedCornellWorld result;
        HEngine::RenderWorld& world = result.World;
        world.SceneGeneration = 0x434f524e454c4c01ull;
        world.Camera.Position = { 0.278f, 0.273f, -0.8f };
        world.Camera.View = glm::lookAt(
            world.Camera.Position,
            world.Camera.Position + glm::vec3(0.0f, 0.0f, 1.0f),
            glm::vec3(0.0f, 1.0f, 0.0f));
        world.Camera.Projection = glm::perspective(
            0.686047881f,
            1.0f,
            0.01f,
            10.0f);
        world.Camera.ViewProjection =
            world.Camera.Projection * world.Camera.View;
        world.Camera.PreviousViewProjection = world.Camera.ViewProjection;
        world.Camera.NonJitteredViewProjection = world.Camera.ViewProjection;
        world.Camera.PreviousNonJitteredViewProjection =
            world.Camera.ViewProjection;
        world.Camera.NearPlane = 0.01f;
        world.Camera.FarPlane = 10.0f;
        world.Environment.Exposure = 1.0f;
        world.PostProcess.Exposure = 1.0f;
        world.Environment.IblIntensity = 0.0f;

        HEngine::Ref<HEngine::Mesh> mesh =
            HEngine::CreateRef<HEngine::Mesh>(
                "Assets/CornellBox/CornellBox.hmesh");
        if (!mesh || mesh->mSubMeshes.size() != 4u || mesh->mMaterial.size() < 4u)
            throw std::runtime_error("Imported Cornell mesh/material slots are incomplete");
        const std::array<HEngine::Ref<HEngine::SubMesh>, 3> whiteParts =
            SplitImportedCornellWhiteGeometry(mesh->mSubMeshes[0]);
        constexpr std::string_view BaseIdentity =
            "Assets/CornellBox/CornellBox.hmesh";
        world.Objects.push_back(MakeImportedCornellObject(
            2001,
            0,
            mesh,
            std::string(BaseIdentity) + "#white-room",
            whiteParts[0]));
        result.ShortBlockObjectIndex = world.Objects.size();
        world.Objects.push_back(MakeImportedCornellObject(
            2002,
            1,
            mesh,
            std::string(BaseIdentity) + "#short-block",
            whiteParts[1]));
        result.TallBlockObjectIndex = world.Objects.size();
        world.Objects.push_back(MakeImportedCornellObject(
            2003,
            2,
            mesh,
            std::string(BaseIdentity) + "#tall-block",
            whiteParts[2]));
        world.Objects.push_back(MakeImportedCornellObject(
            2004,
            3,
            mesh,
            std::string(BaseIdentity) + "#green-wall",
            mesh->mSubMeshes[1]));
        world.Objects.push_back(MakeImportedCornellObject(
            2005,
            4,
            mesh,
            std::string(BaseIdentity) + "#red-wall",
            mesh->mSubMeshes[2]));
        world.Objects.push_back(MakeImportedCornellObject(
            2006,
            5,
            mesh,
            std::string(BaseIdentity) + "#ceiling-light",
            mesh->mSubMeshes[3]));
        if (world.Objects.size() != 6u ||
            result.ShortBlockObjectIndex >= world.Objects.size() ||
            result.TallBlockObjectIndex >= world.Objects.size())
        {
            throw std::runtime_error("Imported Cornell six-object split is incomplete");
        }
        return result;
    }

    struct CornellAaCaptureCase
    {
        std::string_view Id;
        HEngine::AntiAliasingMode Mode = HEngine::AntiAliasingMode::None;
        uint32_t MsaaSamples = 4;
        uint32_t CaptureSupersampleScale = 1;
        float TaaHistoryWeight = 0.92f;
        float TaaSharpness = 0.15f;
    };

    struct CornellAaCaptureResult
    {
        std::filesystem::path LinearPfm;
        std::filesystem::path FinalPng;
    };

    [[nodiscard]] bool ContainsPass(
        std::span<const std::string> passes,
        std::string_view name)
    {
        return std::ranges::find(passes, name) != passes.end();
    }

    [[nodiscard]] bool ContainsPassPrefix(
        std::span<const std::string> passes,
        std::string_view prefix)
    {
        return std::ranges::any_of(
            passes,
            [prefix](const std::string& pass)
            {
                return pass.starts_with(prefix);
            });
    }

    void ValidateCornellAaPassContract(
        const CornellAaCaptureCase& captureCase,
        const HEngine::AntiAliasingSelection& selection,
        const HEngine::SceneRendererStats& stats)
    {
        const uint32_t expectedSampleCount =
            captureCase.Mode == HEngine::AntiAliasingMode::Msaa
            ? captureCase.MsaaSamples
            : 1u;
        if (selection.Requested.Mode != captureCase.Mode ||
            selection.Effective.Mode != captureCase.Mode ||
            selection.Requested.CaptureSupersampleScale !=
                captureCase.CaptureSupersampleScale ||
            selection.Effective.CaptureSupersampleScale !=
                captureCase.CaptureSupersampleScale ||
            selection.bFallback || selection.bBypassed ||
            stats.RequestedAntiAliasing != captureCase.Mode ||
            stats.EffectiveAntiAliasing != captureCase.Mode ||
            stats.RasterSampleCount != expectedSampleCount ||
            stats.bAntiAliasingFallback)
        {
            throw std::runtime_error(
                "Cornell AA selection/sample contract failed for " +
                std::string(captureCase.Id));
        }

        const std::span<const std::string> passes(stats.CompiledPasses);
        const bool bHasFxaa = ContainsPass(passes, "SceneFXAA");
        const bool bHasSmaaEdges = ContainsPass(
            passes,
            "SceneSMAAEdgeDetection");
        const bool bHasSmaaWeights = ContainsPass(
            passes,
            "SceneSMAABlendWeights");
        const bool bHasSmaaBlend = ContainsPass(
            passes,
            "SceneSMAANeighborhoodBlend");
        const bool bHasTaa = ContainsPass(passes, "SceneTAA");
        const bool bHasMsaaDepth = ContainsPass(
            passes,
            "SceneDepthPrepassMSAA");
        const bool bHasMsaaComposite = ContainsPass(
            passes,
            "GICompositeMSAA");
        const bool bHasMsaaResolve = ContainsPass(
            passes,
            "SceneRenderer Resolved HDR Scene Color Resolve");
        const bool bHasUnexpectedSmaa = ContainsPassPrefix(
            passes,
            "SceneSMAA") &&
            !(bHasSmaaEdges && bHasSmaaWeights && bHasSmaaBlend);

        bool bPassContract = false;
        switch (captureCase.Mode)
        {
        case HEngine::AntiAliasingMode::None:
            bPassContract = !bHasFxaa && !bHasSmaaEdges && !bHasSmaaWeights &&
                !bHasSmaaBlend && !bHasTaa && !bHasMsaaDepth &&
                !bHasMsaaComposite && !bHasMsaaResolve;
            break;
        case HEngine::AntiAliasingMode::Fxaa:
            bPassContract = bHasFxaa && !bHasSmaaEdges && !bHasSmaaWeights &&
                !bHasSmaaBlend && !bHasTaa && !bHasMsaaDepth &&
                !bHasMsaaComposite && !bHasMsaaResolve;
            break;
        case HEngine::AntiAliasingMode::Smaa:
            bPassContract = !bHasFxaa && bHasSmaaEdges && bHasSmaaWeights &&
                bHasSmaaBlend && !bHasTaa && !bHasMsaaDepth &&
                !bHasMsaaComposite && !bHasMsaaResolve;
            break;
        case HEngine::AntiAliasingMode::Taa:
            bPassContract = !bHasFxaa && !bHasSmaaEdges && !bHasSmaaWeights &&
                !bHasSmaaBlend && bHasTaa && !bHasMsaaDepth &&
                !bHasMsaaComposite && !bHasMsaaResolve;
            break;
        case HEngine::AntiAliasingMode::Msaa:
            bPassContract = !bHasFxaa && !bHasSmaaEdges && !bHasSmaaWeights &&
                !bHasSmaaBlend && !bHasTaa && bHasMsaaDepth &&
                bHasMsaaComposite && bHasMsaaResolve;
            break;
        }
        if (!bPassContract || bHasUnexpectedSmaa)
        {
            std::ostringstream error;
            error << "Cornell AA pass contract failed for " << captureCase.Id
                  << "; passes=";
            for (const std::string& pass : passes)
                error << pass << ',';
            throw std::runtime_error(error.str());
        }
    }

    [[nodiscard]] CornellAaCaptureResult CaptureCornellAaCase(
        HEngine::RenderDevice& renderDevice,
        const HEngine::GlobalIlluminationSettings& giSettings,
        const CornellAaCaptureCase& captureCase,
        uint32_t width,
        uint32_t height,
        uint32_t frameCount,
        const std::filesystem::path& outputDirectory,
        std::string_view outputStem)
    {
        ImportedCornellWorld importedCornell = BuildImportedCornellWorld();
        const std::filesystem::path dxilDirectory =
            HEngine::ConfigManager::GetEngineGeneratedContentFullPath(
                "Shaders/CACHES/DXIL");
        HEngine::AntiAliasingSettings aaSettings;
        aaSettings.Mode = captureCase.Mode;
        aaSettings.MsaaSamples = captureCase.MsaaSamples;
        aaSettings.TaaHistoryWeight = captureCase.TaaHistoryWeight;
        aaSettings.TaaSharpness = captureCase.TaaSharpness;
        aaSettings.CaptureSupersampleScale =
            captureCase.CaptureSupersampleScale;
        auto renderer = std::make_unique<HEngine::SceneRenderer>(
            HEngine::SceneRendererDesc{
                .Device = renderDevice.GetDevice(),
                .FramesInFlight = renderDevice.GetCapabilities().FramesInFlight,
                .ShaderCacheDirectory = dxilDirectory,
                .AssetRootDirectory =
                    HEngine::ConfigManager::GetInstance().GetRootFolder(),
                .EngineAssetRootDirectory = HEngine::ConfigManager::GetInstance()
                    .GetEngineContentFolder(),
                .Backend = renderDevice.GetBackend(),
                .Capabilities = renderDevice.GetCapabilities(),
                .GlobalIllumination = giSettings,
                .AntiAliasing = aaSettings,
                .bEnableGpuTiming = true,
            });
        renderer->ConfigureOffscreenOutput(
            width,
            height,
            "Cornell AA " + std::string(captureCase.Id));

        const CornellAaCaptureResult paths{
            .LinearPfm = outputDirectory /
                (std::string(outputStem) + "-linear.pfm"),
            .FinalPng = outputDirectory /
                (std::string(outputStem) + "-final.png"),
        };
        HEngine::TextureReadbackQueue linearReadback(renderDevice.GetDevice());
        HEngine::TextureReadbackHandle linearCapture;
        HEngine::TextureReadbackQueue ldrReadback(renderDevice.GetDevice());
        HEngine::TextureReadbackHandle ldrCapture;
        RenderOffscreenFrames(
            renderDevice,
            *renderer,
            importedCornell.World,
            width,
            height,
            frameCount,
            &linearReadback,
            &linearCapture,
            paths.LinearPfm,
            &ldrReadback,
            &ldrCapture,
            paths.FinalPng);
        renderDevice.WaitForIdle();
        linearReadback.Drain(std::numeric_limits<uint64_t>::max());
        ldrReadback.Drain(std::numeric_limits<uint64_t>::max());
        const HEngine::TextureReadbackResult linearResult =
            linearReadback.GetResult(linearCapture);
        const HEngine::TextureReadbackResult ldrResult =
            ldrReadback.GetResult(ldrCapture);
        if (linearResult.Status != HEngine::TextureReadbackStatus::Complete ||
            ldrResult.Status != HEngine::TextureReadbackStatus::Complete ||
            !std::filesystem::is_regular_file(paths.LinearPfm) ||
            !std::filesystem::is_regular_file(paths.FinalPng) ||
            std::filesystem::file_size(paths.FinalPng) < 1024u)
        {
            throw std::runtime_error(
                "Cornell AA capture failed for " + std::string(captureCase.Id) +
                ": linear=" + linearResult.Error + " ldr=" + ldrResult.Error);
        }
        (void)ValidatePfmQuadrantCoverage(
            paths.LinearPfm,
            "Cornell AA " + std::string(captureCase.Id),
            true);
        renderer->CollectGpuTimingStats();
        const HEngine::SceneRendererStats& stats = renderer->GetStats();
        ValidateCornellAaPassContract(
            captureCase,
            renderer->GetAntiAliasingSelection(),
            stats);
        if (stats.GpuRenderGraphSampleCount != frameCount ||
            stats.GiStats.Technique !=
                HEngine::GlobalIlluminationTechniqueKind::HardwareRayTracing ||
            stats.RayTracingInstanceCount != 6u ||
            stats.RayTracingExcludedInstanceCount != 0u ||
            stats.RayTracingEmissiveTriangleCount != 2u)
        {
            throw std::runtime_error(
                "Cornell AA fixed RTGI/frame contract failed for " +
                std::string(captureCase.Id));
        }
        std::cout
            << "CORNELL_AA_CASE_PASS: id=" << captureCase.Id
            << " mode=" << HEngine::ToString(captureCase.Mode)
            << " resolution=" << width << 'x' << height
            << " frames=" << frameCount
            << " rasterSamples=" << stats.RasterSampleCount
            << " pfm=" << paths.LinearPfm.generic_string()
            << " png=" << paths.FinalPng.generic_string() << '\n';
        linearReadback.Release(linearCapture);
        ldrReadback.Release(ldrCapture);
        renderer.reset();
        return paths;
    }

    [[nodiscard]] HEngine::TextureReadbackResult ExportOfflineReferencePfm(
        HEngine::RenderDevice& renderDevice,
        HEngine::SceneRenderer& renderer,
        HEngine::IOfflineViewportRendererSession& session,
        HEngine::RenderWorld& world,
        uint32_t width,
        uint32_t height,
        const std::filesystem::path& outputPath,
        HEngine::OfflineViewportAov aov = HEngine::OfflineViewportAov::Beauty,
        HEngine::OfflineViewportOutputFormat format = HEngine::OfflineViewportOutputFormat::Pfm)
    {
        const HEngine::TextureReadbackHandle exportHandle = session.RequestExport(
            aov,
            outputPath,
            format);
        const auto started = std::chrono::steady_clock::now();
        while (true)
        {
            // Frame-slot completion drives readback collection. Do not replace
            // a real completion value with max() or idle the device for export.
            RenderOffscreenFrames(renderDevice, renderer, world, width, height, 1);
            const auto result = session.GetExportResult(exportHandle);
            if (result.Status == HEngine::TextureReadbackStatus::Complete || result.Status == HEngine::TextureReadbackStatus::Failed)
            {
                session.ReleaseExport(exportHandle);
                return result;
            }
            if (std::chrono::steady_clock::now() - started > std::chrono::seconds(5))
                throw std::runtime_error("Asynchronous offline export timed out");
        }
    }

    void RunMaterialSamplingSmoke(HEngine::RenderDevice& device)
    {
        const auto support = HEngine::QueryHardwareRayTracingSupport(device.GetCapabilities());
        if (!support.bAccelerationStructures || !support.bPipelines)
            throw std::runtime_error("Material sampling smoke requires actual hardware RT pipelines: " + support.Reason);
        constexpr uint32_t width = 64, height = 64;
        const std::string runName = std::string(HEngine::ToString(device.GetBackend())) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto directory = std::filesystem::path(HE_DEFAULT_PROJECT_DIR).parent_path() /
            "out/agent/captures/material-sampling" / runName;
        std::filesystem::create_directories(directory);
        std::cout << "MATERIAL_SAMPLING_OUTPUT: " << directory.string() << std::endl;
        auto world = BuildHardwareRtSmokeWorld();
        world.Objects.resize(1);
        uint32_t sectionCount = 1;
        const char* shaderFormat = device.GetBackend() == HEngine::RenderBackend::Vulkan ? "SPIRV" : "DXIL";
        HEngine::SceneRenderer renderer({.Device = device.GetDevice(), .FramesInFlight = device.GetCapabilities().FramesInFlight,
            .ShaderCacheDirectory = HEngine::ConfigManager::GetEngineGeneratedContentFullPath(std::string("Shaders/CACHES/") + shaderFormat),
            .AssetRootDirectory = HEngine::ConfigManager::GetInstance().GetRootFolder(),
            .EngineAssetRootDirectory = HEngine::ConfigManager::GetInstance().GetEngineContentFolder(),
            .Backend = device.GetBackend(), .Capabilities = device.GetCapabilities(), .bEnableGpuTiming = true});
        renderer.ConfigureOffscreenOutput(width, height, "Engine material sampling reference fixture");
        renderer.ActivateOfflineViewportRenderer("he.viewport.path-tracing");
        auto* sessionPointer = renderer.GetOfflineViewportRendererSession();
        if (!sessionPointer || renderer.GetActiveOfflineViewportRendererId() != "he.viewport.path-tracing")
            throw std::runtime_error("Material sampling smoke did not activate the linked reference provider");
        auto& session = *sessionPointer;
        auto settings = session.GetOfflineSettings();
        settings.bInteractive = false;
        settings.FixedWidth = width; settings.FixedHeight = height;
        settings.MaxBounces = 1; settings.bDenoise = false;
        const auto converge = [&](uint32_t target)
        {
            settings.TargetSamples = target; session.ApplyOfflineSettings(settings);
            const auto started = std::chrono::steady_clock::now();
            while (true)
            {
                const auto snapshotFrame = world.FrameNumber;
                RenderOffscreenFrames(device, renderer, world, width, height, 1);
                world.FrameNumber = snapshotFrame;
                const auto progress = session.GetOfflineProgress();
                if (progress.State == HEngine::OfflineViewportSessionState::Failed)
                    throw std::runtime_error(progress.Error);
                if (progress.IncludedInstances != sectionCount || progress.ExcludedInstances != 0 || progress.InFlight > 1)
                    throw std::runtime_error("Material sampling fixture lost geometry or exceeded its flight bound");
                if (progress.AccumulatedSamples == target) break;
                if (std::chrono::steady_clock::now() - started > std::chrono::seconds(60))
                    throw std::runtime_error("Material sampling reference convergence timed out");
            }
        };
        const auto capture = [&](const std::string& name, HEngine::OfflineViewportAov aov)
        {
            const auto result = ExportOfflineReferencePfm(device, renderer, session, world, width, height,
                directory / (name + ".pfm"), aov);
            if (result.Status != HEngine::TextureReadbackStatus::Complete) throw std::runtime_error(result.Error);
            return ReadLinearPfm(directory / (name + ".pfm"));
        };
        // Analytic image checks isolate texture colour space, UV interpolation,
        // tangent-space normals and alpha-hit rejection from convergence noise.
        const std::string textureRoot = "Saved/RenderingTests/MaterialSampling/" + runName + "/";
        std::filesystem::create_directories(HEngine::ConfigManager::GetProjectFullPath(textureRoot));
        const auto writeTexture = [&](const std::string& name, std::array<uint8_t, 4> first)
        {
            const std::string path = textureRoot + name + ".tga";
            std::ofstream stream(HEngine::ConfigManager::GetProjectFullPath(path), std::ios::binary);
            std::array<uint8_t, 18> header{};
            header[2] = 2; header[12] = 2; header[14] = 2; header[16] = 32; header[17] = 0x28;
            stream.write(reinterpret_cast<const char*>(header.data()), header.size());
            for (const auto rgba : {first, std::array<uint8_t, 4>{0,255,0,255}, std::array<uint8_t, 4>{255,0,0,255}, std::array<uint8_t, 4>{0,0,255,255}})
            {
                const std::array<uint8_t, 4> bgra = {rgba[2], rgba[1], rgba[0], rgba[3]};
                stream.write(reinterpret_cast<const char*>(bgra.data()), bgra.size());
            }
            return path;
        };
        const auto colorTexture = writeTexture("color-alpha", {128,64,32,0});
        const auto normalTexture = writeTexture("normal", {191,128,238,255});
        world = BuildHardwareRtSmokeWorld();
        world.Objects.resize(1); sectionCount = 1;
        world.SceneGeneration = 0x505242544558ull;
        world.Camera.Position = {0.f, 2.f, 0.f};
        world.Camera.View = glm::lookAt(world.Camera.Position, glm::vec3(0), glm::vec3(0,0,-1));
        world.Camera.ViewProjection = world.Camera.Projection * world.Camera.View;
        world.Camera.NonJitteredViewProjection = world.Camera.PreviousViewProjection =
            world.Camera.PreviousNonJitteredViewProjection = world.Camera.ViewProjection;
        auto& section = world.Objects[0].Sections[0];
        for (auto& vertex : section.SubMeshResource->mStaticVertices) vertex.TexCoord = {.25f,.25f};
        auto& material = section.MaterialInstance;
        material.ParameterValues.clear(); material.LegacyTextures = {};
        material.BaseColor = glm::vec4(1); material.Emissive = glm::vec3(1); material.Metallic = 0;
        material.LegacyTextures[0] = {.AssetPath = colorTexture, .bEnabled = true};
        material.LegacyTextures[4] = {.AssetPath = colorTexture, .bEnabled = true};
        settings.FixedWidth = settings.FixedHeight = 64;
        settings.bDirectOnly = true; settings.bUseSceneEnvironment = false; settings.EnvironmentRadiance = glm::vec3(0);
        converge(4);
        const auto srgb = [](float value) { value /= 255.f; return value <= .04045f ? value / 12.92f : std::pow((value + .055f) / 1.055f, 2.4f); };
        const auto checkConstant = [&](const LinearRgbImage& image, glm::vec3 expected, float tolerance, const char* label)
        {
            float maximumError = 0;
            if (image.Pixels.empty()) throw std::runtime_error(std::string(label) + " empty readback");
            for (size_t i = 0; i < image.Pixels.size(); ++i)
            {
                if (!std::isfinite(image.Pixels[i])) throw std::runtime_error(std::string(label) + " non-finite readback");
                maximumError = std::max(maximumError, std::abs(image.Pixels[i] - expected[static_cast<int>(i % 3)]));
            }
            if (maximumError > tolerance) throw std::runtime_error(std::string(label) + " error=" + std::to_string(maximumError));
        };
        const glm::vec3 expectedColor(srgb(128), srgb(64), srgb(32));
        checkConstant(capture("texture-emission", HEngine::OfflineViewportAov::Beauty), expectedColor, .002f, "sRGB emission/UV");
        checkConstant(capture("texture-albedo", HEngine::OfflineViewportAov::Albedo), expectedColor, .002f, "sRGB albedo/UV");
        material.LegacyTextures[1] = {.AssetPath = normalTexture, .bEnabled = true};
        ++material.Revision; ++world.FrameNumber;
        converge(4);
        const glm::vec3 expectedNormal = glm::normalize(glm::vec3(191.f/255.f*2.f-1.f, 238.f/255.f*2.f-1.f, 128.f/255.f*2.f-1.f));
        checkConstant(capture("texture-normal", HEngine::OfflineViewportAov::WorldNormal), expectedNormal, .003f, "Tangent-space normal");
        section.AlphaMode = material.AlphaMode = HEngine::RenderMaterialAlphaMode::Mask;
        settings.EnvironmentRadiance = glm::vec3(.2f,.3f,.4f);
        ++material.Revision; ++world.FrameNumber;
        converge(4);
        checkConstant(capture("texture-alpha-hole", HEngine::OfflineViewportAov::Beauty), settings.EnvironmentRadiance, .0001f, "Alpha any-hit hole");
        std::cout << "MATERIAL_SAMPLING_BASE_TEXTURE_NORMAL_ALPHA_PASS\n";
        // These are actual reference-ray AOV readbacks. Realtime RTGI still
        // uses its documented constant-material approximation; raster output
        // and the shared record ABI are not evidence of realtime RT sampling.
        const auto samplingTexture = writeTexture("sampling-rgba", {128,64,32,255});
        const auto setUvsAndColor = [&](glm::vec2 uv0, glm::vec2 uv1, glm::vec4 color)
        {
            for (auto& vertex : section.SubMeshResource->mStaticVertices)
            {
                vertex.TexCoord = uv0;
                vertex.TexCoord1 = uv1;
                vertex.Color = color;
            }
        };
        const auto checkSampling = [&](const std::string& name, glm::vec3 expected,
                                       HEngine::OfflineViewportAov aov = HEngine::OfflineViewportAov::Albedo,
                                       float tolerance = .002f)
        {
            // In-memory vertex edits must invalidate the BLAS/vertex snapshot,
            // not just the material table. The user's scene file is untouched.
            ++world.SceneGeneration; ++material.Revision; ++world.FrameNumber;
            converge(4);
            checkConstant(capture(name, aov), expected, tolerance, name.c_str());
            std::cout << "MATERIAL_SAMPLING_CHECK_PASS: " << name
                << " tolerance=" << tolerance << " lod=0\n";
        };
        section.AlphaMode = material.AlphaMode = HEngine::RenderMaterialAlphaMode::Opaque;
        material.LegacyTextures = {};
        material.BaseColor = glm::vec4(1); material.Emissive = glm::vec3(0);
        material.LegacyTextures[0] = {.AssetPath = samplingTexture, .bEnabled = true};
        auto& sampling = material.LegacyTextures[0].Sampling;
        sampling.magFilter = 9728;
        setUvsAndColor({.25f,.25f}, {.75f,.25f}, glm::vec4(1));
        checkSampling("sampling-uv0", expectedColor);
        sampling.texCoord = 1;
        checkSampling("sampling-uv1", {0,1,0});
        setUvsAndColor({.75f,.25f}, {.25f,.25f}, glm::vec4(1));
        sampling.scale = {2,1}; sampling.rotation = 1.5707963267948966f;
        sampling.offset = {1,-.25f};
        checkSampling("sampling-uv1-rotate-scale-offset", {0,1,0});
        sampling = {}; sampling.texCoord = 1; sampling.magFilter = 9728;
        sampling.offset = {1,0};
        checkSampling("sampling-wrap-repeat", expectedColor);
        sampling.wrapS = 33071;
        checkSampling("sampling-wrap-clamp", {0,1,0});
        sampling.wrapS = 33648;
        checkSampling("sampling-wrap-mirror", {0,1,0});
        sampling.offset = {-.5f,0};
        checkSampling("sampling-wrap-negative-mirror", expectedColor);
        sampling.wrapS = 10497;
        checkSampling("sampling-wrap-negative-repeat", {0,1,0});
        sampling = {}; sampling.texCoord = 1; sampling.magFilter = 9728;
        // Keep nearest away from the texel boundary: barycentric roundoff at
        // exactly 0.5 can legitimately select either texel before spp averaging.
        setUvsAndColor({.25f,.25f}, {.625f,.25f}, glm::vec4(1));
        checkSampling("sampling-lod0-nearest", {0,1,0});
        sampling.magFilter = 9729;
        setUvsAndColor({.25f,.25f}, {.5f,.25f}, glm::vec4(1));
        const glm::vec3 linearAverage = (expectedColor + glm::vec3(0,1,0)) * .5f;
        checkSampling("sampling-lod0-linear-srgb-before-filter", linearAverage);
        // LOD is deliberately fixed to zero in reference rays. All six valid
        // minification modes must leave this magnification result unchanged;
        // this is not a mip-selection/minification quality test.
        for (const int minFilter : {9728,9729,9984,9985,9986,9987})
        {
            sampling.minFilter = minFilter;
            checkSampling("sampling-lod0-min-mode-" + std::to_string(minFilter), linearAverage);
        }
        sampling.magFilter = 9728;
        setUvsAndColor({.75f,.25f}, {.25f,.25f}, {.5f,.25f,.75f,1});
        material.BaseColor = {.8f,.6f,.4f,1};
        checkSampling("sampling-vertex-rgb", expectedColor * glm::vec3(.5f,.25f,.75f) * glm::vec3(.8f,.6f,.4f));
        material.BaseColor = glm::vec4(1);
        material.Emissive = glm::vec3(1);
        material.LegacyTextures[4] = material.LegacyTextures[0];
        settings.EnvironmentRadiance = glm::vec3(0);
        checkSampling("sampling-emission-independent-of-vertex-rgb", expectedColor,
            HEngine::OfflineViewportAov::Beauty);
        section.AlphaMode = material.AlphaMode = HEngine::RenderMaterialAlphaMode::Mask;
        material.AlphaCutoff = .5f; material.BaseColor.a = .8f;
        settings.EnvironmentRadiance = {.2f,.3f,.4f};
        setUvsAndColor({.75f,.25f}, {.25f,.25f}, {1,1,1,.4f});
        checkSampling("sampling-vertex-alpha-hole", settings.EnvironmentRadiance,
            HEngine::OfflineViewportAov::Beauty, .0001f);
        setUvsAndColor({.75f,.25f}, {.25f,.25f}, glm::vec4(1));
        checkSampling("sampling-vertex-alpha-hit", expectedColor);
        material.LegacyTextures[0].AssetPath = colorTexture;
        checkSampling("sampling-uv1-texture-alpha-hole", settings.EnvironmentRadiance,
            HEngine::OfflineViewportAov::Beauty, .0001f);
        sampling.offset = {.5f,0};
        checkSampling("sampling-uv1-transformed-alpha-hit", {0,1,0});
        std::cout << "MATERIAL_SAMPLING_SMOKE_PASS: actual-reference-ray-AOVs,UV1,transform,vertex-color,alpha,wrap,LOD0-filter,sRGB-before-filter"
            << " NOT_VERIFIED=RT-minification-mips,realtime-RT-textures,depth-shadow-sampling\n";
        setUvsAndColor({.25f,.25f}, {.25f,.25f}, glm::vec4(1));
        ++world.SceneGeneration;
        device.WaitForIdle();
    }

    void RunPbrTestPathTracingSmoke(HEngine::RenderDevice& device)
    {
        constexpr uint32_t width = 640, height = 360;
        const auto scenePath = HEngine::ConfigManager::GetProjectFullPath("Assets/Scenes/PBRTest.he");
        const auto readScene = [&]() { std::ifstream stream(scenePath, std::ios::binary); return std::string(std::istreambuf_iterator<char>(stream), {}); };
        const auto originalScene = readScene();
        auto level = HEngine::CreateRef<HEngine::Level>();
        // This non-Editor host omits Editor-only components during deserialization.
        // Apply this fixture's authored visibility to an in-memory document first.
        auto document = HEngine::Json::parse(originalScene);
        for (auto it = document.begin(); it != document.end();)
            if (it.value().contains("EditorEntityStateComponent") &&
                !it.value()["EditorEntityStateComponent"].value("bVisible", true)) it = document.erase(it);
            else ++it;
        if (!HEngine::SceneSerializer(level).DeserializeFromJson(document)) throw std::runtime_error("PBRTest deserialization failed");
        auto cameraEntity = level->GetPrimaryCameraEntity();
        auto& camera = cameraEntity.GetComponent<HEngine::CameraComponent>().Camera;
        const auto& transform = cameraEntity.GetComponent<HEngine::TransformComponent>();
        HEngine::RenderWorldExtractor extractor;
        auto world = extractor.Extract(*level, HEngine::RenderViewDescription{
            .View = glm::inverse(transform.GetTransform()), .Projection = camera.GetProjection(),
            .CameraPosition = transform.Translation, .NearPlane = camera.GetPerspectiveNearClip(),
            .FarPlane = camera.GetPerspectiveFarClip(), .bEditorView = true, .FrameNumber = 1});
        const auto directory = std::filesystem::path(HE_DEFAULT_PROJECT_DIR).parent_path() / "out/agent/captures/pt-pbrtest";
        std::filesystem::create_directories(directory);
        std::ofstream manifest(directory / "materials.txt");
        uint32_t sectionCount = 0;
        for (const auto& object : world.Objects)
            for (const auto& section : object.Sections)
            {
                ++sectionCount;
                const auto& material = section.MaterialInstance;
                manifest << "section=" << sectionCount << " material=" << material.AssetPath << " alpha=" << int(section.AlphaMode)
                    << " doubleSided=" << section.bDoubleSided << " metallic=" << material.Metallic << " roughness=" << material.Roughness << '\n';
                for (const auto& texture : material.LegacyTextures) manifest << "  " << texture.bEnabled << ' ' << texture.AssetPath << '\n';
            }
        if (sectionCount != 25 || world.DirectionalLights.size() != 1 || world.Environment.IblIntensity != 0)
            throw std::runtime_error("PBRTest fixture changed: sections=" + std::to_string(sectionCount) +
                " directional=" + std::to_string(world.DirectionalLights.size()) + " environment=" + std::to_string(world.Environment.IblIntensity));
        HEngine::SceneRenderer renderer({.Device = device.GetDevice(), .FramesInFlight = device.GetCapabilities().FramesInFlight,
            .ShaderCacheDirectory = HEngine::ConfigManager::GetEngineGeneratedContentFullPath("Shaders/CACHES/DXIL"),
            .AssetRootDirectory = HEngine::ConfigManager::GetInstance().GetRootFolder(),
            .EngineAssetRootDirectory = HEngine::ConfigManager::GetInstance().GetEngineContentFolder(),
            .Backend = device.GetBackend(), .Capabilities = device.GetCapabilities(), .bEnableGpuTiming = true});
        renderer.ConfigureOffscreenOutput(width, height, "PBRTest original scene PT");
        renderer.ActivateOfflineViewportRenderer("he.viewport.path-tracing");
        auto& session = *renderer.GetOfflineViewportRendererSession();
        auto settings = session.GetOfflineSettings();
        settings.bInteractive = false;
        settings.FixedWidth = width; settings.FixedHeight = height;
        settings.MaxBounces = 8; settings.bDenoise = true; settings.DenoiseIntervalMilliseconds = 100;
        std::ofstream timing(directory / "frames.csv");
        timing << "stage,frame,cpuMs,gpuMs,traceMs,minSpp,avgSpp,inFlight,denoisedSpp\n";
        uint32_t frame = 0;
        const auto converge = [&](uint32_t target)
        {
            settings.TargetSamples = target; session.ApplyOfflineSettings(settings);
            const auto started = std::chrono::steady_clock::now();
            while (true)
            {
                const auto tick = std::chrono::steady_clock::now();
                const auto snapshotFrame = world.FrameNumber;
                RenderOffscreenFrames(device, renderer, world, width, height, 1);
                world.FrameNumber = snapshotFrame;
                const auto progress = session.GetOfflineProgress();
                timing << target << ',' << ++frame << ',' << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tick).count()
                    << ',' << renderer.GetStats().GpuFrameSpanMilliseconds << ',' << progress.TracingMilliseconds << ',' << progress.AccumulatedSamples
                    << ',' << progress.AverageSamples << ',' << progress.InFlight << ',' << progress.DenoisedSamples << '\n';
                if (progress.State == HEngine::OfflineViewportSessionState::Failed) throw std::runtime_error(progress.Error);
                if (progress.IncludedInstances != sectionCount || progress.ExcludedInstances != 0 || progress.InFlight > 1)
                    throw std::runtime_error("PBRTest lost geometry or exceeded its flight bound");
                if (progress.AccumulatedSamples == target && (!settings.bDenoise || progress.DenoisedSamples == target)) break;
                if (std::chrono::steady_clock::now() - started > std::chrono::seconds(120)) throw std::runtime_error("PBRTest failed to converge within timeout");
            }
        };
        const auto capture = [&](const std::string& name, HEngine::OfflineViewportAov aov)
        {
            const auto path = directory / (name + ".pfm");
            const auto result = ExportOfflineReferencePfm(device, renderer, session, world, width, height, path, aov);
            if (result.Status != HEngine::TextureReadbackStatus::Complete) throw std::runtime_error(result.Error);
            return ReadLinearPfm(path);
        };
        for (uint32_t target : {1u, 4u, 16u, 64u, 256u})
        {
            converge(target);
            const auto raw = capture(std::to_string(target) + "-raw", HEngine::OfflineViewportAov::Beauty);
            (void)capture(std::to_string(target) + "-denoised", HEngine::OfflineViewportAov::DenoisedBeauty);
            if (target == 1)
            {
                (void)capture("initial-albedo-1", HEngine::OfflineViewportAov::Albedo);
                (void)capture("initial-normal-1", HEngine::OfflineViewportAov::WorldNormal);
            }
            if (!std::ranges::any_of(raw.Pixels, [](float value) { return value > 100.f; })) throw std::runtime_error("PBRTest is missing its directional illumination");
            const auto countsPath = directory / (std::to_string(target) + "-counts.raw");
            const auto countsResult = ExportOfflineReferencePfm(device, renderer, session, world, width, height,
                countsPath, HEngine::OfflineViewportAov::SampleCount, HEngine::OfflineViewportOutputFormat::Raw);
            std::vector<uint32_t> counts(width * height);
            std::ifstream countStream(countsPath, std::ios::binary);
            countStream.read(reinterpret_cast<char*>(counts.data()), counts.size() * sizeof(uint32_t));
            if (countsResult.Status != HEngine::TextureReadbackStatus::Complete || !countStream ||
                !std::ranges::all_of(counts, [target](uint32_t count) { return count == target; }))
                throw std::runtime_error("PBRTest completed spp disagrees with GPU pixel counts");
            std::cout << "PBRTEST_PT_STAGE_PASS spp=" << target << " instances=" << sectionCount << '\n';
        }
        (void)capture("albedo", HEngine::OfflineViewportAov::Albedo);
        (void)capture("normal", HEngine::OfflineViewportAov::WorldNormal);
        settings.bDenoise = false;
        world.DirectionalLights[0].Intensity *= 0.5f;
        ++world.FrameNumber;
        converge(16);
        const auto half = capture("half-light-16", HEngine::OfflineViewportAov::Beauty);
        world.DirectionalLights[0].Intensity *= 2.f;
        ++world.FrameNumber;
        converge(16);
        const auto full = capture("repeat-full-light-16", HEngine::OfflineViewportAov::Beauty);
        double squaredError = 0, energy = 0;
        for (size_t i = 0; i < full.Pixels.size(); ++i)
        {
            const double expected = full.Pixels[i] * .5;
            squaredError += std::pow(half.Pixels[i] - expected, 2);
            energy += expected * expected;
        }
        const double relativeError = std::sqrt(squaredError / std::max(energy, 1e-20));
        if (relativeError > 1e-4) throw std::runtime_error("Directional intensity failed linearity: " + std::to_string(relativeError));
        std::cout << "PBRTEST_PT_LIGHT_LINEARITY_PASS error=" << relativeError << '\n';
        settings.bDenoise = true;
        session.Restart();
        converge(16);
        const auto denoiseEnabledRaw = capture("repeat-denoise-raw-16", HEngine::OfflineViewportAov::Beauty);
        if (denoiseEnabledRaw.Pixels != full.Pixels)
            throw std::runtime_error("Denoiser scheduling changed the raw reference image");
        std::cout << "PBRTEST_PT_DENOISE_RAW_ISOLATION_PASS\n";
        settings.bDenoise = false;
        session.Restart();
        for (uint32_t target : {1u,4u,16u})
        {
            converge(target);
            (void)capture("repeat-stage-" + std::to_string(target), HEngine::OfflineViewportAov::Beauty);
            if (target == 1)
            {
                (void)capture("repeat-albedo-1", HEngine::OfflineViewportAov::Albedo);
                (void)capture("repeat-normal-1", HEngine::OfflineViewportAov::WorldNormal);
            }
        }
        if (ReadLinearPfm(directory / "repeat-stage-16.pfm").Pixels != full.Pixels)
            throw std::runtime_error("Changing target samples changed the raw reference sequence");
        std::cout << "PBRTEST_PT_TARGET_SEQUENCE_PASS\n";
        for (const auto target : {1u, 4u, 16u})
            if (ReadLinearPfm(directory / (std::to_string(target) + "-raw.pfm")).Pixels !=
                ReadLinearPfm(directory / ("repeat-stage-" + std::to_string(target) + ".pfm")).Pixels)
                throw std::runtime_error("Initial and restarted primary-ray sequences differ");
        for (const auto* guide : {"albedo", "normal"})
            if (ReadLinearPfm(directory / (std::string("initial-") + guide + "-1.pfm")).Pixels !=
                ReadLinearPfm(directory / (std::string("repeat-") + guide + "-1.pfm")).Pixels)
                throw std::runtime_error("Initial and restarted primary guides differ");
        std::cout << "PBRTEST_PT_INITIAL_SEQUENCE_PASS\n";
        // Analytic image checks isolate texture colour space, UV interpolation,
        // tangent-space normals and alpha-hit rejection from convergence noise.
        const std::string textureRoot = "Saved/RenderingTests/PtPbr/";
        std::filesystem::create_directories(HEngine::ConfigManager::GetProjectFullPath(textureRoot));
        const auto writeTexture = [&](const std::string& name, std::array<uint8_t, 4> first)
        {
            const std::string path = textureRoot + name + ".tga";
            std::ofstream stream(HEngine::ConfigManager::GetProjectFullPath(path), std::ios::binary);
            std::array<uint8_t, 18> header{};
            header[2] = 2; header[12] = 2; header[14] = 2; header[16] = 32; header[17] = 0x28;
            stream.write(reinterpret_cast<const char*>(header.data()), header.size());
            for (const auto rgba : {first, std::array<uint8_t, 4>{0,255,0,255}, std::array<uint8_t, 4>{255,0,0,255}, std::array<uint8_t, 4>{0,0,255,255}})
            {
                const std::array<uint8_t, 4> bgra = {rgba[2], rgba[1], rgba[0], rgba[3]};
                stream.write(reinterpret_cast<const char*>(bgra.data()), bgra.size());
            }
            return path;
        };
        const auto colorTexture = writeTexture("color-alpha", {128,64,32,0});
        const auto normalTexture = writeTexture("normal", {191,128,238,255});
        world = BuildHardwareRtSmokeWorld();
        world.Objects.resize(1); sectionCount = 1;
        world.SceneGeneration = 0x505242544558ull;
        world.Camera.Position = {0.f, 2.f, 0.f};
        world.Camera.View = glm::lookAt(world.Camera.Position, glm::vec3(0), glm::vec3(0,0,-1));
        world.Camera.ViewProjection = world.Camera.Projection * world.Camera.View;
        world.Camera.NonJitteredViewProjection = world.Camera.PreviousViewProjection =
            world.Camera.PreviousNonJitteredViewProjection = world.Camera.ViewProjection;
        auto& section = world.Objects[0].Sections[0];
        for (auto& vertex : section.SubMeshResource->mStaticVertices) vertex.TexCoord = {.25f,.25f};
        auto& material = section.MaterialInstance;
        material.ParameterValues.clear(); material.LegacyTextures = {};
        material.BaseColor = glm::vec4(1); material.Emissive = glm::vec3(1); material.Metallic = 0;
        material.LegacyTextures[0] = {.AssetPath = colorTexture, .bEnabled = true};
        material.LegacyTextures[4] = {.AssetPath = colorTexture, .bEnabled = true};
        settings.FixedWidth = settings.FixedHeight = 64;
        settings.bDirectOnly = true; settings.bUseSceneEnvironment = false; settings.EnvironmentRadiance = glm::vec3(0);
        converge(4);
        const auto srgb = [](float value) { value /= 255.f; return value <= .04045f ? value / 12.92f : std::pow((value + .055f) / 1.055f, 2.4f); };
        const auto checkConstant = [&](const LinearRgbImage& image, glm::vec3 expected, float tolerance, const char* label)
        {
            float maximumError = 0;
            if (image.Pixels.empty()) throw std::runtime_error(std::string(label) + " empty readback");
            for (size_t i = 0; i < image.Pixels.size(); ++i)
            {
                if (!std::isfinite(image.Pixels[i])) throw std::runtime_error(std::string(label) + " non-finite readback");
                maximumError = std::max(maximumError, std::abs(image.Pixels[i] - expected[static_cast<int>(i % 3)]));
            }
            if (maximumError > tolerance) throw std::runtime_error(std::string(label) + " error=" + std::to_string(maximumError));
        };
        const glm::vec3 expectedColor(srgb(128), srgb(64), srgb(32));
        checkConstant(capture("texture-emission", HEngine::OfflineViewportAov::Beauty), expectedColor, .002f, "sRGB emission/UV");
        checkConstant(capture("texture-albedo", HEngine::OfflineViewportAov::Albedo), expectedColor, .002f, "sRGB albedo/UV");
        material.LegacyTextures[1] = {.AssetPath = normalTexture, .bEnabled = true};
        ++material.Revision; ++world.FrameNumber;
        converge(4);
        const glm::vec3 expectedNormal = glm::normalize(glm::vec3(191.f/255.f*2.f-1.f, 238.f/255.f*2.f-1.f, 128.f/255.f*2.f-1.f));
        checkConstant(capture("texture-normal", HEngine::OfflineViewportAov::WorldNormal), expectedNormal, .003f, "Tangent-space normal");
        section.AlphaMode = material.AlphaMode = HEngine::RenderMaterialAlphaMode::Mask;
        settings.EnvironmentRadiance = glm::vec3(.2f,.3f,.4f);
        ++material.Revision; ++world.FrameNumber;
        converge(4);
        checkConstant(capture("texture-alpha-hole", HEngine::OfflineViewportAov::Beauty), settings.EnvironmentRadiance, .0001f, "Alpha any-hit hole");
        std::cout << "PBRTEST_PT_TEXTURE_NORMAL_ALPHA_PASS\n";
        // Delta lights have no light-selection noise. Test both positive energy
        // and updates to the light table without rebuilding geometry.
        settings.EnvironmentRadiance = glm::vec3(0);
        section.AlphaMode = material.AlphaMode = HEngine::RenderMaterialAlphaMode::Opaque;
        material.LegacyTextures = {};
        material.BaseColor = glm::vec4(.8f, .2f, .05f, 1.f);
        material.Emissive = glm::vec3(0); material.Metallic = 1.f; material.Roughness = .4f;
        world.PointLights.clear(); world.SpotLights.clear();
        world.DirectionalLights = {{.Direction = {0,-1,0}, .Intensity = 1.f}};
        ++material.Revision; ++world.FrameNumber;
        converge(4);
        const auto metal = capture("metal-directional", HEngine::OfflineViewportAov::Beauty);
        const auto peak = [](const LinearRgbImage& image) { return *std::ranges::max_element(image.Pixels); };
        if (peak(metal) < .5f) throw std::runtime_error("Metal lost its GGX reflection");
        const auto blasBuilds = renderer.GetStats().RayTracingBlasBuildCount;
        material.Roughness = .8f; ++material.Revision; ++world.FrameNumber;
        converge(4);
        if (peak(capture("metal-rough", HEngine::OfflineViewportAov::Beauty)) >= peak(metal) * .5f)
            throw std::runtime_error("GGX roughness did not widen the highlight");
        world.DirectionalLights.clear();
        world.PointLights = {{.Position = {0,2,0}, .Intensity = 4.f, .Radius = 100.f}};
        ++world.FrameNumber; converge(4);
        const auto point = capture("metal-point", HEngine::OfflineViewportAov::Beauty);
        if (peak(point) <= .01f) throw std::runtime_error("Point light missing");
        world.PointLights.clear();
        world.SpotLights = {{.Position = {0,2,0}, .Intensity = 4.f, .Radius = 100.f,
            .Direction = {0,-1,0}, .InnerConeCosine = .5f, .OuterConeCosine = .4f}};
        ++world.FrameNumber; converge(4);
        const auto spot = capture("metal-spot", HEngine::OfflineViewportAov::Beauty);
        if (peak(spot) <= .01f) throw std::runtime_error("Spot light missing");
        world.SpotLights[0].Direction = {0,1,0};
        ++world.FrameNumber; converge(4);
        checkConstant(capture("spot-away", HEngine::OfflineViewportAov::Beauty), glm::vec3(0), .0001f, "Spot direction update");
        if (renderer.GetStats().RayTracingBlasBuildCount != blasBuilds)
            throw std::runtime_error("Material or light updates rebuilt unchanged BLAS");
        std::cout << "PBRTEST_PT_METAL_ROUGHNESS_LIGHT_UPDATES_PASS\n";

        renderer.DeactivateOfflineViewportRenderer();
        RenderOffscreenFrames(device, renderer, world, width, height, 4);
        device.WaitForIdle(); // Final test teardown only.
        if (readScene() != originalScene) throw std::runtime_error("PBRTest scene file was modified");
        std::cout << "PBRTEST_PT_PASS original scene unchanged\n";
    }

    void RunOfflineProgressiveSmoke(HEngine::RenderDevice& device)
    {
        constexpr uint32_t width = 256, height = 256;
        auto cornell = BuildImportedCornellWorld();
        HEngine::SceneRenderer renderer(HEngine::SceneRendererDesc{
            .Device = device.GetDevice(),
            .FramesInFlight = device.GetCapabilities().FramesInFlight,
            .ShaderCacheDirectory = HEngine::ConfigManager::GetEngineGeneratedContentFullPath("Shaders/CACHES/DXIL"),
            .AssetRootDirectory = HEngine::ConfigManager::GetInstance().GetRootFolder(),
            .EngineAssetRootDirectory = HEngine::ConfigManager::GetInstance().GetEngineContentFolder(),
            .Backend = device.GetBackend(), .Capabilities = device.GetCapabilities(), .bEnableGpuTiming = true});
        renderer.ConfigureOffscreenOutput(width, height, "Progressive PT acceptance");
        renderer.ActivateOfflineViewportRenderer("he.viewport.path-tracing");
        auto& session = *renderer.GetOfflineViewportRendererSession();
        auto settings = session.GetOfflineSettings();
        settings.bInteractive = false;
        settings.bDenoise = true;
        settings.FixedWidth = width;
        settings.FixedHeight = height;
        settings.MaxBounces = 8;
        settings.DenoiseIntervalMilliseconds = 100;
        const auto directory = std::filesystem::path(HE_DEFAULT_PROJECT_DIR).parent_path() / "out/agent/captures/progressive-pt";
        std::filesystem::create_directories(directory);
        // No TLAS is a normal outcome for an empty/fully excluded editor world.
        // Exercise initial activation, loss of geometry, and recovery on the GPU.
        auto excludedWorld = cornell.World;
        for (auto& object : excludedWorld.Objects)
            for (auto& section : object.Sections)
                section.MaterialInstance.bCustomSurface = true;
        ++excludedWorld.FrameNumber;
        ++excludedWorld.SceneGeneration;
        RenderOffscreenFrames(device, renderer, excludedWorld, width, height, 4);
        auto unavailable = session.GetOfflineProgress();
        if (unavailable.State != HEngine::OfflineViewportSessionState::Failed ||
            unavailable.ExcludedInstances == 0 || unavailable.AccumulatedSamples != 0 ||
            unavailable.InFlight != 0 || unavailable.Error.find("custom material shader") == std::string::npos)
            throw std::runtime_error("Fully excluded PT world did not report a safe failure");
        bool exportRejected = false;
        try { (void)session.RequestExport(HEngine::OfflineViewportAov::Beauty, directory / "invalid.pfm", HEngine::OfflineViewportOutputFormat::Pfm); }
        catch (const std::runtime_error&) { exportRejected = true; }
        if (!exportRejected) throw std::runtime_error("Unavailable PT world allowed invalid export");
        std::cout << "PROGRESSIVE_PT_EXCLUDED_WORLD_PASS\n";
        std::ofstream timings(directory / "frames.csv");
        timings << "stage,frame,cpuWallMs,minSpp,avgSpp,maxSpp,submittedMinSpp,inFlight,generation,denoisedSpp,gpuSample,gpuSpanMs,traceMs,denoiseMs\n";
        uint32_t frameIndex = 0;
        for (uint32_t target : {1u, 4u, 16u, 64u, 256u})
        {
            settings.TargetSamples = target;
            session.ApplyOfflineSettings(settings);
            const auto started = std::chrono::steady_clock::now();
            while (true)
            {
                const auto frameStarted = std::chrono::steady_clock::now();
                RenderOffscreenFrames(device, renderer, cornell.World, width, height, 1);
                const auto progress = session.GetOfflineProgress();
                const double wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStarted).count();
                timings << target << ',' << frameIndex++ << ',' << wallMs << ',' << progress.AccumulatedSamples << ','
                    << progress.AverageSamples << ',' << progress.MaximumSamples << ',' << progress.SubmittedSamples << ','
                    << progress.InFlight << ',' << progress.Generation << ',' << progress.DenoisedSamples << ','
                    << renderer.GetStats().GpuRenderGraphSampleCount << ',' << renderer.GetStats().GpuFrameSpanMilliseconds << ','
                    << renderer.GetStats().GpuGiTraceMilliseconds << ',' << progress.DenoiseMilliseconds << '\n';
                if (frameIndex == 2)
                {
                    std::ofstream passes(directory / "passes.txt");
                    for (const auto& name : renderer.GetStats().CompiledPasses)
                    {
                        passes << name << '\n';
                        if (name == "SceneLightClustering" || name == "DirectionalShadow" || name == "DeferredLightingHDR" || name == "SSRHalfTrace")
                            throw std::runtime_error("Offline composition retained an unrelated raster lighting pass");
                    }
                }
                if (progress.InFlight > 1 || progress.AccumulatedSamples > target)
                    throw std::runtime_error("Progressive PT exceeded flight/sample bounds");
                if (progress.AccumulatedSamples == target && progress.DenoisedSamples == target) break;
                if (std::chrono::steady_clock::now() - started > std::chrono::seconds(60))
                    throw std::runtime_error("Progressive PT or OIDN failed to converge: " + progress.DenoiserStatus);
            }
            for (auto aov : {HEngine::OfflineViewportAov::Beauty, HEngine::OfflineViewportAov::DenoisedBeauty})
            {
                const auto path = directory / (std::to_string(target) + (aov == HEngine::OfflineViewportAov::Beauty ? "-raw.pfm" : "-denoised.pfm"));
                const auto result = ExportOfflineReferencePfm(device, renderer, session, cornell.World, width, height, path, aov);
                if (result.Status != HEngine::TextureReadbackStatus::Complete) throw std::runtime_error(result.Error);
            }
            const auto countsPath = directory / (std::to_string(target) + "-counts.raw");
            const auto countsResult = ExportOfflineReferencePfm(device, renderer, session, cornell.World, width, height,
                countsPath, HEngine::OfflineViewportAov::SampleCount, HEngine::OfflineViewportOutputFormat::Raw);
            if (countsResult.Status != HEngine::TextureReadbackStatus::Complete || std::filesystem::file_size(countsPath) != width * height * sizeof(uint32_t))
                throw std::runtime_error("GPU sample-count export is incomplete");
            std::vector<uint32_t> counts(width * height);
            std::ifstream countStream(countsPath, std::ios::binary);
            countStream.read(reinterpret_cast<char*>(counts.data()), counts.size() * sizeof(uint32_t));
            if (!countStream || !std::ranges::all_of(counts, [target](uint32_t count) { return count == target; }))
                throw std::runtime_error("Actual GPU pixel counts disagree with completed spp");
            std::cout << "PROGRESSIVE_PT_STAGE_PASS: spp=" << target << " frames=" << frameIndex << '\n';
        }
        auto emptyWorld = cornell.World;
        emptyWorld.Objects.clear();
        emptyWorld.SceneGeneration += 2;
        emptyWorld.FrameNumber += 2;
        const auto validGeneration = session.GetOfflineProgress().Generation;
        RenderOffscreenFrames(device, renderer, emptyWorld, width, height, 4);
        unavailable = session.GetOfflineProgress();
        if (unavailable.State != HEngine::OfflineViewportSessionState::Failed ||
            unavailable.Generation <= validGeneration || unavailable.AccumulatedSamples != 0)
            throw std::runtime_error("Empty PT world retained a previous valid generation");
        const auto recoveryStarted = std::chrono::steady_clock::now();
        do
        {
            RenderOffscreenFrames(device, renderer, cornell.World, width, height, 1);
            if (std::chrono::steady_clock::now() - recoveryStarted > std::chrono::seconds(10))
                throw std::runtime_error("PT did not recover after supported geometry returned");
        } while (session.GetOfflineProgress().AccumulatedSamples == 0);
        if (!session.GetOfflineProgress().Error.empty())
            throw std::runtime_error("PT retained an obsolete unsupported-scene error");
        std::cout << "PROGRESSIVE_PT_EMPTY_WORLD_RECOVERY_PASS\n";
        session.Pause();
        const auto paused = session.GetOfflineProgress();
        settings.PreviewAov = HEngine::OfflineViewportAov::Albedo;
        session.ApplyOfflineSettings(settings);
        RenderOffscreenFrames(device, renderer, cornell.World, width, height, 3);
        if (session.GetOfflineProgress().Generation != paused.Generation)
            throw std::runtime_error("Display-only change reset raw accumulation");

        // Synthetic camera snapshots exercise the actual GPU chain. This is
        // camera-snapshot-to-completion latency, not OS input or Present latency.
        settings.bInteractive = true;
        settings.TargetSamples = 4;
        settings.PreviewAov = HEngine::OfflineViewportAov::Beauty;
        session.ApplyOfflineSettings(settings);
        session.Resume();
        std::ofstream motion(directory / "motion.csv");
        motion << "pose,firstCompletedPreviewMs,generation,width,height,minSpp,inFlight,staleResults\n";
        for (uint32_t pose = 0; pose < 24; ++pose)
        {
            auto& camera = cornell.World.Camera;
            camera.PreviousViewProjection = camera.ViewProjection;
            camera.PreviousNonJitteredViewProjection = camera.NonJitteredViewProjection;
            camera.Position.x = 0.278f + 0.08f * std::sin(float(pose + 1) * 0.4f);
            const glm::vec3 direction(std::sin(float(pose + 1) * 0.12f) * 0.15f, 0.0f, 1.0f);
            camera.View = glm::lookAt(camera.Position, camera.Position + direction, glm::vec3(0, 1, 0));
            camera.ViewProjection = camera.Projection * camera.View;
            camera.NonJitteredViewProjection = camera.ViewProjection;
            const auto previousGeneration = session.GetOfflineProgress().Generation;
            const auto obsoleteExport = pose == 0
                ? session.RequestExport(HEngine::OfflineViewportAov::Beauty, directory / "obsolete-view.pfm", HEngine::OfflineViewportOutputFormat::Pfm)
                : HEngine::TextureReadbackHandle{};
            const auto inputTime = std::chrono::steady_clock::now();
            while (true)
            {
                RenderOffscreenFrames(device, renderer, cornell.World, width, height, 1);
                const auto progress = session.GetOfflineProgress();
                const double latency = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - inputTime).count();
                if (progress.InFlight > 1) throw std::runtime_error("Navigation exceeded the in-flight bound");
                if (progress.Generation > previousGeneration && progress.AccumulatedSamples > 0)
                {
                    motion << pose << ',' << latency << ',' << progress.Generation << ',' << progress.Width << ',' << progress.Height
                        << ',' << progress.AccumulatedSamples << ',' << progress.InFlight << ',' << progress.StaleResults << '\n';
                    break;
                }
                if (latency > 200.0) throw std::runtime_error("Current-camera completed preview exceeded 200 ms design target: " +
                    progress.Phase + ", generation=" + std::to_string(progress.Generation) +
                    ", minSpp=" + std::to_string(progress.AccumulatedSamples));
            }
            if (obsoleteExport)
            {
                // CancelPending releases an unsubmitted readback slot, so an
                // invalid handle is also the documented cancellation outcome.
                if (session.IsExportValid(obsoleteExport))
                {
                    if (session.GetExportResult(obsoleteExport).Status != HEngine::TextureReadbackStatus::Failed)
                        throw std::runtime_error("An old-generation export silently captured the new camera");
                    session.ReleaseExport(obsoleteExport);
                }
            }
        }
        const auto settleStarted = std::chrono::steady_clock::now();
        while (true)
        {
            RenderOffscreenFrames(device, renderer, cornell.World, width, height, 1);
            const auto progress = session.GetOfflineProgress();
            if (progress.Width == width && progress.Height == height && progress.DenoisedSamples == 4) break;
            if (std::chrono::steady_clock::now() - settleStarted > std::chrono::seconds(5))
                throw std::runtime_error("Stationary camera failed to restore target resolution and denoise");
        }
        // Constant-environment reflection regression. A flat plane covers the
        // complete camera footprint, so background pixels cannot hide a black
        // metallic BSDF. Compare direct NEE with combined NEE/BSDF sampling.
        auto furnace = BuildHardwareRtSmokeWorld();
        furnace.SceneGeneration = 0x4655524e414345ull;
        furnace.Objects.resize(1);
        furnace.Camera.Position = {0.0f, 2.0f, 0.0f};
        furnace.Camera.View = glm::lookAt(furnace.Camera.Position, glm::vec3(0), glm::vec3(0, 0, -1));
        furnace.Camera.ViewProjection = furnace.Camera.Projection * furnace.Camera.View;
        furnace.Camera.NonJitteredViewProjection = furnace.Camera.PreviousViewProjection =
            furnace.Camera.PreviousNonJitteredViewProjection = furnace.Camera.ViewProjection;
        auto& metal = furnace.Objects[0].Sections[0].MaterialInstance;
        metal.BaseColor = {0.75f, 0.5f, 0.25f, 1.0f};
        metal.Metallic = 1.0f;
        settings.bInteractive = false;
        settings.bDenoise = false;
        settings.bUseSceneEnvironment = false;
        settings.EnvironmentRadiance = glm::vec3(1.0f);
        settings.FixedWidth = settings.FixedHeight = 64;
        settings.TargetSamples = 256;
        settings.MaxBounces = 1;
        std::array<glm::dvec3, 3> means{};
        std::ofstream bsdfReport(directory / "bsdf.csv");
        bsdfReport << "case,roughness,directOnly,meanR,meanG,meanB\n";
        for (uint32_t variant = 0; variant < 3; ++variant)
        {
            metal.Roughness = variant == 0 ? 0.1f : 0.8f;
            ++metal.Revision;
            settings.bDirectOnly = variant == 2;
            session.ApplyOfflineSettings(settings);
            session.Restart();
            const auto started = std::chrono::steady_clock::now();
            do
            {
                RenderOffscreenFrames(device, renderer, furnace, width, height, 1);
                if (std::chrono::steady_clock::now() - started > std::chrono::seconds(10))
                    throw std::runtime_error("BSDF regression failed to finish its raw samples");
            } while (session.GetOfflineProgress().AccumulatedSamples < 256);
            const auto path = directory / ("metal-" + std::to_string(variant) + ".pfm");
            if (ExportOfflineReferencePfm(device, renderer, session, furnace, width, height, path).Status != HEngine::TextureReadbackStatus::Complete)
                throw std::runtime_error("BSDF reference export failed");
            const auto image = ReadLinearPfm(path);
            for (size_t pixel = 0; pixel < image.Pixels.size() / 3; ++pixel)
                for (size_t channel = 0; channel < 3; ++channel)
                {
                    const float value = image.Pixels[pixel * 3 + channel];
                    if (!std::isfinite(value) || value < 0) throw std::runtime_error("BSDF produced invalid radiance");
                    means[variant][channel] += value / double(image.Width * image.Height);
                }
            bsdfReport << variant << ',' << metal.Roughness << ',' << settings.bDirectOnly << ','
                << means[variant].x << ',' << means[variant].y << ',' << means[variant].z << '\n';
        }
        for (uint32_t channel = 0; channel < 3; ++channel)
        {
            if (std::abs(means[0][channel] - double(metal.BaseColor[channel])) > 0.04)
                throw std::runtime_error("Smooth metal reflection lost its Fresnel/base-color energy");
            if (std::abs(means[1][channel] - means[2][channel]) > 0.025)
                throw std::runtime_error("Environment NEE and MIS disagree on rough-metal direct illumination");
        }
        session.Cancel();
        renderer.DeactivateOfflineViewportRenderer();
        // Exercise mode retirement with a CPU filter and an export still owned
        // by the departing session. No device idle between mode transitions.
        for (uint32_t cycle = 0; cycle < 3; ++cycle)
        {
            renderer.ActivateOfflineViewportRenderer("he.viewport.path-tracing");
            auto& departing = *renderer.GetOfflineViewportRendererSession();
            auto lifecycleSettings = departing.GetOfflineSettings();
            lifecycleSettings.bInteractive = false;
            lifecycleSettings.FixedWidth = lifecycleSettings.FixedHeight = 64;
            lifecycleSettings.TargetSamples = 256;
            lifecycleSettings.bDenoise = true;
            lifecycleSettings.DenoiseIntervalMilliseconds = 100;
            departing.ApplyOfflineSettings(lifecycleSettings);
            const auto lifecycleStart = std::chrono::steady_clock::now();
            while (departing.GetOfflineProgress().DenoiserStatus != "OIDN CPU working")
            {
                RenderOffscreenFrames(device, renderer, cornell.World, width, height, 1);
                if (std::chrono::steady_clock::now() - lifecycleStart > std::chrono::seconds(5))
                    throw std::runtime_error("Lifecycle test never observed a running CPU denoiser");
            }
            const auto exportPath = directory / ("retire-" + std::to_string(cycle) + ".pfm");
            (void)departing.RequestExport(HEngine::OfflineViewportAov::Beauty, exportPath, HEngine::OfflineViewportOutputFormat::Pfm);
            RenderOffscreenFrames(device, renderer, cornell.World, width, height, 1);
            if (cycle == 2)
            {
                HEngine::OfflineViewportRendererProviderRegistry::Get().Unregister("he.viewport.path-tracing");
                RenderOffscreenFrames(device, renderer, cornell.World, width, height, 1);
                if (renderer.GetOfflineViewportRendererSession())
                    throw std::runtime_error("Provider revocation left an active renderer session");
            }
            else renderer.DeactivateOfflineViewportRenderer();
            const auto hasLeases = [cycle]()
            {
                try { return HEngine::OfflineViewportRendererProviderRegistry::Get().GetActiveSessionCount("he.viewport.path-tracing") != 0; }
                catch (const std::out_of_range&)
                {
                    if (cycle != 2) throw;
                    // Draining entries are erased when their final lease dies.
                    return false;
                }
            };
            while (hasLeases())
            {
                RenderOffscreenFrames(device, renderer, cornell.World, width, height, 1);
                if (std::chrono::steady_clock::now() - lifecycleStart > std::chrono::seconds(5))
                    throw std::runtime_error("Retired session failed to drain GPU/export/denoise ownership");
            }
            if (!std::filesystem::is_regular_file(exportPath) || std::filesystem::file_size(exportPath) == 0)
                throw std::runtime_error("A submitted export was lost during session retirement");
        }
        device.WaitForIdle(); // Final test shutdown only.
        std::cout << "PROGRESSIVE_PT_SMOKE_PASS: raw/OIDN stages, bounded progress, display reset isolation\n";
    }

    void RunCornellAaModeSwitchSmoke(
        HEngine::RenderDevice& renderDevice,
        const HEngine::GlobalIlluminationSettings& giSettings)
    {
        constexpr uint32_t Width = 256;
        constexpr uint32_t Height = 256;
        ImportedCornellWorld importedCornell = BuildImportedCornellWorld();
        const std::filesystem::path dxilDirectory =
            HEngine::ConfigManager::GetEngineGeneratedContentFullPath(
                "Shaders/CACHES/DXIL");
        HEngine::SceneRenderer renderer(HEngine::SceneRendererDesc{
            .Device = renderDevice.GetDevice(),
            .FramesInFlight = renderDevice.GetCapabilities().FramesInFlight,
            .ShaderCacheDirectory = dxilDirectory,
            .AssetRootDirectory =
                HEngine::ConfigManager::GetInstance().GetRootFolder(),
            .EngineAssetRootDirectory = HEngine::ConfigManager::GetInstance()
                .GetEngineContentFolder(),
            .Backend = renderDevice.GetBackend(),
            .Capabilities = renderDevice.GetCapabilities(),
            .GlobalIllumination = giSettings,
        });
        renderer.ConfigureOffscreenOutput(Width, Height, "Cornell AA Switch");
        constexpr std::array<CornellAaCaptureCase, 7> Sequence = {{
            { "switch-none-a", HEngine::AntiAliasingMode::None, 4u, 1u },
            { "switch-msaa-a", HEngine::AntiAliasingMode::Msaa, 4u, 1u },
            { "switch-none-b", HEngine::AntiAliasingMode::None, 4u, 1u },
            { "switch-msaa-b", HEngine::AntiAliasingMode::Msaa, 4u, 1u },
            { "switch-taa", HEngine::AntiAliasingMode::Taa, 4u, 1u },
            { "switch-fxaa", HEngine::AntiAliasingMode::Fxaa, 4u, 1u },
            { "switch-smaa", HEngine::AntiAliasingMode::Smaa, 4u, 1u },
        }};
        for (const CornellAaCaptureCase& captureCase : Sequence)
        {
            renderDevice.WaitForIdle();
            HEngine::AntiAliasingSettings settings;
            settings.Mode = captureCase.Mode;
            settings.MsaaSamples = captureCase.MsaaSamples;
            renderer.SetAntiAliasingSettings(settings);
            RenderOffscreenFrames(
                renderDevice,
                renderer,
                importedCornell.World,
                Width,
                Height,
                2);
            renderDevice.WaitForIdle();
            ValidateCornellAaPassContract(
                captureCase,
                renderer.GetAntiAliasingSelection(),
                renderer.GetStats());
        }
        std::cout
            << "CORNELL_AA_MODE_SWITCH_PASS: sequence="
            << "None,MSAA4x,None,MSAA4x,TAA,FXAA,SMAA"
            << " resolution=256x256 framesPerMode=2\n";
    }

    void RunCornellOfflineAaBypassHashSmoke(
        HEngine::RenderDevice& renderDevice,
        const std::filesystem::path& outputDirectory)
    {
        constexpr uint32_t Width = 512;
        constexpr uint32_t Height = 512;
        constexpr uint32_t Samples = 256;
        if (!HEngine::OfflineViewportRendererProviderRegistry::Get().Contains(
                "he.viewport.path-tracing"))
        {
            throw std::runtime_error(
                "Cornell AA smoke requires the Offline Reference provider");
        }

        ImportedCornellWorld importedCornell = BuildImportedCornellWorld();
        const std::filesystem::path dxilDirectory =
            HEngine::ConfigManager::GetEngineGeneratedContentFullPath(
                "Shaders/CACHES/DXIL");
        auto reference = std::make_unique<HEngine::SceneRenderer>(
            HEngine::SceneRendererDesc{
                .Device = renderDevice.GetDevice(),
                .FramesInFlight = renderDevice.GetCapabilities().FramesInFlight,
                .ShaderCacheDirectory = dxilDirectory,
                .AssetRootDirectory =
                    HEngine::ConfigManager::GetInstance().GetRootFolder(),
                .EngineAssetRootDirectory = HEngine::ConfigManager::GetInstance()
                    .GetEngineContentFolder(),
                .Backend = renderDevice.GetBackend(),
                .Capabilities = renderDevice.GetCapabilities(),
            });
        reference->ConfigureOffscreenOutput(
            Width,
            Height,
            "Cornell AA Reference Hash");
        reference->ActivateOfflineViewportRenderer("he.viewport.path-tracing");
        HEngine::IOfflineViewportRendererSession* session =
            reference->GetOfflineViewportRendererSession();
        if (!session)
            throw std::runtime_error("Cornell AA reference session is missing");

        HEngine::OfflineViewportRendererSettings settings;
        settings.TargetSamples = Samples;
        settings.SamplesPerFrame = 1;
        settings.MaxBounces = 8;
        settings.RenderScale = 1.0f;
        settings.FixedWidth = Width;
        settings.FixedHeight = Height;
        settings.SeedMode = HEngine::OfflineViewportSeedMode::Fixed;
        settings.Seed = 0x48454e47u;
        settings.bDirectOnly = false;
        settings.bAutoReset = true;
        settings.PreviewAov = HEngine::OfflineViewportAov::Beauty;
        session->ApplyOfflineSettings(settings);
        RenderOffscreenFrames(
            renderDevice,
            *reference,
            importedCornell.World,
            Width,
            Height,
            Samples);
        renderDevice.WaitForIdle();
        if (session->GetOfflineProgress().AccumulatedSamples != Samples)
        {
            throw std::runtime_error(
                "Cornell AA control Reference did not reach 256 samples");
        }

        const std::filesystem::path controlPath = outputDirectory /
            "cornell-aa-reference-none-256spp-linear.pfm";
        const HEngine::TextureReadbackResult controlResult =
            ExportOfflineReferencePfm(
                renderDevice,
                *reference,
                *session,
                importedCornell.World,
                Width,
                Height,
                controlPath);
        if (controlResult.Status != HEngine::TextureReadbackStatus::Complete ||
            !std::filesystem::is_regular_file(controlPath))
        {
            throw std::runtime_error(
                "Cornell AA control Reference export failed: " +
                controlResult.Error);
        }
        (void)ValidatePfmQuadrantCoverage(
            controlPath,
            "Cornell AA control Reference",
            true);

        HEngine::AntiAliasingSettings requestedTaa;
        requestedTaa.Mode = HEngine::AntiAliasingMode::Taa;
        reference->SetAntiAliasingSettings(requestedTaa);
        const HEngine::AntiAliasingSelection bypassSelection =
            reference->GetAntiAliasingSelection();
        if (bypassSelection.Requested.Mode != HEngine::AntiAliasingMode::Taa ||
            bypassSelection.Effective.Mode != HEngine::AntiAliasingMode::None ||
            !bypassSelection.bBypassed || bypassSelection.bFallback ||
            bypassSelection.Effective.CaptureSupersampleScale != 1u)
        {
            throw std::runtime_error(
                "Offline Reference did not bypass requested realtime TAA");
        }

        // Export the exact same native Beauty after changing only the realtime
        // AA request. Re-running the path integral would conflate the bypass
        // contract with harmless GPU floating-point scheduling differences.
        const std::filesystem::path bypassedPath = outputDirectory /
            "cornell-aa-reference-requested-taa-bypassed-256spp-linear.pfm";
        const HEngine::TextureReadbackResult bypassedResult =
            ExportOfflineReferencePfm(
                renderDevice,
                *reference,
                *session,
                importedCornell.World,
                Width,
                Height,
                bypassedPath);
        if (bypassedResult.Status != HEngine::TextureReadbackStatus::Complete ||
            !std::filesystem::is_regular_file(bypassedPath))
        {
            throw std::runtime_error(
                "Cornell AA bypassed Reference export failed: " +
                bypassedResult.Error);
        }
        (void)ValidatePfmQuadrantCoverage(
            bypassedPath,
            "Cornell AA bypassed Reference",
            true);

        const std::string controlHash = ComputeFileSha256(controlPath);
        const std::string bypassedHash = ComputeFileSha256(bypassedPath);
        if (controlHash != bypassedHash)
        {
            throw std::runtime_error(
                "Fixed-seed Offline Reference hash changed under requested TAA: " +
                controlHash + " != " + bypassedHash);
        }
        const std::span<const std::string> passes(
            reference->GetStats().CompiledPasses);
        if (ContainsPass(passes, "SceneTAA") ||
            ContainsPass(passes, "SceneFXAA") ||
            ContainsPassPrefix(passes, "SceneSMAA") ||
            ContainsPass(passes, "SceneDepthPrepassMSAA") ||
            reference->GetStats().RasterSampleCount != 1u)
        {
            throw std::runtime_error(
                "Offline Reference graph retained a realtime AA pass");
        }
        std::cout
            << "CORNELL_REFERENCE_AA_BYPASS_PASS: requested=TAA effective=None "
            << "samples=256 seed=0x48454e47 sha256=" << controlHash
            << " control=" << controlPath.generic_string()
            << " bypassed=" << bypassedPath.generic_string() << '\n';
        reference->DeactivateOfflineViewportRenderer();
        reference.reset();
    }

    void RunCornellAntiAliasingSmoke(HEngine::RenderDevice& renderDevice)
    {
        constexpr uint32_t Width = 512;
        constexpr uint32_t Height = 512;
        constexpr uint32_t FrameCount = 120;
        constexpr uint32_t SupersampleScale = 4;
        const std::filesystem::path outputDirectory =
            std::filesystem::path(HE_DEFAULT_PROJECT_DIR).parent_path() /
            "out/agent/captures/cornell-aa";
        std::filesystem::create_directories(outputDirectory);

        const HEngine::GlobalIlluminationSettings giSettings{
            .Technique =
                HEngine::GlobalIlluminationTechniqueKind::HardwareRayTracing,
            .Denoiser = HEngine::GlobalIlluminationDenoiserKind::Svgf,
            .RenderScale = 1.0f,
            .SamplesPerPixel = 1,
            .MaxBounces = 4,
            .AtrousIterations = 4,
            .bBilateralUpsample = true,
        };
        constexpr std::array<CornellAaCaptureCase, 5> Cases = {{
            { "none", HEngine::AntiAliasingMode::None, 4u, 1u },
            { "fxaa", HEngine::AntiAliasingMode::Fxaa, 4u, 1u },
            { "smaa", HEngine::AntiAliasingMode::Smaa, 4u, 1u },
            { "taa", HEngine::AntiAliasingMode::Taa, 4u, 1u },
            { "msaa4x", HEngine::AntiAliasingMode::Msaa, 4u, 1u },
        }};
        std::array<CornellAaCaptureResult, Cases.size()> captureResults;
        for (size_t caseIndex = 0; caseIndex < Cases.size(); ++caseIndex)
        {
            const CornellAaCaptureCase& captureCase = Cases[caseIndex];
            captureResults[caseIndex] = CaptureCornellAaCase(
                renderDevice,
                giSettings,
                captureCase,
                Width,
                Height,
                FrameCount,
                outputDirectory,
                "cornell-aa-" + std::string(captureCase.Id) + "-120f");
        }

        HEngine::GlobalIlluminationSettings taaHistorySettings = giSettings;
        taaHistorySettings.DebugView =
            HEngine::GlobalIlluminationDebugView::HistoryLength;
        const CornellAaCaptureResult taaHistoryCapture = CaptureCornellAaCase(
            renderDevice,
            taaHistorySettings,
            CornellAaCaptureCase{
                "taa-history",
                HEngine::AntiAliasingMode::Taa,
                4u,
                1u,
            },
            Width,
            Height,
            FrameCount,
            outputDirectory,
            "cornell-aa-taa-history-120f");
        const CornellAaCaptureResult taaCurrentCapture = CaptureCornellAaCase(
            renderDevice,
            giSettings,
            CornellAaCaptureCase{
                "taa-current",
                HEngine::AntiAliasingMode::Taa,
                4u,
                1u,
                0.0f,
                0.0f,
            },
            Width,
            Height,
            FrameCount,
            outputDirectory,
            "cornell-aa-taa-current-120f");

        const CornellAaCaptureCase supersampleCase{
            "ssaa4x-source",
            HEngine::AntiAliasingMode::None,
            4u,
            SupersampleScale,
        };
        const uint32_t captureSupersampleScale =
            supersampleCase.CaptureSupersampleScale;
        const CornellAaCaptureResult supersampleSource = CaptureCornellAaCase(
            renderDevice,
            giSettings,
            supersampleCase,
            Width * captureSupersampleScale,
            Height * captureSupersampleScale,
            FrameCount,
            outputDirectory,
            "cornell-aa-ssaa4x-source-2048-120f");
        const LinearRgbImage highResolution = ReadLinearPfm(
            supersampleSource.LinearPfm);
        if (highResolution.Width != Width * captureSupersampleScale ||
            highResolution.Height != Height * captureSupersampleScale)
        {
            throw std::runtime_error(
                "Cornell SSAA source capture has an unexpected resolution");
        }
        const LinearRgbImage groundTruth = DownsampleLinearLanczos2(
            highResolution,
            Width,
            Height);
        const std::filesystem::path groundTruthPfm = outputDirectory /
            "cornell-aa-ssaa4x-lanczos2-linear.pfm";
        const std::filesystem::path groundTruthPng = outputDirectory /
            "cornell-aa-ssaa4x-lanczos2-final.png";
        WriteLinearPfm(groundTruthPfm, groundTruth);
        WriteToneMappedPng(groundTruthPng, groundTruth);
        (void)ValidatePfmQuadrantCoverage(
            groundTruthPfm,
            "Cornell SSAA 4x Lanczos2 ground truth",
            true);
        if (!std::filesystem::is_regular_file(groundTruthPng) ||
            std::filesystem::file_size(groundTruthPng) < 1024u)
        {
            throw std::runtime_error(
                "Cornell SSAA 4x Lanczos2 PNG output is invalid");
        }
        std::cout
            << "CORNELL_AA_SSAA4X_PASS: source=2048x2048 output=512x512 "
            << "domain=linear-hdr filter=Lanczos2 pfm="
            << groundTruthPfm.generic_string()
            << " png=" << groundTruthPng.generic_string() << '\n';

        const std::filesystem::path maskDirectory =
            std::filesystem::path(HE_DEFAULT_PROJECT_DIR) /
            "SourceAssets/CornellBox/Generated";
        const std::array<std::string_view, 4> regionNames = {{
            "fully_occluded_floor_from_area_light",
            "stable_back_wall",
            "red_bleed_floor",
            "green_bleed_floor",
        }};
        const LinearRgbImage noneImage = ReadLinearPfm(
            captureResults[0].LinearPfm);
        const LinearRgbImage taaImage = ReadLinearPfm(
            captureResults[3].LinearPfm);
        const LinearRgbImage taaHistoryImage = ReadLinearPfm(
            taaHistoryCapture.LinearPfm);
        const LinearRgbImage taaCurrentImage = ReadLinearPfm(
            taaCurrentCapture.LinearPfm);
        std::array<float, regionNames.size()> taaRatios{};
        std::array<float, regionNames.size()> taaToNoneRatios{};
        for (size_t regionIndex = 0;
            regionIndex < regionNames.size();
            ++regionIndex)
        {
            const std::string_view regionName = regionNames[regionIndex];
            const BinaryImageMask mask = ReadBinaryPgmMask(
                maskDirectory /
                ("CornellBox.roi-" + std::string(regionName) + ".pgm"));
            const float referenceMean = MeanMaskedLuminance(groundTruth, mask);
            const float noneMean = MeanMaskedLuminance(noneImage, mask);
            const float taaMean = MeanMaskedLuminance(taaImage, mask);
            const float taaCurrentMean = MeanMaskedLuminance(
                taaCurrentImage,
                mask);
            if (!(referenceMean > 1e-8f) ||
                noneMean / referenceMean < 0.75f ||
                noneMean / referenceMean > 1.25f)
            {
                throw std::runtime_error(
                    "Cornell AA SSAA reference is inconsistent with the "
                    "non-jittered baseline in region " +
                    std::string(regionName));
            }
            taaRatios[regionIndex] = taaMean / referenceMean;
            const float taaCurrentRatio = taaCurrentMean / noneMean;
            if (taaCurrentRatio < 0.8f || taaCurrentRatio > 1.2f)
            {
                throw std::runtime_error(
                    "TAA jitter corrupted the upstream SVGF signal outside "
                    "the 20% Cornell tolerance in region " +
                    std::string(regionName) + ": ratio=" +
                    std::to_string(taaCurrentRatio));
            }
            const float taaHistoryRatio = taaMean / noneMean;
            taaToNoneRatios[regionIndex] = taaHistoryRatio;
            if (taaHistoryRatio < 0.8f || taaHistoryRatio > 1.2f)
            {
                throw std::runtime_error(
                    "TAA jitter transported SVGF energy outside the 20% "
                    "Cornell tolerance in region " +
                    std::string(regionName) + ": ratio=" +
                    std::to_string(taaHistoryRatio));
            }
            if (regionIndex == 0u &&
                MeanMaskedLuminance(taaHistoryImage, mask) < 64.0f)
            {
                throw std::runtime_error(
                    "TAA jitter prevented SVGF history convergence in the "
                    "fully occluded Cornell floor ROI");
            }
        }
        const BinaryImageMask edgeMask = ReadBinaryPgmMask(
            maskDirectory / "CornellBox.edge-mask.pgm");
        if (edgeMask.Width != taaImage.Width ||
            edgeMask.Height != taaImage.Height)
        {
            throw std::runtime_error(
                "Cornell AA edge mask dimensions differ from the captures");
        }
        float maximumNonEdgeLinearLuminanceDelta = 0.0f;
        for (size_t pixel = 0; pixel < edgeMask.Pixels.size(); ++pixel)
        {
            if (edgeMask.Pixels[pixel] != 0u)
                continue;
            const size_t base = pixel * 3u;
            const float taaLuminance =
                0.2126f * taaImage.Pixels[base + 0u] +
                0.7152f * taaImage.Pixels[base + 1u] +
                0.0722f * taaImage.Pixels[base + 2u];
            const float noneLuminance =
                0.2126f * noneImage.Pixels[base + 0u] +
                0.7152f * noneImage.Pixels[base + 1u] +
                0.0722f * noneImage.Pixels[base + 2u];
            maximumNonEdgeLinearLuminanceDelta = std::max(
                maximumNonEdgeLinearLuminanceDelta,
                taaLuminance - noneLuminance);
        }
        if (maximumNonEdgeLinearLuminanceDelta > 0.1f)
        {
            throw std::runtime_error(
                "TAA retained a non-edge SVGF firefly above the 0.1 "
                "linear-luminance delta limit");
        }
        std::cout
            << "CORNELL_TAA_SVGF_JITTER_PASS: ratioToNone "
            << "occluded=" << taaToNoneRatios[0]
            << " stable=" << taaToNoneRatios[1]
            << " red=" << taaToNoneRatios[2]
            << " green=" << taaToNoneRatios[3]
            << "; ratioToSSAA occluded=" << taaRatios[0]
            << " stable=" << taaRatios[1]
            << " red=" << taaRatios[2]
            << " green=" << taaRatios[3]
            << " occludedHistory>=64 maxNonEdgeLinearDelta="
            << maximumNonEdgeLinearLuminanceDelta << '\n';

        RunCornellAaModeSwitchSmoke(renderDevice, giSettings);
        RunCornellOfflineAaBypassHashSmoke(renderDevice, outputDirectory);
        std::cout
            << "CORNELL_AA_SMOKE_PASS: modes=None,FXAA,SMAA,TAA,MSAA4x "
            << "framesPerMode=120 resolution=512x512 ssaa=4x-linear-Lanczos2 "
            << "output=" << outputDirectory.generic_string() << '\n';
    }

    [[nodiscard]] float HalfToFloat(uint16_t half) noexcept
    {
        const uint32_t sign = static_cast<uint32_t>(half & 0x8000u) << 16u;
        uint32_t exponent = (half >> 10u) & 0x1fu;
        uint32_t mantissa = half & 0x03ffu;
        uint32_t bits = 0;
        if (exponent == 0u)
        {
            if (mantissa == 0u)
            {
                bits = sign;
            }
            else
            {
                exponent = 1u;
                while ((mantissa & 0x0400u) == 0u)
                {
                    mantissa <<= 1u;
                    --exponent;
                }
                mantissa &= 0x03ffu;
                bits = sign | ((exponent + 112u) << 23u) | (mantissa << 13u);
            }
        }
        else if (exponent == 31u)
        {
            bits = sign | 0x7f800000u | (mantissa << 13u);
        }
        else
        {
            bits = sign | ((exponent + 112u) << 23u) | (mantissa << 13u);
        }
        return std::bit_cast<float>(bits);
    }

    void RunRealtimeRtgiSmoke(
        HEngine::Window& window,
        HEngine::RenderDevice& renderDevice,
        HEngine::RayTracingScene& scene,
        HEngine::RenderWorld& world,
        bool bRunLegacyReferenceContracts = true)
    {
        constexpr uint32_t Width = 64;
        constexpr uint32_t Height = 64;
        const std::filesystem::path dxilDirectory =
            HEngine::ConfigManager::GetEngineGeneratedContentFullPath(
                "Shaders/CACHES/DXIL");
        auto primaryVisibility = std::make_unique<HEngine::SceneRenderer>(
            HEngine::SceneRendererDesc{
                .Device = renderDevice.GetDevice(),
                .FramesInFlight = renderDevice.GetCapabilities().FramesInFlight,
                .ShaderCacheDirectory = dxilDirectory,
                .AssetRootDirectory =
                    HEngine::ConfigManager::GetInstance().GetEngineContentFolder(),
                .EngineAssetRootDirectory =
                    HEngine::ConfigManager::GetInstance().GetEngineContentFolder(),
            });
        primaryVisibility->ConfigureOffscreenOutput(
            Width,
            Height,
            "Hardware RTGI Smoke Primary Visibility");
        {
            HEngine::FrameContext& frame = renderDevice.BeginFrame();
            try
            {
                HEngine::EffekseerRuntime::BeginHostFrame(frame.FrameIndex, frame.CompletedSubmissionInstance, frame.GraphicsCommandList);
                primaryVisibility->RecordFrame(
                    world,
                    HEngine::SceneRendererFrameDesc{
                        .CommandList = frame.GraphicsCommandList,
                        .Width = Width,
                        .Height = Height,
                        .FrameSlot = frame.FrameIndex,
                        .CompletedSubmission = frame.CompletedSubmissionInstance,
                        .OutputMode = HEngine::SceneRendererOutputMode::OffscreenOnly,
                    });
                renderDevice.SubmitAndPresent(frame, false);
                HEngine::EffekseerRuntime::NotifyHostFrameSubmitted(frame.FrameIndex, frame.SubmissionInstance);
                primaryVisibility->NotifyFrameSubmitted(
                    frame.FrameIndex,
                    frame.SubmissionInstance);
            }
            catch (...)
            {
                if (frame.bSubmissionIssued)
                {
                    HEngine::EffekseerRuntime::NotifyHostFrameSubmitted(frame.FrameIndex, frame.SubmissionInstance);
                    primaryVisibility->NotifyFrameSubmitted(
                        frame.FrameIndex,
                        frame.SubmissionInstance);
                }
                else
                {
                    renderDevice.AbortFrame(frame);
                    HEngine::EffekseerRuntime::AbortHostFrame(frame.FrameIndex);
                    primaryVisibility->AbortFrame(frame.FrameIndex);
                }
                throw;
            }
        }
        renderDevice.WaitForIdle();
        const HEngine::SceneRendererOffscreenOutput primaryOutput =
            primaryVisibility->GetOffscreenOutput();
        if (!primaryOutput || !primaryOutput.Depth)
            throw std::runtime_error("RTGI smoke primary visibility has no depth output");

        HEngine::SceneSurfaceDataPass surfacePass({
            .Device = renderDevice.GetDevice(),
            .Scene = &scene,
            .ShaderArtifactCacheRoot = dxilDirectory.parent_path(),
            .Backend = renderDevice.GetBackend(),
        });
        HEngine::SvgfDenoiser svgf({
            .Device = renderDevice.GetDevice(),
            .ShaderArtifactCacheRoot = dxilDirectory.parent_path(),
            .Backend = renderDevice.GetBackend(),
        });
        HEngine::RealtimeRayTracedGlobalIllumination rtgi({
            .Device = renderDevice.GetDevice(),
            .Scene = &scene,
            .Denoiser = &svgf,
            .ShaderArtifactCacheRoot = dxilDirectory.parent_path(),
            .Backend = renderDevice.GetBackend(),
        });
        HEngine::GlobalIlluminationSettings settings{
            .Technique = HEngine::GlobalIlluminationTechniqueKind::HardwareRayTracing,
            .Denoiser = HEngine::GlobalIlluminationDenoiserKind::Svgf,
            .RenderScale = 0.5f,
            .SamplesPerPixel = 1,
            .MaxBounces = 2,
            .AtrousIterations = 3,
            .bBilateralUpsample = true,
        };

        nvrhi::TextureDesc denoisedReadbackDesc;
        denoisedReadbackDesc.setWidth(Width)
            .setHeight(Height)
            .setFormat(nvrhi::Format::RGBA16_FLOAT);
        nvrhi::TextureDesc historyReadbackDesc;
        historyReadbackDesc.setWidth(Width / 2u)
            .setHeight(Height / 2u)
            .setFormat(nvrhi::Format::R32_FLOAT);
        nvrhi::TextureDesc confidenceReadbackDesc;
        confidenceReadbackDesc.setWidth(Width)
            .setHeight(Height)
            .setFormat(nvrhi::Format::R32_FLOAT);
        nvrhi::StagingTextureHandle denoisedStaging =
            renderDevice.GetDevice()->createStagingTexture(
                denoisedReadbackDesc,
                nvrhi::CpuAccessMode::Read);
        nvrhi::StagingTextureHandle historyStaging =
            renderDevice.GetDevice()->createStagingTexture(
                historyReadbackDesc,
                nvrhi::CpuAccessMode::Read);
        nvrhi::StagingTextureHandle confidenceStaging =
            renderDevice.GetDevice()->createStagingTexture(
                confidenceReadbackDesc,
                nvrhi::CpuAccessMode::Read);
        if (!denoisedStaging || !historyStaging || !confidenceStaging)
            throw std::runtime_error("RTGI smoke failed to create staging outputs");
        const std::filesystem::path rtgiPfmPath =
            std::filesystem::path(HE_DEFAULT_PROJECT_DIR).parent_path() /
            "out/agent/captures/hardware-rtgi-svgf-linear.pfm";
        HEngine::TextureReadbackQueue rtgiReadback(renderDevice.GetDevice());
        HEngine::TextureReadbackHandle rtgiCapture;

        HEngine::RenderGraphResourcePool resourcePool(
            renderDevice.GetDevice(),
            renderDevice.GetCapabilities().FramesInFlight);
        uint64_t completedSubmission = 0;
        for (uint32_t rtgiFrame = 0; rtgiFrame < 2; ++rtgiFrame)
        {
            scene.Prepare(
                world,
                HEngine::RayTracingSceneRevisions{
                    .Scene = 1,
                    .GeometryTopology = 1,
                    .Material = 1,
                    .Light = 1,
                },
                completedSubmission);
            surfacePass.PrepareFrame(world.Camera, Width, Height);
            const HEngine::GlobalIlluminationHistorySignature signature{
                .Width = Width,
                .Height = Height,
                .RenderScale = settings.RenderScale,
                .ProjectionFingerprint = 1,
                .JitterSequenceGeneration = 1,
                .SceneGeneration = 1,
                .GeometryTopologyGeneration = 1,
                .MaterialGeneration = 1,
                .LightGeneration = 1,
                .Technique = settings.Technique,
                .Denoiser = settings.Denoiser,
                .SamplesPerPixel = 1,
                .MaxBounces = settings.MaxBounces,
                .bHalfResolution = true,
            };
            rtgi.PrepareFrame(
                HEngine::RealtimeRtgiFrameParameters{
                    .Camera = world.Camera,
                    .HistorySignature = signature,
                    .Width = Width,
                    .Height = Height,
                    .FrameSequenceIndex = rtgiFrame,
                    .MaximumRayDistance = 100.0f,
                    .CompletedSubmission = completedSubmission,
                },
                settings);

            HEngine::RenderGraph graph;
            graph.Resize(Width, Height);
            const HEngine::RayTracingSceneGraphResources sceneResources =
                scene.AddBuildPasses(graph);
            const HEngine::TextureHandle depth = graph.ImportTexture(
                HEngine::RenderGraphTextureDesc{
                    .DebugName = "RTGI Smoke Imported Primary Depth",
                    .Width = Width,
                    .Height = Height,
                    .Format = nvrhi::Format::D32,
                },
                primaryOutput.Depth,
                nvrhi::ResourceStates::ShaderResource,
                nvrhi::ResourceStates::ShaderResource);
            const HEngine::SceneSurfaceData surface = surfacePass.AddPasses(
                graph,
                depth,
                sceneResources);
            const HEngine::GlobalIlluminationOutputs outputs = rtgi.AddPasses(
                graph,
                HEngine::GlobalIlluminationInputs{
                    .Surface = surface,
                    .SceneTlas = sceneResources.SceneTlas,
                    .MaterialBuffer = sceneResources.Materials,
                    .GeometryBuffer = sceneResources.Geometries,
                    .LightBuffer = sceneResources.EmissiveTriangles,
                    .RayTracingVertexBuffer = sceneResources.Vertices,
                    .RayTracingIndexBuffer = sceneResources.Indices,
                    .RayTracingInstanceBuffer = sceneResources.Instances,
                .AnalyticLightBuffer = sceneResources.AnalyticLights,
                    .CurrentInverseViewProjection = glm::inverse(
                        world.Camera.ViewProjection),
                    .PreviousViewProjection = world.Camera.PreviousViewProjection,
                    .Width = Width,
                    .Height = Height,
                },
                settings);
            graph.AddPass<RealtimeRtgiReadbackPassData>(
                "RealtimeRTGISmokeReadback",
                HEngine::RenderGraphQueue::Copy,
                [outputs](
                    HEngine::RenderGraphBuilder& builder,
                    RealtimeRtgiReadbackPassData& data)
                {
                    data.DenoisedDiffuse = builder.CopySource(outputs.CompositeRadiance);
                    data.HistoryLength = builder.CopySource(outputs.HistoryLength);
                    data.Confidence = builder.CopySource(outputs.Confidence);
                    builder.SetSideEffect();
                },
                [denoisedStaging,
                    historyStaging,
                    confidenceStaging,
                    rtgiFrame,
                    &rtgiReadback,
                    &rtgiCapture,
                    rtgiPfmPath](
                    HEngine::RenderGraphContext& context,
                    const RealtimeRtgiReadbackPassData& data)
                {
                    context.GetCommandList()->copyTexture(
                        denoisedStaging,
                        nvrhi::TextureSlice(),
                        context.GetTexture(
                            data.DenoisedDiffuse,
                            HEngine::RenderGraphAccess::Read),
                        nvrhi::TextureSlice());
                    context.GetCommandList()->copyTexture(
                        historyStaging,
                        nvrhi::TextureSlice(),
                        context.GetTexture(
                            data.HistoryLength,
                            HEngine::RenderGraphAccess::Read),
                        nvrhi::TextureSlice());
                    context.GetCommandList()->copyTexture(
                        confidenceStaging,
                        nvrhi::TextureSlice(),
                        context.GetTexture(
                            data.Confidence,
                            HEngine::RenderGraphAccess::Read),
                        nvrhi::TextureSlice());
                    if (rtgiFrame == 1u)
                    {
                        rtgiCapture = rtgiReadback.Record(
                            context.GetCommandList(),
                            context.GetTexture(
                                data.DenoisedDiffuse,
                                HEngine::RenderGraphAccess::Read),
                            HEngine::TextureReadbackDesc{
                                .OutputPath = rtgiPfmPath,
                                .OutputFormat =
                                    HEngine::TextureReadbackOutputFormat::Pfm,
                                .AlphaMode =
                                    HEngine::TextureReadbackAlphaMode::Discard,
                                .NonFinitePolicy =
                                    HEngine::TextureReadbackNonFinitePolicy::Reject,
                                .bReplaceExisting = true,
                            });
                    }
                });
            graph.CompileOrThrow();
            const std::vector<std::string> passNames = graph.GetCompiledPassNames();
            for (std::string_view required : {
                "SceneSurfaceData",
                "RTDirectAndIndirectTrace",
                "TemporalReprojection",
                "HistoryFix",
                "MomentsAndVariance",
                "ATrousFilter[0]",
                "ATrousFilter[1]",
                "ATrousFilter[2]",
                "BilateralUpsample",
                "RealtimeRTGISmokeReadback",
            })
            {
                if (std::ranges::find(passNames, required) == passNames.end())
                    throw std::runtime_error("RTGI smoke graph omitted a required pass");
            }

            HEngine::FrameContext& frame = renderDevice.BeginFrame();
            bool bPoolActive = false;
            try
            {
                resourcePool.Collect(frame.CompletedSubmissionInstance);
                resourcePool.BeginFrame(
                    frame.FrameIndex,
                    frame.CompletedSubmissionInstance);
                bPoolActive = true;
                graph.Execute(HEngine::RenderGraphExecutionParams{
                    .CommandList = frame.GraphicsCommandList,
                    .ResourcePool = &resourcePool,
                    .FrameSlot = frame.FrameIndex,
                });
                renderDevice.SubmitAndPresent(frame, false);
                resourcePool.EndFrame(frame.FrameIndex, frame.SubmissionInstance);
                bPoolActive = false;
                scene.NotifyFrameSubmitted(frame.SubmissionInstance);
                rtgi.NotifyFrameSubmitted(frame.SubmissionInstance);
                if (rtgiCapture && rtgiReadback.IsValid(rtgiCapture) &&
                    rtgiReadback.GetResult(rtgiCapture).Status ==
                        HEngine::TextureReadbackStatus::PendingSubmission)
                {
                    rtgiReadback.NotifySubmitted(
                        rtgiCapture,
                        frame.SubmissionInstance);
                }
                completedSubmission = frame.SubmissionInstance;
            }
            catch (...)
            {
                if (frame.bSubmissionIssued)
                {
                    if (bPoolActive)
                        resourcePool.EndFrame(frame.FrameIndex, frame.SubmissionInstance);
                    scene.NotifyFrameSubmitted(frame.SubmissionInstance);
                    rtgi.NotifyFrameSubmitted(frame.SubmissionInstance);
                    if (rtgiCapture && rtgiReadback.IsValid(rtgiCapture) &&
                        rtgiReadback.GetResult(rtgiCapture).Status ==
                            HEngine::TextureReadbackStatus::PendingSubmission)
                    {
                        rtgiReadback.NotifySubmitted(
                            rtgiCapture,
                            frame.SubmissionInstance);
                    }
                }
                else
                {
                    renderDevice.AbortFrame(frame);
                    if (bPoolActive)
                        resourcePool.AbortFrame(frame.FrameIndex);
                    rtgi.AbortFrame();
                    if (rtgiCapture && rtgiReadback.IsValid(rtgiCapture))
                        rtgiReadback.CancelPending(rtgiCapture);
                }
                throw;
            }
        }
        renderDevice.WaitForIdle();
        rtgiReadback.Drain(std::numeric_limits<uint64_t>::max());
        const HEngine::TextureReadbackResult rtgiCaptureResult =
            rtgiReadback.GetResult(rtgiCapture);
        if (rtgiCaptureResult.Status != HEngine::TextureReadbackStatus::Complete ||
            !std::filesystem::is_regular_file(rtgiPfmPath) ||
            std::filesystem::file_size(rtgiPfmPath) < 1024u)
        {
            throw std::runtime_error(
                "Real-time RTGI did not produce a valid linear HDR PFM: " +
                rtgiCaptureResult.Error);
        }

        float maximumLuminance = 0.0f;
        bool bFinite = true;
        size_t rowPitch = 0;
        const void* denoisedData = renderDevice.GetDevice()->mapStagingTexture(
            denoisedStaging,
            nvrhi::TextureSlice(),
            nvrhi::CpuAccessMode::Read,
            &rowPitch);
        if (!denoisedData)
            throw std::runtime_error("RTGI smoke failed to map denoised output");
        for (uint32_t y = 0; y < Height; ++y)
        {
            const auto* row = reinterpret_cast<const uint16_t*>(
                static_cast<const uint8_t*>(denoisedData) + y * rowPitch);
            for (uint32_t x = 0; x < Width; ++x)
            {
                const float red = HalfToFloat(row[x * 4u + 0u]);
                const float green = HalfToFloat(row[x * 4u + 1u]);
                const float blue = HalfToFloat(row[x * 4u + 2u]);
                bFinite = bFinite && std::isfinite(red) &&
                    std::isfinite(green) && std::isfinite(blue);
                maximumLuminance = std::max(
                    maximumLuminance,
                    0.2126f * red + 0.7152f * green + 0.0722f * blue);
            }
        }
        renderDevice.GetDevice()->unmapStagingTexture(denoisedStaging);

        float maximumHistoryLength = 0.0f;
        float maximumConfidence = 0.0f;
        const auto inspectScalarFloat = [&, device = renderDevice.GetDevice()](
            nvrhi::IStagingTexture* staging,
            uint32_t width,
            uint32_t height,
            float& maximum)
        {
            size_t scalarRowPitch = 0;
            const void* data = device->mapStagingTexture(
                staging,
                nvrhi::TextureSlice(),
                nvrhi::CpuAccessMode::Read,
                &scalarRowPitch);
            if (!data)
                throw std::runtime_error("RTGI smoke failed to map a scalar output");
            for (uint32_t y = 0; y < height; ++y)
            {
                const auto* row = reinterpret_cast<const float*>(
                    static_cast<const uint8_t*>(data) + y * scalarRowPitch);
                for (uint32_t x = 0; x < width; ++x)
                {
                    const float value = row[x];
                    bFinite = bFinite && std::isfinite(value);
                    maximum = std::max(maximum, value);
                }
            }
            device->unmapStagingTexture(staging);
        };
        inspectScalarFloat(
            historyStaging,
            Width / 2u,
            Height / 2u,
            maximumHistoryLength);
        inspectScalarFloat(
            confidenceStaging,
            Width,
            Height,
            maximumConfidence);

        const HEngine::RayTracingSceneStats& stats = scene.GetStats();
        std::cout
            << "REALTIME_RTGI_SMOKE_METRICS: finite=" << (bFinite ? 1 : 0)
            << " historyMax=" << maximumHistoryLength
            << " confidenceMax=" << maximumConfidence
            << " maxLuminance=" << maximumLuminance
            << " tlasBuilds=" << stats.TlasBuildCount
            << " tlasUpdates=" << stats.TlasUpdateCount << '\n';
        std::cout << "REALTIME_RTGI_LINEAR_CAPTURE: "
                  << rtgiPfmPath.generic_string() << '\n';
        rtgiReadback.Release(rtgiCapture);
        if (!bFinite || maximumLuminance <= 0.0f ||
            maximumHistoryLength < 2.0f || maximumConfidence <= 0.0f ||
            stats.TlasBuildCount != 1u || stats.TlasUpdateCount != 0u)
        {
            throw std::runtime_error(
                "RTGI/SVGF smoke did not produce stable finite history and radiance");
        }
        std::cout
            << "REALTIME_RTGI_SMOKE_PASS: 1spp scale=0.5 bounces=2 atrous=3 "
            << "historyMax=" << maximumHistoryLength
            << " confidenceMax=" << maximumConfidence
            << " maxLuminance=" << maximumLuminance
            << " staticTlasBuilds=" << stats.TlasBuildCount
            << " staticTlasUpdates=" << stats.TlasUpdateCount << '\n';

        auto integratedRenderer = std::make_unique<HEngine::SceneRenderer>(
            HEngine::SceneRendererDesc{
                .Device = renderDevice.GetDevice(),
                .FramesInFlight = renderDevice.GetCapabilities().FramesInFlight,
                .ShaderCacheDirectory = dxilDirectory,
                .AssetRootDirectory =
                    HEngine::ConfigManager::GetInstance().GetEngineContentFolder(),
                .EngineAssetRootDirectory =
                    HEngine::ConfigManager::GetInstance().GetEngineContentFolder(),
                .Backend = renderDevice.GetBackend(),
                .Path = HEngine::RenderingPath::Forward, // This fixture asserts the forward RTGI graph.
                .Capabilities = renderDevice.GetCapabilities(),
                .GlobalIllumination = settings,
                .bEnableGpuTiming = true,
            });
        constexpr uint32_t Perf1080Width = 1920;
        constexpr uint32_t Perf1080Height = 1080;
        integratedRenderer->ConfigureOffscreenOutput(
            Perf1080Width,
            Perf1080Height,
            "Integrated RTGI Smoke");
        if (!integratedRenderer->RequiresContinuousUpdate())
            throw std::runtime_error("Integrated RTGI did not request continuous updates");
        RenderOffscreenFrames(
            renderDevice,
            *integratedRenderer,
            world,
            Perf1080Width,
            Perf1080Height,
            120);
        renderDevice.WaitForIdle();
        integratedRenderer->CollectGpuCullingStats();
        integratedRenderer->CollectGpuTimingStats();
        const HEngine::SceneRendererStats& integratedStats =
            integratedRenderer->GetStats();
        const auto passIndex = [&integratedStats](std::string_view name)
        {
            const auto found = std::ranges::find(
                integratedStats.CompiledPasses,
                name);
            return found == integratedStats.CompiledPasses.end()
                ? std::numeric_limits<size_t>::max()
                : static_cast<size_t>(std::distance(
                    integratedStats.CompiledPasses.begin(),
                    found));
        };
        const size_t depthIndex = passIndex("SceneDepthPrepass");
        const size_t surfaceIndex = std::min(passIndex("SceneSurfaceData"), passIndex("RasterSurfaceGuides"));
        const size_t opaqueIndex = passIndex("OpaqueForwardHDR");
        const size_t traceIndex = passIndex("RTDirectAndIndirectTrace");
        const size_t temporalIndex = passIndex("TemporalReprojection");
        const size_t compositeIndex = passIndex("GIComposite");
        const size_t transparentIndex = passIndex("TransparentForwardHDR");
        const bool bHasDepthPyramid = std::ranges::any_of(
            integratedStats.CompiledPasses,
            [](const std::string& passName)
            {
                return passName.starts_with("DepthPyramid");
            });
        if (!(depthIndex < surfaceIndex && surfaceIndex < opaqueIndex &&
                opaqueIndex < traceIndex && traceIndex < temporalIndex &&
                temporalIndex < compositeIndex &&
                compositeIndex < transparentIndex) ||
            bHasDepthPyramid ||
            integratedStats.GiStats.Technique !=
                HEngine::GlobalIlluminationTechniqueKind::HardwareRayTracing ||
            integratedStats.RayTracingInstanceCount != 2u ||
            integratedStats.RayTracingTlasBuildCount != 1u ||
            integratedStats.RayTracingTlasUpdateCount != 0u ||
            !integratedStats.bGpuGiTimingValid ||
            integratedStats.GpuGiTraceMilliseconds <= 0.0 ||
            integratedStats.GpuSvgfMilliseconds <= 0.0)
        {
            std::cerr << "INTEGRATED_RTGI_DIAGNOSTIC instances=" << integratedStats.RayTracingInstanceCount
                << " tlasBuilds=" << integratedStats.RayTracingTlasBuildCount << " updates=" << integratedStats.RayTracingTlasUpdateCount
                << " timing=" << integratedStats.bGpuGiTimingValid << " traceMs=" << integratedStats.GpuGiTraceMilliseconds
                << " svgfMs=" << integratedStats.GpuSvgfMilliseconds << " technique=" << int(integratedStats.GiStats.Technique) << " passes=";
            for (const auto& name : integratedStats.CompiledPasses) std::cerr << name << ' ';
            std::cerr << '\n';
            throw std::runtime_error(
                "Integrated SceneRenderer RTGI graph/stats contract failed");
        }
        std::cout
            << "INTEGRATED_RTGI_SMOKE_PASS: resolution=1920x1080 frames=120 spp=1 bounces=2 scale=0.5 passes="
            << integratedStats.CompiledPasses.size()
            << " instances=" << integratedStats.RayTracingInstanceCount
            << " staticTlasBuilds="
            << integratedStats.RayTracingTlasBuildCount
            << " staticTlasUpdates="
            << integratedStats.RayTracingTlasUpdateCount
            << " rtLogicalPayloadBytes="
            << (integratedStats.RayTracingGeometryBytes +
                integratedStats.RayTracingSceneTableBytes)
            << " rtActiveBufferAllocationRequirementBytes="
            << integratedStats.RayTracingActiveBufferAllocationRequirementBytes
            << " rtActiveAsAllocationRequirementBytes="
            << integratedStats.RayTracingActiveAsAllocationRequirementBytes
            << " transientLogicalPeakBytes="
            << integratedStats.RenderGraphPeakFrameTransientBytes
            << " blasBuildEventMs="
            << integratedStats.GpuRayTracingBlasBuildEventMilliseconds
            << " tlasBuildEventMs="
            << integratedStats.GpuRayTracingTlasBuildEventMilliseconds
            << " tlasUpdateLastMs="
            << integratedStats.GpuRayTracingTlasUpdateMilliseconds
            << " traceLastMs=" << integratedStats.GpuGiTraceMilliseconds
            << " svgfLastMs=" << integratedStats.GpuSvgfMilliseconds
            << " compositeLastMs="
            << integratedStats.GpuGiCompositeMilliseconds
            << '\n';
        std::cout << "INTEGRATED_RTGI_GPU_TIMINGS_BEGIN\n"
                  << integratedRenderer->BuildGpuTimingDebugDump()
                  << "\nINTEGRATED_RTGI_GPU_TIMINGS_END\n";
        const HEngine::RenderWorld stableWorld = world;
        const uint32_t resetCountBeforeMotion =
            integratedRenderer->GetStats().GiStats.HistoryResetCount;
        world.Objects[0].PreviousWorldTransform = world.Objects[0].WorldTransform;
        world.Objects[0].WorldTransform = glm::translate(
            world.Objects[0].WorldTransform,
            glm::vec3(0.08f, 0.0f, 0.0f));
        RenderOffscreenFrames(
            renderDevice,
            *integratedRenderer,
            world,
            Perf1080Width,
            Perf1080Height,
            1);
        renderDevice.WaitForIdle();
        if (integratedRenderer->GetStats().GiStats.HistoryResetCount !=
            resetCountBeforeMotion)
        {
            throw std::runtime_error(
                "Ordinary object motion incorrectly reset RTGI history");
        }

        world.Camera.PreviousViewProjection = world.Camera.ViewProjection;
        world.Camera.Position.x += 0.05f;
        world.Camera.View = glm::lookAt(
            world.Camera.Position,
            glm::vec3(0.0f, 0.0f, 0.0f),
            glm::vec3(0.0f, 1.0f, 0.0f));
        world.Camera.ViewProjection = world.Camera.Projection * world.Camera.View;
        world.Camera.NonJitteredViewProjection = world.Camera.ViewProjection;
        world.Camera.PreviousNonJitteredViewProjection =
            world.Camera.PreviousViewProjection;
        RenderOffscreenFrames(
            renderDevice,
            *integratedRenderer,
            world,
            Perf1080Width,
            Perf1080Height,
            1);
        renderDevice.WaitForIdle();
        if (integratedRenderer->GetStats().GiStats.HistoryResetCount !=
            resetCountBeforeMotion)
        {
            throw std::runtime_error(
                "Normal camera motion incorrectly reset RTGI history");
        }

        HEngine::GlobalIlluminationSettings motionHistoryDebug = settings;
        motionHistoryDebug.DebugView =
            HEngine::GlobalIlluminationDebugView::HistoryLength;
        integratedRenderer->SetGlobalIlluminationSettings(motionHistoryDebug);
        const std::filesystem::path motionHistoryPfm =
            std::filesystem::path(HE_DEFAULT_PROJECT_DIR).parent_path() /
            "out/agent/captures/rtgi-motion-history-debug-linear.pfm";
        HEngine::TextureReadbackQueue motionHistoryReadback(
            renderDevice.GetDevice());
        HEngine::TextureReadbackHandle motionHistoryCapture;
        RenderOffscreenFrames(
            renderDevice,
            *integratedRenderer,
            world,
            Perf1080Width,
            Perf1080Height,
            1,
            &motionHistoryReadback,
            &motionHistoryCapture,
            motionHistoryPfm);
        renderDevice.WaitForIdle();
        motionHistoryReadback.Drain(std::numeric_limits<uint64_t>::max());
        if (motionHistoryReadback.GetResult(motionHistoryCapture).Status !=
            HEngine::TextureReadbackStatus::Complete)
        {
            throw std::runtime_error("RTGI motion history debug capture failed");
        }
        const std::array<float, 4> motionHistoryCoverage =
            ValidatePfmQuadrantCoverage(
                motionHistoryPfm,
                "RTGI motion history debug",
                false);
        if (*std::max_element(
                motionHistoryCoverage.begin(),
                motionHistoryCoverage.end()) < 64.0f)
        {
            throw std::runtime_error(
                "RTGI camera/object motion rejected all mature history");
        }
        motionHistoryReadback.Release(motionHistoryCapture);
        integratedRenderer->SetGlobalIlluminationSettings(settings);

        world.Camera.bCameraCut = true;
        RenderOffscreenFrames(
            renderDevice,
            *integratedRenderer,
            world,
            Perf1080Width,
            Perf1080Height,
            1);
        renderDevice.WaitForIdle();
        const uint32_t resetCountAfterCut =
            integratedRenderer->GetStats().GiStats.HistoryResetCount;
        if (resetCountAfterCut <= resetCountBeforeMotion)
            throw std::runtime_error("Explicit camera cut did not reset RTGI history");
        world.Camera.bCameraCut = false;
        world.Objects[1].Sections[0].MaterialInstance.Emissive *= 1.5f;
        ++world.Objects[1].Sections[0].MaterialInstance.Revision;
        RenderOffscreenFrames(
            renderDevice,
            *integratedRenderer,
            world,
            Perf1080Width,
            Perf1080Height,
            1);
        renderDevice.WaitForIdle();
        if (integratedRenderer->GetStats().GiStats.HistoryResetCount <=
            resetCountAfterCut)
        {
            throw std::runtime_error("Emissive-light generation change did not reset RTGI history");
        }
        std::cout
            << "RTGI_HISTORY_RUNTIME_PASS: objectMotionReset=0 cameraMotionReset=0 "
            << "cameraCutReset=1 lightStepReset=1\n";
        world = stableWorld;
        integratedRenderer.reset();

        constexpr uint32_t Perf1440Width = 2560;
        constexpr uint32_t Perf1440Height = 1440;
        auto integratedRenderer1440 = std::make_unique<HEngine::SceneRenderer>(
            HEngine::SceneRendererDesc{
                .Device = renderDevice.GetDevice(),
                .FramesInFlight = renderDevice.GetCapabilities().FramesInFlight,
                .ShaderCacheDirectory = dxilDirectory,
                .AssetRootDirectory =
                    HEngine::ConfigManager::GetInstance().GetEngineContentFolder(),
                .EngineAssetRootDirectory =
                    HEngine::ConfigManager::GetInstance().GetEngineContentFolder(),
                .Backend = renderDevice.GetBackend(),
                .Capabilities = renderDevice.GetCapabilities(),
                .GlobalIllumination = settings,
                .bEnableGpuTiming = true,
            });
        integratedRenderer1440->ConfigureOffscreenOutput(
            Perf1440Width,
            Perf1440Height,
            "Integrated RTGI 1440p Smoke");
        RenderOffscreenFrames(
            renderDevice,
            *integratedRenderer1440,
            world,
            Perf1440Width,
            Perf1440Height,
            32);
        renderDevice.WaitForIdle();
        integratedRenderer1440->CollectGpuTimingStats();
        const HEngine::SceneRendererStats& stats1440 =
            integratedRenderer1440->GetStats();
        if (stats1440.GpuRenderGraphSampleCount != 32u ||
            stats1440.RayTracingTlasBuildCount != 1u ||
            stats1440.RayTracingTlasUpdateCount != 0u ||
            stats1440.RenderGraphPeakFrameTransientBytes == 0u)
        {
            throw std::runtime_error("Integrated 1440p RTGI performance sample failed");
        }
        std::cout
            << "INTEGRATED_RTGI_1440P_SMOKE_PASS: resolution=2560x1440 frames=32 spp=1 bounces=2 scale=0.5 "
            << "gpuAverageMs=" << stats1440.GpuRenderGraphAverageMilliseconds
            << " transientPeakBytes="
            << stats1440.RenderGraphPeakFrameTransientBytes << '\n';
        std::cout << "INTEGRATED_RTGI_1440P_GPU_TIMINGS_BEGIN\n"
                  << integratedRenderer1440->BuildGpuTimingDebugDump()
                  << "\nINTEGRATED_RTGI_1440P_GPU_TIMINGS_END\n";
        integratedRenderer1440.reset();
        if (!bRunLegacyReferenceContracts) return;

        if (!HEngine::OfflineViewportRendererProviderRegistry::Get().Contains(
                "he.viewport.path-tracing"))
        {
            std::cout
                << "INTEGRATED_OFFLINE_RENDERER_SKIP: provider disabled for RenderSmoke\n";
            return;
        }

        auto integratedReference = std::make_unique<HEngine::SceneRenderer>(
            HEngine::SceneRendererDesc{
                .Device = renderDevice.GetDevice(),
                .FramesInFlight = renderDevice.GetCapabilities().FramesInFlight,
                .ShaderCacheDirectory = dxilDirectory,
                .AssetRootDirectory =
                    HEngine::ConfigManager::GetInstance().GetEngineContentFolder(),
                .EngineAssetRootDirectory =
                    HEngine::ConfigManager::GetInstance().GetEngineContentFolder(),
                .Backend = renderDevice.GetBackend(),
                .Capabilities = renderDevice.GetCapabilities(),
                .bEnableGpuTiming = true,
            });
        integratedReference->ConfigureOffscreenOutput(
            renderDevice.GetSwapChain().GetWidth(),
            renderDevice.GetSwapChain().GetHeight(),
            "Integrated Reference PT Smoke");
        if (integratedReference->GetOfflineViewportRendererSession() ||
            integratedReference->GetOfflineViewportBeautyTexture())
        {
            throw std::runtime_error(
                "A normal SceneRenderer unexpectedly created an offline session");
        }
        integratedReference->ActivateOfflineViewportRenderer(
            "he.viewport.path-tracing");
        HEngine::IOfflineViewportRendererSession* referenceSession =
            integratedReference->GetOfflineViewportRendererSession();
        if (!referenceSession ||
            integratedReference->GetActiveOfflineViewportRendererId() !=
                "he.viewport.path-tracing" ||
            HEngine::OfflineViewportRendererProviderRegistry::Get()
                    .GetActiveSessionCount("he.viewport.path-tracing") != 1u)
        {
            throw std::runtime_error(
                "Offline reference provider activation did not publish one session");
        }
        HEngine::OfflineViewportRendererSettings referenceSettings;
        referenceSettings.TargetSamples = 2;
        referenceSettings.SamplesPerFrame = 1;
        referenceSettings.MaxBounces = 4;
        referenceSettings.RenderScale = 0.5f;
        referenceSettings.SeedMode = HEngine::OfflineViewportSeedMode::Fixed;
        referenceSettings.Seed = 0x48454e47u;
        referenceSettings.bDirectOnly = false;
        referenceSettings.bAutoReset = true;
        referenceSettings.PreviewAov =
            HEngine::OfflineViewportAov::DirectDiffuse;
        referenceSession->ApplyOfflineSettings(referenceSettings);
        RenderFrames(
            window,
            renderDevice,
            *integratedReference,
            world,
            2,
            HEngine::SceneRendererOutputMode::OffscreenOnly,
            HEngine::RuntimeUiRenderMode::Offscreen);
        renderDevice.WaitForIdle();
        const HEngine::SceneRendererStats& referenceStats =
            integratedReference->GetStats();
        const HEngine::OfflineViewportRendererProgress referenceProgress =
            referenceSession->GetOfflineProgress();
        const bool bHasReferenceTrace = std::ranges::find(
            referenceStats.CompiledPasses,
            "ReferencePathTrace") != referenceStats.CompiledPasses.end();
        const bool bHasReferenceComposite = std::ranges::find(
            referenceStats.CompiledPasses,
            "OfflineViewportComposite") != referenceStats.CompiledPasses.end();
        const bool bHasReferenceAovPreview = std::ranges::find(
            referenceStats.CompiledPasses,
            "OfflineReferencePreview[direct-diffuse]") !=
                referenceStats.CompiledPasses.end();
        const bool bHasTransparent = std::ranges::find(
            referenceStats.CompiledPasses,
            "TransparentForwardHDR") != referenceStats.CompiledPasses.end();
        const bool bHasReferenceDenoiser = std::ranges::any_of(
            referenceStats.CompiledPasses,
            [](const std::string& passName)
            {
                return passName == "TemporalReprojection" ||
                    passName == "HistoryFix" ||
                    passName == "MomentsAndVariance" ||
                    passName.starts_with("ATrousFilter[") ||
                    passName == "BilateralUpsample";
            });
        if (integratedReference->RequiresContinuousUpdate() ||
            !bHasReferenceTrace || !bHasReferenceComposite ||
            !bHasReferenceAovPreview || bHasTransparent ||
            bHasReferenceDenoiser ||
            referenceStats.GiStats.AccumulatedSamples != 2u ||
            referenceStats.RayTracingInstanceCount != 2u ||
            referenceProgress.State !=
                HEngine::OfflineViewportSessionState::Complete ||
            referenceProgress.TargetSamples != 2u)
        {
            throw std::runtime_error(
                "Integrated SceneRenderer reference path tracer contract failed");
        }
        std::cout
            << "INTEGRATED_REFERENCE_PT_SMOKE_PASS: samples="
            << referenceStats.GiStats.AccumulatedSamples
            << " passes=" << referenceStats.CompiledPasses.size()
            << " instances=" << referenceStats.RayTracingInstanceCount
            << '\n';

        const uint64_t samplesBeforeFreeze =
            referenceStats.GiStats.AccumulatedSamples;
        const uint32_t resetsBeforeFreeze =
            referenceStats.GiStats.HistoryResetCount;
        referenceSettings.TargetSamples = static_cast<uint32_t>(
            samplesBeforeFreeze + 1u);
        referenceSession->ApplyOfflineSettings(referenceSettings);
        referenceSession->Pause();
        RenderOffscreenFrames(
            renderDevice,
            *integratedReference,
            world,
            renderDevice.GetSwapChain().GetWidth(),
            renderDevice.GetSwapChain().GetHeight(),
            1);
        renderDevice.WaitForIdle();
        const HEngine::SceneRendererStats frozenReferenceStats =
            integratedReference->GetStats();
        const HEngine::OfflineViewportRendererProgress frozenProgress =
            referenceSession->GetOfflineProgress();
        const bool bFrozenHasReferenceTrace = std::ranges::find(
            frozenReferenceStats.CompiledPasses,
            "ReferencePathTrace") != frozenReferenceStats.CompiledPasses.end();
        if (integratedReference->RequiresContinuousUpdate() ||
            bFrozenHasReferenceTrace ||
            frozenReferenceStats.GiStats.AccumulatedSamples !=
                samplesBeforeFreeze ||
            frozenReferenceStats.GiStats.HistoryResetCount != resetsBeforeFreeze ||
            frozenProgress.State != HEngine::OfflineViewportSessionState::Paused)
        {
            throw std::runtime_error(
                "Freezing reference accumulation traced, advanced, or reset history");
        }

        referenceSession->Resume();
        RenderOffscreenFrames(
            renderDevice,
            *integratedReference,
            world,
            renderDevice.GetSwapChain().GetWidth(),
            renderDevice.GetSwapChain().GetHeight(),
            1);
        renderDevice.WaitForIdle();
        const HEngine::SceneRendererStats resumedReferenceStats =
            integratedReference->GetStats();
        const HEngine::OfflineViewportRendererProgress resumedProgress =
            referenceSession->GetOfflineProgress();
        const bool bResumedHasReferenceTrace = std::ranges::find(
            resumedReferenceStats.CompiledPasses,
            "ReferencePathTrace") != resumedReferenceStats.CompiledPasses.end();
        if (integratedReference->RequiresContinuousUpdate() ||
            !bResumedHasReferenceTrace ||
            resumedReferenceStats.GiStats.AccumulatedSamples !=
                samplesBeforeFreeze + 1u ||
            resumedReferenceStats.GiStats.HistoryResetCount != resetsBeforeFreeze ||
            resumedProgress.State != HEngine::OfflineViewportSessionState::Complete)
        {
            throw std::runtime_error(
                "Resuming reference accumulation did not continue preserved history");
        }
        std::cout
            << "REFERENCE_FREEZE_RESUME_PASS: frozenSamples="
            << frozenReferenceStats.GiStats.AccumulatedSamples
            << " resumedSamples="
            << resumedReferenceStats.GiStats.AccumulatedSamples
            << " historyResets=0 frozenTracePass=0 resumedTracePass=1\n";

        const HEngine::RenderWorld referenceStableWorld = world;
        world.Camera.PreviousViewProjection = world.Camera.ViewProjection;
        world.Camera.PreviousNonJitteredViewProjection =
            world.Camera.NonJitteredViewProjection;
        world.Camera.Position.x += 0.04f;
        world.Camera.View = glm::lookAt(
            world.Camera.Position,
            glm::vec3(0.0f),
            glm::vec3(0.0f, 1.0f, 0.0f));
        world.Camera.ViewProjection = world.Camera.Projection * world.Camera.View;
        world.Camera.NonJitteredViewProjection = world.Camera.ViewProjection;
        RenderOffscreenFrames(
            renderDevice,
            *integratedReference,
            world,
            renderDevice.GetSwapChain().GetWidth(),
            renderDevice.GetSwapChain().GetHeight(),
            1);
        renderDevice.WaitForIdle();
        const HEngine::GlobalIlluminationStats cameraResetStats =
            integratedReference->GetStats().GiStats;
        if (cameraResetStats.AccumulatedSamples != 1u ||
            !HEngine::HasHistoryResetReason(
                cameraResetStats.LastResetReasons,
                HEngine::GlobalIlluminationHistoryResetReason::ExternalRendererState))
        {
            throw std::runtime_error(
                "Reference camera motion mixed two views into one accumulation");
        }

        world = referenceStableWorld;
        RenderOffscreenFrames(
            renderDevice,
            *integratedReference,
            world,
            renderDevice.GetSwapChain().GetWidth(),
            renderDevice.GetSwapChain().GetHeight(),
            1);
        renderDevice.WaitForIdle();
        world.Objects.front().PreviousWorldTransform =
            world.Objects.front().WorldTransform;
        world.Objects.front().WorldTransform = glm::translate(
            world.Objects.front().WorldTransform,
            glm::vec3(0.03f, 0.0f, 0.0f));
        RenderOffscreenFrames(
            renderDevice,
            *integratedReference,
            world,
            renderDevice.GetSwapChain().GetWidth(),
            renderDevice.GetSwapChain().GetHeight(),
            1);
        renderDevice.WaitForIdle();
        const HEngine::GlobalIlluminationStats objectResetStats =
            integratedReference->GetStats().GiStats;
        if (objectResetStats.AccumulatedSamples != 1u ||
            !HEngine::HasHistoryResetReason(
                objectResetStats.LastResetReasons,
                HEngine::GlobalIlluminationHistoryResetReason::ExternalRendererState))
        {
            throw std::runtime_error(
                "Reference object motion mixed two scene states into one accumulation");
        }
        std::cout
            << "REFERENCE_ACCUMULATION_INVALIDATION_PASS: cameraMotionReset=1 "
            << "objectMotionReset=1 samplesAfterChange=1\n";
        world = referenceStableWorld;

        for (uint32_t bounceCount : { 0u, 1u, 2u, 4u, 8u })
        {
            HEngine::OfflineViewportRendererSettings ladderSettings =
                referenceSettings;
            ladderSettings.TargetSamples = 1;
            ladderSettings.MaxBounces = bounceCount;
            ladderSettings.bDirectOnly = false;
            referenceSession->ApplyOfflineSettings(ladderSettings);
            RenderOffscreenFrames(
                renderDevice,
                *integratedReference,
                world,
                renderDevice.GetSwapChain().GetWidth(),
                renderDevice.GetSwapChain().GetHeight(),
                1);
            renderDevice.WaitForIdle();
            const HEngine::GlobalIlluminationStats ladderStats =
                integratedReference->GetStats().GiStats;
            if (ladderStats.AccumulatedSamples != 1u ||
                ladderStats.BounceCount != bounceCount)
            {
                throw std::runtime_error(
                    "Reference path tracer bounce ladder did not reset/record exactly");
            }
        }
        HEngine::OfflineViewportRendererSettings directOnlySettings =
            referenceSettings;
        directOnlySettings.TargetSamples = 1;
        directOnlySettings.MaxBounces = 4;
        directOnlySettings.bDirectOnly = true;
        referenceSession->ApplyOfflineSettings(directOnlySettings);
        RenderOffscreenFrames(
            renderDevice,
            *integratedReference,
            world,
            renderDevice.GetSwapChain().GetWidth(),
            renderDevice.GetSwapChain().GetHeight(),
            1);
        renderDevice.WaitForIdle();
        if (integratedReference->GetStats().GiStats.AccumulatedSamples != 1u)
            throw std::runtime_error("Reference direct-only mode did not record one sample");
        std::cout
            << "REFERENCE_BOUNCE_LADDER_PASS: directOnly=1 bounces=0,1,2,4,8 "
            << "fixedSeed=0x48454e47\n";
        integratedReference->DeactivateOfflineViewportRenderer();
        if (integratedReference->GetOfflineViewportRendererSession() ||
            integratedReference->GetOfflineViewportBeautyTexture() ||
            !integratedReference->GetActiveOfflineViewportRendererId().empty() ||
            HEngine::OfflineViewportRendererProviderRegistry::Get()
                    .GetActiveSessionCount("he.viewport.path-tracing") != 0u)
        {
            throw std::runtime_error(
                "Offline reference provider deactivation retained a session");
        }
        RenderOffscreenFrames(
            renderDevice,
            *integratedReference,
            world,
            renderDevice.GetSwapChain().GetWidth(),
            renderDevice.GetSwapChain().GetHeight(),
            1);
        renderDevice.WaitForIdle();
        const HEngine::SceneRendererStats& retiredReferenceStats =
            integratedReference->GetStats();
        const bool bRetiredReferencePass = std::ranges::find(
            retiredReferenceStats.CompiledPasses,
            "ReferencePathTrace") != retiredReferenceStats.CompiledPasses.end();
        if (retiredReferenceStats.RayTracingActiveAsAllocationRequirementBytes !=
                0u ||
            retiredReferenceStats.RayTracingActiveBufferAllocationRequirementBytes !=
                0u ||
            bRetiredReferencePass)
        {
            throw std::runtime_error(
                "Offline reference provider deactivation retained GPU resources");
        }
        std::cout
            << "OFFLINE_PROVIDER_LIFECYCLE_PASS: enabled=1 deactivated=1 "
            << "session=0 rtBytes=0 tracePass=0\n";
        integratedReference.reset();
    }

    void RunImportedCornellSurfaceAovSmoke(
        HEngine::RenderDevice& renderDevice,
        HEngine::RenderWorld& cornell,
        const std::filesystem::path& dxilDirectory)
    {
        constexpr uint32_t Width = 256;
        constexpr uint32_t Height = 256;
        auto primaryVisibility = std::make_unique<HEngine::SceneRenderer>(
            HEngine::SceneRendererDesc{
                .Device = renderDevice.GetDevice(),
                .FramesInFlight = renderDevice.GetCapabilities().FramesInFlight,
                .ShaderCacheDirectory = dxilDirectory,
                .AssetRootDirectory =
                    HEngine::ConfigManager::GetInstance().GetRootFolder(),
                .EngineAssetRootDirectory =
                    HEngine::ConfigManager::GetInstance().GetEngineContentFolder(),
                .Backend = renderDevice.GetBackend(),
                .Capabilities = renderDevice.GetCapabilities(),
            });
        primaryVisibility->ConfigureOffscreenOutput(
            Width,
            Height,
            "Cornell Surface AOV Primary Visibility");
        RenderOffscreenFrames(
            renderDevice,
            *primaryVisibility,
            cornell,
            Width,
            Height,
            1);
        renderDevice.WaitForIdle();
        const HEngine::SceneRendererOffscreenOutput primaryOutput =
            primaryVisibility->GetOffscreenOutput();
        if (!primaryOutput || !primaryOutput.Depth)
        {
            throw std::runtime_error(
                "Surface AOV smoke primary visibility has no depth output");
        }

        HEngine::RayTracingScene scene({
            .Device = renderDevice.GetDevice(),
            .FramesInFlight = renderDevice.GetCapabilities().FramesInFlight,
            .InitialInstanceCapacity = 16,
        });
        scene.Prepare(
            cornell,
            HEngine::RayTracingSceneRevisions{
                .Scene = cornell.SceneGeneration,
                .GeometryTopology = 1,
                .Material = 1,
                .Light = 1,
            },
            0);
        const std::span<const HEngine::RayTracingInstanceMapping> mappings =
            scene.GetInstanceMappings();
        const auto findMapping = [mappings](int entityId)
            -> const HEngine::RayTracingInstanceMapping*
        {
            const auto found = std::ranges::find_if(
                mappings,
                [entityId](const HEngine::RayTracingInstanceMapping& mapping)
                {
                    return mapping.EntityId == entityId;
                });
            return found == mappings.end() ? nullptr : &*found;
        };
        const HEngine::RayTracingInstanceMapping* instanceZero =
            scene.FindInstanceMapping(0u);
        const HEngine::RayTracingInstanceMapping* greenWall = findMapping(2004);
        const HEngine::RayTracingInstanceMapping* redWall = findMapping(2005);
        if (mappings.size() != 6u || !instanceZero || !greenWall || !redWall ||
            greenWall->StableInstanceId == 0u || redWall->StableInstanceId == 0u ||
            greenWall->StableInstanceId == redWall->StableInstanceId ||
            greenWall->MaterialIndex == instanceZero->MaterialIndex ||
            redWall->MaterialIndex == instanceZero->MaterialIndex ||
            greenWall->MaterialIndex == redWall->MaterialIndex)
        {
            throw std::runtime_error(
                "Surface AOV Cornell fixture does not expose distinct non-zero stable instances and materials");
        }

        uint32_t maximumMaterialIndex = 0;
        for (const HEngine::RayTracingInstanceMapping& mapping : mappings)
            maximumMaterialIndex = std::max(maximumMaterialIndex, mapping.MaterialIndex);
        std::vector<glm::vec3> expectedBaseColors(
            static_cast<size_t>(maximumMaterialIndex) + 1u,
            glm::vec3(0.0f));
        std::vector<bool> bExpectedMaterial(
            static_cast<size_t>(maximumMaterialIndex) + 1u,
            false);
        for (const HEngine::RayTracingInstanceMapping& mapping : mappings)
        {
            if (mapping.ObjectIndex >= cornell.Objects.size() ||
                mapping.SectionIndex >=
                    cornell.Objects[mapping.ObjectIndex].Sections.size())
            {
                throw std::runtime_error(
                    "Surface AOV Cornell instance mapping points outside the render world");
            }
            const glm::vec3 expected(
                cornell.Objects[mapping.ObjectIndex]
                    .Sections[mapping.SectionIndex]
                    .MaterialInstance.BaseColor);
            if (bExpectedMaterial[mapping.MaterialIndex])
            {
                const glm::vec3 difference = glm::abs(
                    expectedBaseColors[mapping.MaterialIndex] - expected);
                if (std::max({ difference.x, difference.y, difference.z }) > 1.0e-6f)
                {
                    throw std::runtime_error(
                        "Surface AOV Cornell deduplicated material has conflicting base colors");
                }
            }
            expectedBaseColors[mapping.MaterialIndex] = expected;
            bExpectedMaterial[mapping.MaterialIndex] = true;
        }

        HEngine::SceneSurfaceDataPass surfacePass({
            .Device = renderDevice.GetDevice(),
            .Scene = &scene,
            .ShaderArtifactCacheRoot = dxilDirectory.parent_path(),
            .Backend = renderDevice.GetBackend(),
        });
        surfacePass.PrepareFrame(cornell.Camera, Width, Height);

        const auto makeStaging = [&renderDevice](
            nvrhi::Format format,
            std::string_view debugName)
        {
            return renderDevice.GetDevice()->createStagingTexture(
                nvrhi::TextureDesc()
                    .setWidth(Width)
                    .setHeight(Height)
                    .setFormat(format)
                    .setDebugName(std::string(debugName)),
                nvrhi::CpuAccessMode::Read);
        };
        nvrhi::StagingTextureHandle baseColorStaging = makeStaging(
            nvrhi::Format::RGBA16_FLOAT,
            "Cornell Surface Base Color AOV Readback");
        nvrhi::StagingTextureHandle materialIdStaging = makeStaging(
            nvrhi::Format::R32_UINT,
            "Cornell Surface Material ID AOV Readback");
        nvrhi::StagingTextureHandle motionStaging = makeStaging(
            nvrhi::Format::RG16_FLOAT,
            "Cornell Surface Motion AOV Readback");
        nvrhi::StagingTextureHandle previousDepthStaging = makeStaging(
            nvrhi::Format::R32_FLOAT,
            "Cornell Surface Previous Depth AOV Readback");
        if (!baseColorStaging || !materialIdStaging || !motionStaging ||
            !previousDepthStaging)
        {
            throw std::runtime_error(
                "Surface AOV smoke failed to create GPU staging textures");
        }

        HEngine::RenderGraphResourcePool resourcePool(
            renderDevice.GetDevice(),
            renderDevice.GetCapabilities().FramesInFlight);
        HEngine::RenderGraph graph;
        graph.Resize(Width, Height);
        const HEngine::RayTracingSceneGraphResources sceneResources =
            scene.AddBuildPasses(graph);
        const HEngine::TextureHandle depth = graph.ImportTexture(
            HEngine::RenderGraphTextureDesc{
                .DebugName = "Cornell Surface AOV Primary Depth",
                .Width = Width,
                .Height = Height,
                .Format = nvrhi::Format::D32,
            },
            primaryOutput.Depth,
            nvrhi::ResourceStates::ShaderResource,
            nvrhi::ResourceStates::ShaderResource);
        const HEngine::SceneSurfaceData surface = surfacePass.AddPasses(
            graph,
            depth,
            sceneResources);
        graph.AddPass<SurfaceDataAovReadbackPassData>(
            "SurfaceDataAovReadback",
            HEngine::RenderGraphQueue::Copy,
            [surface](
                HEngine::RenderGraphBuilder& builder,
                SurfaceDataAovReadbackPassData& data)
            {
                data.BaseColorMetalness = builder.CopySource(
                    surface.BaseColorMetalness);
                data.MaterialId = builder.CopySource(surface.MaterialId);
                data.MotionVectors = builder.CopySource(surface.MotionVectors);
                data.PreviousDepth = builder.CopySource(surface.PreviousDepth);
                builder.SetSideEffect();
            },
            [baseColorStaging,
                materialIdStaging,
                motionStaging,
                previousDepthStaging](
                HEngine::RenderGraphContext& context,
                const SurfaceDataAovReadbackPassData& data)
            {
                const auto copy = [&context](
                    nvrhi::IStagingTexture* destination,
                    HEngine::TextureHandle source)
                {
                    context.GetCommandList()->copyTexture(
                        destination,
                        nvrhi::TextureSlice(),
                        context.GetTexture(source, HEngine::RenderGraphAccess::Read),
                        nvrhi::TextureSlice());
                };
                copy(baseColorStaging, data.BaseColorMetalness);
                copy(materialIdStaging, data.MaterialId);
                copy(motionStaging, data.MotionVectors);
                copy(previousDepthStaging, data.PreviousDepth);
            });
        graph.CompileOrThrow();
        const std::vector<std::string> passNames = graph.GetCompiledPassNames();
        if (std::ranges::find(passNames, "SceneSurfaceData") == passNames.end() ||
            std::ranges::find(passNames, "SurfaceDataAovReadback") ==
                passNames.end())
        {
            throw std::runtime_error(
                "Surface AOV smoke graph omitted raster or GPU readback work");
        }

        HEngine::FrameContext& frame = renderDevice.BeginFrame();
        bool bPoolActive = false;
        try
        {
            resourcePool.BeginFrame(
                frame.FrameIndex,
                frame.CompletedSubmissionInstance);
            bPoolActive = true;
            graph.Execute(HEngine::RenderGraphExecutionParams{
                .CommandList = frame.GraphicsCommandList,
                .ResourcePool = &resourcePool,
                .FrameSlot = frame.FrameIndex,
            });
            renderDevice.SubmitAndPresent(frame, false);
            resourcePool.EndFrame(frame.FrameIndex, frame.SubmissionInstance);
            bPoolActive = false;
            scene.NotifyFrameSubmitted(frame.SubmissionInstance);
        }
        catch (...)
        {
            if (frame.bSubmissionIssued)
            {
                if (bPoolActive)
                    resourcePool.EndFrame(frame.FrameIndex, frame.SubmissionInstance);
                scene.NotifyFrameSubmitted(frame.SubmissionInstance);
            }
            else
            {
                renderDevice.AbortFrame(frame);
                if (bPoolActive)
                    resourcePool.AbortFrame(frame.FrameIndex);
                scene.AbortFrame();
            }
            throw;
        }
        renderDevice.WaitForIdle();

        std::vector<uint32_t> materialIds(
            static_cast<size_t>(Width) * Height,
            HEngine::InvalidRayTracingSceneIndex);
        std::vector<uint64_t> materialPixelCounts(
            static_cast<size_t>(maximumMaterialIndex) + 1u,
            0u);
        uint64_t coveredPixelCount = 0;
        uint64_t unexpectedMaterialPixelCount = 0;
        size_t rowPitch = 0;
        const void* mapped = renderDevice.GetDevice()->mapStagingTexture(
            materialIdStaging,
            nvrhi::TextureSlice(),
            nvrhi::CpuAccessMode::Read,
            &rowPitch);
        if (!mapped)
            throw std::runtime_error("Surface AOV smoke failed to map MaterialId");
        for (uint32_t y = 0; y < Height; ++y)
        {
            const auto* row = reinterpret_cast<const uint32_t*>(
                static_cast<const uint8_t*>(mapped) + y * rowPitch);
            for (uint32_t x = 0; x < Width; ++x)
            {
                const uint32_t materialId = row[x];
                materialIds[static_cast<size_t>(y) * Width + x] = materialId;
                if (materialId == HEngine::InvalidRayTracingSceneIndex)
                    continue;
                ++coveredPixelCount;
                if (materialId >= materialPixelCounts.size() ||
                    !bExpectedMaterial[materialId])
                {
                    ++unexpectedMaterialPixelCount;
                    continue;
                }
                ++materialPixelCounts[materialId];
            }
        }
        renderDevice.GetDevice()->unmapStagingTexture(materialIdStaging);

        bool bFinite = true;
        float maximumBaseColorError = 0.0f;
        std::vector<uint64_t> matchingBaseColorPixelCounts(
            static_cast<size_t>(maximumMaterialIndex) + 1u,
            0u);
        std::vector<uint64_t> zeroBaseColorPixelCounts(
            static_cast<size_t>(maximumMaterialIndex) + 1u,
            0u);
        mapped = renderDevice.GetDevice()->mapStagingTexture(
            baseColorStaging,
            nvrhi::TextureSlice(),
            nvrhi::CpuAccessMode::Read,
            &rowPitch);
        if (!mapped)
            throw std::runtime_error("Surface AOV smoke failed to map BaseColor");
        for (uint32_t y = 0; y < Height; ++y)
        {
            const auto* row = reinterpret_cast<const uint16_t*>(
                static_cast<const uint8_t*>(mapped) + y * rowPitch);
            for (uint32_t x = 0; x < Width; ++x)
            {
                const uint32_t materialId =
                    materialIds[static_cast<size_t>(y) * Width + x];
                if (materialId == HEngine::InvalidRayTracingSceneIndex ||
                    materialId >= expectedBaseColors.size() ||
                    !bExpectedMaterial[materialId])
                {
                    continue;
                }
                const glm::vec3 actual(
                    HalfToFloat(row[x * 4u + 0u]),
                    HalfToFloat(row[x * 4u + 1u]),
                    HalfToFloat(row[x * 4u + 2u]));
                bFinite = bFinite && std::isfinite(actual.x) &&
                    std::isfinite(actual.y) && std::isfinite(actual.z);
                const glm::vec3 difference = glm::abs(
                    actual - expectedBaseColors[materialId]);
                const float pixelError = std::max({
                    difference.x,
                    difference.y,
                    difference.z,
                });
                maximumBaseColorError = std::max(
                    maximumBaseColorError,
                    pixelError);
                if (pixelError <= 0.002f)
                    ++matchingBaseColorPixelCounts[materialId];
                if (std::max({ actual.x, actual.y, actual.z }) <= 1.0e-6f)
                    ++zeroBaseColorPixelCounts[materialId];
            }
        }
        renderDevice.GetDevice()->unmapStagingTexture(baseColorStaging);

        float maximumMotionMagnitude = 0.0f;
        mapped = renderDevice.GetDevice()->mapStagingTexture(
            motionStaging,
            nvrhi::TextureSlice(),
            nvrhi::CpuAccessMode::Read,
            &rowPitch);
        if (!mapped)
            throw std::runtime_error("Surface AOV smoke failed to map motion vectors");
        for (uint32_t y = 0; y < Height; ++y)
        {
            const auto* row = reinterpret_cast<const uint16_t*>(
                static_cast<const uint8_t*>(mapped) + y * rowPitch);
            for (uint32_t x = 0; x < Width; ++x)
            {
                if (materialIds[static_cast<size_t>(y) * Width + x] ==
                    HEngine::InvalidRayTracingSceneIndex)
                {
                    continue;
                }
                const float motionX = HalfToFloat(row[x * 2u + 0u]);
                const float motionY = HalfToFloat(row[x * 2u + 1u]);
                bFinite = bFinite && std::isfinite(motionX) &&
                    std::isfinite(motionY);
                maximumMotionMagnitude = std::max(
                    maximumMotionMagnitude,
                    std::max(std::abs(motionX), std::abs(motionY)));
            }
        }
        renderDevice.GetDevice()->unmapStagingTexture(motionStaging);

        float minimumPreviousDepth = std::numeric_limits<float>::max();
        float maximumPreviousDepth = std::numeric_limits<float>::lowest();
        mapped = renderDevice.GetDevice()->mapStagingTexture(
            previousDepthStaging,
            nvrhi::TextureSlice(),
            nvrhi::CpuAccessMode::Read,
            &rowPitch);
        if (!mapped)
            throw std::runtime_error("Surface AOV smoke failed to map previous depth");
        for (uint32_t y = 0; y < Height; ++y)
        {
            const auto* row = reinterpret_cast<const float*>(
                static_cast<const uint8_t*>(mapped) + y * rowPitch);
            for (uint32_t x = 0; x < Width; ++x)
            {
                if (materialIds[static_cast<size_t>(y) * Width + x] ==
                    HEngine::InvalidRayTracingSceneIndex)
                {
                    continue;
                }
                const float previousDepth = row[x];
                bFinite = bFinite && std::isfinite(previousDepth);
                minimumPreviousDepth = std::min(
                    minimumPreviousDepth,
                    previousDepth);
                maximumPreviousDepth = std::max(
                    maximumPreviousDepth,
                    previousDepth);
            }
        }
        renderDevice.GetDevice()->unmapStagingTexture(previousDepthStaging);

        size_t visibleMaterialCount = 0;
        for (uint64_t count : materialPixelCounts)
            visibleMaterialCount += count > 0u ? 1u : 0u;
        const uint64_t greenPixels =
            materialPixelCounts[greenWall->MaterialIndex];
        const uint64_t redPixels = materialPixelCounts[redWall->MaterialIndex];
        for (size_t materialIndex = 0;
            materialIndex < materialPixelCounts.size();
            ++materialIndex)
        {
            std::cout
                << "SURFACE_DATA_AOV_BASE_COLOR: material=" << materialIndex
                << " pixels=" << materialPixelCounts[materialIndex]
                << " matching=" << matchingBaseColorPixelCounts[materialIndex]
                << " zero=" << zeroBaseColorPixelCounts[materialIndex] << '\n';
        }
        std::cout
            << "SURFACE_DATA_AOV_METRICS: covered=" << coveredPixelCount
            << " visibleMaterials=" << visibleMaterialCount
            << " instance0Stable=" << instanceZero->StableInstanceId
            << " instance0Material=" << instanceZero->MaterialIndex
            << " greenStable=" << greenWall->StableInstanceId
            << " greenMaterial=" << greenWall->MaterialIndex
            << " greenPixels=" << greenPixels
            << " redStable=" << redWall->StableInstanceId
            << " redMaterial=" << redWall->MaterialIndex
            << " redPixels=" << redPixels
            << " baseColorMaxError=" << maximumBaseColorError
            << " motionMaxAbs=" << maximumMotionMagnitude
            << " previousDepthRange=" << minimumPreviousDepth
            << ',' << maximumPreviousDepth << '\n';

        constexpr uint64_t MinimumWallPixels = 256u;
        if (!bFinite || coveredPixelCount == 0u || visibleMaterialCount < 3u ||
            unexpectedMaterialPixelCount != 0u ||
            greenPixels < MinimumWallPixels || redPixels < MinimumWallPixels ||
            maximumBaseColorError > 0.002f ||
            maximumMotionMagnitude > 1.0e-4f ||
            !(minimumPreviousDepth < maximumPreviousDepth))
        {
            std::ostringstream error;
            error
                << "Surface AOV stable-instance GPU readback failed: covered="
                << coveredPixelCount << " visibleMaterials=" << visibleMaterialCount
                << " unexpectedMaterialPixels=" << unexpectedMaterialPixelCount
                << " green(stable=" << greenWall->StableInstanceId
                << ", material=" << greenWall->MaterialIndex
                << ", pixels=" << greenPixels << ") red(stable="
                << redWall->StableInstanceId << ", material="
                << redWall->MaterialIndex << ", pixels=" << redPixels
                << ") instance0Material=" << instanceZero->MaterialIndex
                << " baseColorMaxError=" << maximumBaseColorError
                << " motionMaxAbs=" << maximumMotionMagnitude
                << " previousDepthRange=" << minimumPreviousDepth << ','
                << maximumPreviousDepth
                << "; distinct draws may still be reading stable instance 0";
            throw std::runtime_error(error.str());
        }
        std::cout
            << "SURFACE_DATA_AOV_PASS: imported Cornell BaseColor/MaterialId/Motion/PreviousDepth GPU readback preserved distinct stable instances\n";
    }

    void RunScreenSpaceGlobalIlluminationSmoke(
        HEngine::RenderDevice& renderDevice)
    {
        constexpr uint32_t Width = 512;
        constexpr uint32_t Height = 512;
        ImportedCornellWorld importedCornell = BuildImportedCornellWorld();
        const std::filesystem::path dxilDirectory =
            HEngine::ConfigManager::GetEngineGeneratedContentFullPath(
                "Shaders/CACHES/DXIL");
        const HEngine::GlobalIlluminationSettings settings{
            .Technique = HEngine::GlobalIlluminationTechniqueKind::ScreenSpace,
            .Denoiser = HEngine::GlobalIlluminationDenoiserKind::Svgf,
            .RenderScale = 1.0f,
            .MaxBounces = 2,
            .AtrousIterations = 4,
            .ScreenSpaceRayCount = 8,
            .ScreenSpaceStepCount = 128,
            .ScreenSpaceHiZStartMip = 0,
            .ScreenSpaceBinaryRefinementSteps = 6,
            .ScreenSpaceMaximumRayDistance = 1.0f,
            .ScreenSpaceThickness = 0.015f,
            .ScreenSpaceEdgeFade = 0.02f,
            .ScreenSpaceTemporalWeight = 0.97f,
            .bBilateralUpsample = true,
        };
        HEngine::RenderCapabilities screenSpaceOnlyCapabilities =
            renderDevice.GetCapabilities();
        const auto disableHardwareRayTracingFeature = [](
            HEngine::RenderFeatureStatus& feature)
        {
            feature.bSupported = false;
            feature.bEnabled = false;
            feature.Reason =
                "Explicitly disabled by the SSGI no-hardware-RT smoke.";
        };
        screenSpaceOnlyCapabilities.bNvrhiRayTracingAccelStruct = false;
        screenSpaceOnlyCapabilities.bNvrhiRayTracingPipeline = false;
        screenSpaceOnlyCapabilities.bNvrhiRayQuery = false;
        screenSpaceOnlyCapabilities.bNvrhiShaderExecutionReordering = false;
        screenSpaceOnlyCapabilities.bNvrhiRayTracingOpacityMicromap = false;
        screenSpaceOnlyCapabilities.bNvrhiNvapiBuildSupport = false;
        screenSpaceOnlyCapabilities.bNvrhiNativeOpacityMicromapBuildSupport =
            false;
        screenSpaceOnlyCapabilities.bHardwareRayTracingRendererReady = false;
        disableHardwareRayTracingFeature(
            screenSpaceOnlyCapabilities.RayTracingAccelStruct);
        disableHardwareRayTracingFeature(
            screenSpaceOnlyCapabilities.RayTracingPipeline);
        disableHardwareRayTracingFeature(screenSpaceOnlyCapabilities.RayQuery);
        disableHardwareRayTracingFeature(
            screenSpaceOnlyCapabilities.ShaderExecutionReordering);
        disableHardwareRayTracingFeature(
            screenSpaceOnlyCapabilities.RayTracingOpacityMicromap);
        disableHardwareRayTracingFeature(
            screenSpaceOnlyCapabilities.HardwareRayTracingRendererPath);
        HEngine::ResolveRendererCapabilityPolicy(
            screenSpaceOnlyCapabilities,
            false);
        const bool bAnyHardwareRayTracingCapability =
            screenSpaceOnlyCapabilities.bNvrhiRayTracingAccelStruct ||
            screenSpaceOnlyCapabilities.bNvrhiRayTracingPipeline ||
            screenSpaceOnlyCapabilities.bNvrhiRayQuery ||
            screenSpaceOnlyCapabilities.bNvrhiShaderExecutionReordering ||
            screenSpaceOnlyCapabilities.bNvrhiRayTracingOpacityMicromap ||
            screenSpaceOnlyCapabilities.bNvrhiNvapiBuildSupport ||
            screenSpaceOnlyCapabilities
                .bNvrhiNativeOpacityMicromapBuildSupport ||
            screenSpaceOnlyCapabilities.bHardwareRayTracingRendererReady ||
            screenSpaceOnlyCapabilities.Profile ==
                HEngine::RendererProfile::HardwareRayTracing ||
            screenSpaceOnlyCapabilities.RayTracingAccelStruct.bSupported ||
            screenSpaceOnlyCapabilities.RayTracingAccelStruct.bEnabled ||
            screenSpaceOnlyCapabilities.RayTracingPipeline.bSupported ||
            screenSpaceOnlyCapabilities.RayTracingPipeline.bEnabled ||
            screenSpaceOnlyCapabilities.RayQuery.bSupported ||
            screenSpaceOnlyCapabilities.RayQuery.bEnabled ||
            screenSpaceOnlyCapabilities.ShaderExecutionReordering.bSupported ||
            screenSpaceOnlyCapabilities.ShaderExecutionReordering.bEnabled ||
            screenSpaceOnlyCapabilities.RayTracingOpacityMicromap.bSupported ||
            screenSpaceOnlyCapabilities.RayTracingOpacityMicromap.bEnabled ||
            screenSpaceOnlyCapabilities.HardwareRayTracingRendererPath
                .bSupported ||
            screenSpaceOnlyCapabilities.HardwareRayTracingRendererPath
                .bEnabled;
        if (bAnyHardwareRayTracingCapability ||
            !screenSpaceOnlyCapabilities.BaselineRaster.bEnabled ||
            !screenSpaceOnlyCapabilities.bComputeQueue)
        {
            throw std::runtime_error(
                "SSGI no-hardware-RT smoke capability precondition failed");
        }
        auto renderer = std::make_unique<HEngine::SceneRenderer>(
            HEngine::SceneRendererDesc{
                .Device = renderDevice.GetDevice(),
                .FramesInFlight = renderDevice.GetCapabilities().FramesInFlight,
                .ShaderCacheDirectory = dxilDirectory,
                .AssetRootDirectory =
                    HEngine::ConfigManager::GetInstance().GetRootFolder(),
                .EngineAssetRootDirectory =
                    HEngine::ConfigManager::GetInstance().GetEngineContentFolder(),
                .Backend = renderDevice.GetBackend(),
                .Capabilities = screenSpaceOnlyCapabilities,
                .GlobalIllumination = settings,
                .bEnableGpuTiming = true,
            });
        const HEngine::GlobalIlluminationSelection initialSelection =
            renderer->GetGlobalIlluminationSelection();
        if (initialSelection.Requested !=
                HEngine::GlobalIlluminationTechniqueKind::ScreenSpace ||
            initialSelection.Selected !=
                HEngine::GlobalIlluminationTechniqueKind::ScreenSpace ||
            !initialSelection.bSupported || initialSelection.bFallback ||
            initialSelection.bUsesHardwareRayTracing)
        {
            throw std::runtime_error(
                "SSGI was not selected with all hardware RT capabilities disabled");
        }
        renderer->ConfigureOffscreenOutput(
            Width,
            Height,
            "Cornell SSGI Smoke");
        const std::filesystem::path outputPath =
            std::filesystem::path(HE_DEFAULT_PROJECT_DIR).parent_path() /
            "out/agent/captures/cornell-ssgi-optimized-120f-linear.pfm";
        const std::filesystem::path outputPng =
            std::filesystem::path(HE_DEFAULT_PROJECT_DIR).parent_path() /
            "out/agent/captures/cornell-ssgi-optimized-120f-final.png";
        HEngine::TextureReadbackQueue readback(renderDevice.GetDevice());
        HEngine::TextureReadbackHandle capture;
        HEngine::TextureReadbackQueue ldrReadback(renderDevice.GetDevice());
        HEngine::TextureReadbackHandle ldrCapture;
        RenderOffscreenFrames(
            renderDevice,
            *renderer,
            importedCornell.World,
            Width,
            Height,
            120,
            &readback,
            &capture,
            outputPath,
            &ldrReadback,
            &ldrCapture,
            outputPng);
        renderDevice.WaitForIdle();
        readback.Drain(std::numeric_limits<uint64_t>::max());
        ldrReadback.Drain(std::numeric_limits<uint64_t>::max());
        const HEngine::TextureReadbackResult result = readback.GetResult(capture);
        const HEngine::TextureReadbackResult ldrResult =
            ldrReadback.GetResult(ldrCapture);
        if (result.Status != HEngine::TextureReadbackStatus::Complete ||
            ldrResult.Status != HEngine::TextureReadbackStatus::Complete ||
            !std::filesystem::is_regular_file(outputPath) ||
            !std::filesystem::is_regular_file(outputPng))
        {
            throw std::runtime_error(
                "SSGI smoke linear HDR capture failed: " + result.Error);
        }
        const std::array<float, 4> coverage = ValidatePfmQuadrantCoverage(
            outputPath,
            "SSGI smoke",
            false);
        readback.Release(capture);
        ldrReadback.Release(ldrCapture);
        renderer->CollectGpuTimingStats();
        std::cout << "SSGI_STABLE_120_GPU_TIMINGS_BEGIN\n"
                  << renderer->BuildGpuTimingDebugDump()
                  << "\nSSGI_STABLE_120_GPU_TIMINGS_END\n";
        const std::filesystem::path staticSequenceDirectory =
            std::filesystem::path(HE_DEFAULT_PROJECT_DIR).parent_path() /
            "out/agent/captures/cornell-ssgi-static";
        std::filesystem::create_directories(staticSequenceDirectory);
        for (uint32_t sequenceFrame = 0; sequenceFrame < 8u; ++sequenceFrame)
        {
            std::ostringstream frameName;
            frameName << "frame_" << std::setw(3) << std::setfill('0')
                      << sequenceFrame << ".pfm";
            HEngine::TextureReadbackQueue sequenceReadback(
                renderDevice.GetDevice());
            HEngine::TextureReadbackHandle sequenceCapture;
            RenderOffscreenFrames(
                renderDevice,
                *renderer,
                importedCornell.World,
                Width,
                Height,
                1,
                &sequenceReadback,
                &sequenceCapture,
                staticSequenceDirectory / frameName.str());
            renderDevice.WaitForIdle();
            sequenceReadback.Drain(std::numeric_limits<uint64_t>::max());
            if (sequenceReadback.GetResult(sequenceCapture).Status !=
                HEngine::TextureReadbackStatus::Complete)
            {
                throw std::runtime_error("SSGI static sequence readback failed");
            }
            sequenceReadback.Release(sequenceCapture);
        }
        const auto captureDebugView = [
            &renderDevice,
            &renderer,
            &importedCornell,
            &settings,
            Width,
            Height](
            HEngine::GlobalIlluminationDebugView view,
            std::string_view fileStem)
        {
            HEngine::GlobalIlluminationSettings debugSettings = settings;
            debugSettings.DebugView = view;
            renderer->SetGlobalIlluminationSettings(debugSettings);
            const std::filesystem::path debugPath =
                std::filesystem::path(HE_DEFAULT_PROJECT_DIR).parent_path() /
                "out/agent/captures" /
                ("cornell-ssgi-" + std::string(fileStem) + "-linear.pfm");
            HEngine::TextureReadbackQueue debugReadback(
                renderDevice.GetDevice());
            HEngine::TextureReadbackHandle debugCapture;
            RenderOffscreenFrames(
                renderDevice,
                *renderer,
                importedCornell.World,
                Width,
                Height,
                1,
                &debugReadback,
                &debugCapture,
                debugPath);
            renderDevice.WaitForIdle();
            debugReadback.Drain(std::numeric_limits<uint64_t>::max());
            if (debugReadback.GetResult(debugCapture).Status !=
                HEngine::TextureReadbackStatus::Complete)
            {
                throw std::runtime_error(
                    "SSGI debug readback failed for " +
                    std::string(fileStem));
            }
            debugReadback.Release(debugCapture);
        };
        captureDebugView(
            HEngine::GlobalIlluminationDebugView::RawDiffuse,
            "raw-diffuse");
        captureDebugView(
            HEngine::GlobalIlluminationDebugView::RawDirectDiffuse,
            "raw-direct-diffuse");
        captureDebugView(
            HEngine::GlobalIlluminationDebugView::RawIndirectDiffuse,
            "raw-indirect-diffuse");
        captureDebugView(
            HEngine::GlobalIlluminationDebugView::HitDistance,
            "hit-distance");
        captureDebugView(
            HEngine::GlobalIlluminationDebugView::Confidence,
            "confidence");
        captureDebugView(
            HEngine::GlobalIlluminationDebugView::Coverage,
            "coverage");
        captureDebugView(
            HEngine::GlobalIlluminationDebugView::Albedo,
            "albedo");
        captureDebugView(
            HEngine::GlobalIlluminationDebugView::Normal,
            "normal");
        captureDebugView(
            HEngine::GlobalIlluminationDebugView::DemodulatedDiffuseIllumination,
            "demodulated-illumination");
        captureDebugView(
            HEngine::GlobalIlluminationDebugView::FilteredDiffuseIllumination,
            "filtered-illumination");
        captureDebugView(
            HEngine::GlobalIlluminationDebugView::RemodulatedDiffuse,
            "remodulated-radiance");
        captureDebugView(
            HEngine::GlobalIlluminationDebugView::DenoisedDiffuse,
            "denoised-diffuse");
        captureDebugView(
            HEngine::GlobalIlluminationDebugView::HistoryLength,
            "history-length");
        captureDebugView(
            HEngine::GlobalIlluminationDebugView::Variance,
            "variance");
        renderer->SetGlobalIlluminationSettings(settings);
        RenderOffscreenFrames(
            renderDevice,
            *renderer,
            importedCornell.World,
            Width,
            Height,
            1);
        renderDevice.WaitForIdle();
        renderer->CollectGpuTimingStats();
        const HEngine::SceneRendererStats& stats = renderer->GetStats();
        const HEngine::GlobalIlluminationSelection finalSelection =
            renderer->GetGlobalIlluminationSelection();
        const std::vector<std::string>& passNames = stats.CompiledPasses;
        const auto contains = [&passNames](std::string_view passName)
        {
            return std::ranges::find(passNames, passName) != passNames.end();
        };
        const auto passIndex = [&passNames](std::string_view passName)
        {
            const auto found = std::ranges::find(passNames, passName);
            return found == passNames.end()
                ? std::numeric_limits<size_t>::max()
                : static_cast<size_t>(std::distance(passNames.begin(), found));
        };
        const bool bPassOrderValid =
            passIndex("SceneUpload") < passIndex("SceneSurfaceTableUpload") &&
            passIndex("SceneSurfaceTableUpload") <
                passIndex("SceneDepthPrepass") &&
            passIndex("SceneDepthPrepass") < passIndex("SceneSurfaceData") &&
            passIndex("SceneSurfaceData") < passIndex("OpaqueForwardHDR") &&
            passIndex("OpaqueForwardHDR") < passIndex("DepthPyramidMip0") &&
            passIndex("DepthPyramidMip0") < passIndex("SSGIRayMarch") &&
            passIndex("SSGIRayMarch") < passIndex("TemporalReprojection") &&
            passIndex("TemporalReprojection") < passIndex("GIComposite") &&
            passIndex("GIComposite") < passIndex("TransparentForwardHDR");
        const bool bHasHardwareRtPass = std::ranges::any_of(
            passNames,
            [](const std::string& passName)
            {
                return passName.starts_with("HardwareRTBLAS") ||
                    passName.starts_with("HardwareRTTLAS") ||
                    passName == "RTDirectAndIndirectTrace" ||
                    passName == "ReferencePathTrace";
            });
        if (stats.GiStats.Technique !=
                HEngine::GlobalIlluminationTechniqueKind::ScreenSpace ||
            finalSelection.Requested !=
                HEngine::GlobalIlluminationTechniqueKind::ScreenSpace ||
            finalSelection.Selected !=
                HEngine::GlobalIlluminationTechniqueKind::ScreenSpace ||
            !finalSelection.bSupported || finalSelection.bFallback ||
            finalSelection.bUsesHardwareRayTracing ||
            stats.RayTracingBlasBuildCount != 0u ||
            stats.RayTracingTlasBuildCount != 0u ||
            stats.RayTracingTlasUpdateCount != 0u ||
            stats.RayTracingActiveAsAllocationRequirementBytes != 0u ||
            bHasHardwareRtPass ||
            !bPassOrderValid ||
            !contains("SceneSurfaceTableUpload") ||
            !contains("SceneSurfaceData") ||
            !contains("DepthPyramidMip0") ||
            !contains("SSGIRayMarch") ||
            !contains("TemporalReprojection") ||
            !contains("DiffuseRemodulation") ||
            !contains("GIComposite") ||
            *std::max_element(coverage.begin(), coverage.end()) <= 1e-5f)
        {
            throw std::runtime_error(
                "SSGI smoke created RT resources, omitted required passes, or produced no finite radiance");
        }
        std::cout
            << "SSGI_SMOKE_PASS: resolution=" << Width << 'x' << Height
            << " scale=1 rays=8 steps=128 bounces=2 mips="
            << HEngine::CalculateMipCount(Width, Height)
            << " capabilitiesRT=0 selected=ScreenSpace usesHardwareRT=0"
            << " BLAS=0 TLAS=0 asBytes=0 passes=" << passNames.size()
            << " surfaceTableLogicalBytes="
            << (stats.RayTracingGeometryBytes +
                stats.RayTracingSceneTableBytes)
            << " surfaceTableAllocationRequirementBytes="
            << stats.RayTracingActiveBufferAllocationRequirementBytes
            << " traceMs=" << stats.GpuGiTraceMilliseconds
            << " svgfMs=" << stats.GpuSvgfMilliseconds
            << " persistentBytes=" << stats.RenderGraphPersistentBytes
            << " transientReservedBytes="
            << stats.RenderGraphTransientReservedBytes
            << " transientPeakBytes=" << stats.RenderGraphPeakFrameTransientBytes
            << " path=" << outputPath.generic_string()
            << " png=" << outputPng.generic_string() << '\n';
        std::cout << "SSGI_GPU_TIMINGS_BEGIN\n"
                  << renderer->BuildGpuTimingDebugDump()
                  << "\nSSGI_GPU_TIMINGS_END\n";

        const std::filesystem::path motionRoot =
            std::filesystem::path(HE_DEFAULT_PROJECT_DIR).parent_path() /
            "out/agent/captures/cornell-ssgi-motion";
        const std::filesystem::path cameraMotionDirectory =
            motionRoot / "camera";
        const std::filesystem::path objectMotionDirectory =
            motionRoot / "object";
        const std::filesystem::path resizeDirectory = motionRoot / "resize";
        std::filesystem::create_directories(cameraMotionDirectory);
        std::filesystem::create_directories(objectMotionDirectory);
        std::filesystem::create_directories(resizeDirectory);

        const auto captureMotionFrame = [
            &renderDevice,
            &renderer,
            &importedCornell](
            const std::filesystem::path& directory,
            uint32_t sequenceFrame,
            uint32_t width,
            uint32_t height,
            std::string_view label)
        {
            std::ostringstream frameName;
            frameName << "frame_" << std::setw(3) << std::setfill('0')
                      << sequenceFrame << ".pfm";
            const std::filesystem::path framePath =
                directory / frameName.str();
            HEngine::TextureReadbackQueue motionReadback(
                renderDevice.GetDevice());
            HEngine::TextureReadbackHandle motionCapture;
            RenderOffscreenFrames(
                renderDevice,
                *renderer,
                importedCornell.World,
                width,
                height,
                1,
                &motionReadback,
                &motionCapture,
                framePath);
            renderDevice.WaitForIdle();
            motionReadback.Drain(std::numeric_limits<uint64_t>::max());
            const HEngine::TextureReadbackResult motionResult =
                motionReadback.GetResult(motionCapture);
            if (motionResult.Status !=
                    HEngine::TextureReadbackStatus::Complete ||
                !std::filesystem::is_regular_file(framePath))
            {
                throw std::runtime_error(
                    std::string(label) + " PFM readback failed: " +
                    motionResult.Error);
            }
            motionReadback.Release(motionCapture);
            const std::array<float, 4> luminance =
                ValidatePfmQuadrantCoverage(framePath, label, false);
            return *std::max_element(luminance.begin(), luminance.end());
        };

        HEngine::RenderWorld& motionWorld = importedCornell.World;
        const uint32_t resetsBeforeCameraMotion =
            renderer->GetStats().GiStats.HistoryResetCount;
        motionWorld.Camera.PreviousViewProjection =
            motionWorld.Camera.ViewProjection;
        motionWorld.Camera.PreviousNonJitteredViewProjection =
            motionWorld.Camera.NonJitteredViewProjection;
        motionWorld.Camera.Position += glm::vec3(0.015f, 0.0f, -0.01f);
        const glm::vec3 movedCameraForward = glm::normalize(
            glm::vec3(0.04f, 0.01f, 1.0f));
        motionWorld.Camera.View = glm::lookAt(
            motionWorld.Camera.Position,
            motionWorld.Camera.Position + movedCameraForward,
            glm::vec3(0.0f, 1.0f, 0.0f));
        motionWorld.Camera.ViewProjection =
            motionWorld.Camera.Projection * motionWorld.Camera.View;
        motionWorld.Camera.NonJitteredViewProjection =
            motionWorld.Camera.ViewProjection;
        float cameraMotionMaximumLuminance = 0.0f;
        for (uint32_t sequenceFrame = 0; sequenceFrame < 4u; ++sequenceFrame)
        {
            cameraMotionMaximumLuminance = std::max(
                cameraMotionMaximumLuminance,
                captureMotionFrame(
                    cameraMotionDirectory,
                    sequenceFrame,
                    Width,
                    Height,
                    "SSGI camera motion"));
            motionWorld.Camera.PreviousViewProjection =
                motionWorld.Camera.ViewProjection;
            motionWorld.Camera.PreviousNonJitteredViewProjection =
                motionWorld.Camera.NonJitteredViewProjection;
        }
        if (renderer->GetStats().GiStats.HistoryResetCount !=
            resetsBeforeCameraMotion)
        {
            throw std::runtime_error(
                "Ordinary SSGI camera translation/rotation reset full history");
        }
        std::cout
            << "SSGI_CAMERA_MOTION_READBACK: frames=4 historyResetDelta=0 "
            << "finiteNonEmpty=1 maxLuminance="
            << cameraMotionMaximumLuminance << " directory="
            << cameraMotionDirectory.generic_string() << '\n';

        if (importedCornell.ShortBlockObjectIndex >=
            motionWorld.Objects.size())
        {
            throw std::runtime_error(
                "SSGI motion fixture has no Cornell short block");
        }
        const uint32_t resetsBeforeObjectMotion =
            renderer->GetStats().GiStats.HistoryResetCount;
        HEngine::RenderObject& shortBlock =
            motionWorld.Objects[importedCornell.ShortBlockObjectIndex];
        shortBlock.PreviousWorldTransform = shortBlock.WorldTransform;
        shortBlock.WorldTransform = glm::translate(
            shortBlock.WorldTransform,
            glm::vec3(-0.05f, 0.0f, 0.0f));
        float objectMotionMaximumLuminance = 0.0f;
        for (uint32_t sequenceFrame = 0; sequenceFrame < 4u; ++sequenceFrame)
        {
            objectMotionMaximumLuminance = std::max(
                objectMotionMaximumLuminance,
                captureMotionFrame(
                    objectMotionDirectory,
                    sequenceFrame,
                    Width,
                    Height,
                    "SSGI short-block motion"));
            shortBlock.PreviousWorldTransform = shortBlock.WorldTransform;
        }
        if (renderer->GetStats().GiStats.HistoryResetCount !=
            resetsBeforeObjectMotion)
        {
            throw std::runtime_error(
                "Ordinary Cornell short-block motion reset full SSGI history");
        }
        std::cout
            << "SSGI_OBJECT_MOTION_READBACK: frames=4 translationX=-0.05 "
            << "historyResetDelta=0 finiteNonEmpty=1 maxLuminance="
            << objectMotionMaximumLuminance << " directory="
            << objectMotionDirectory.generic_string() << '\n';

        constexpr uint32_t ResizeWidth = 384;
        constexpr uint32_t ResizeHeight = 320;
        const uint32_t resetsBeforeResize =
            renderer->GetStats().GiStats.HistoryResetCount;
        renderDevice.WaitForIdle();
        renderer->Resize(ResizeWidth, ResizeHeight);
        renderer->ConfigureOffscreenOutput(
            ResizeWidth,
            ResizeHeight,
            "Cornell SSGI Smoke Resized");
        RenderOffscreenFrames(
            renderDevice,
            *renderer,
            motionWorld,
            ResizeWidth,
            ResizeHeight,
            1);
        renderDevice.WaitForIdle();
        const float resizeMaximumLuminance = captureMotionFrame(
            resizeDirectory,
            1,
            ResizeWidth,
            ResizeHeight,
            "SSGI resize");
        const HEngine::SceneRendererStats& resizeStats = renderer->GetStats();
        const bool bResizeHasHardwareRtPass = std::ranges::any_of(
            resizeStats.CompiledPasses,
            [](const std::string& passName)
            {
                return passName.starts_with("HardwareRTBLAS") ||
                    passName.starts_with("HardwareRTTLAS") ||
                    passName == "RTDirectAndIndirectTrace" ||
                    passName == "ReferencePathTrace";
            });
        if (resizeStats.GiStats.HistoryResetCount != resetsBeforeResize + 1u ||
            !HEngine::HasHistoryResetReason(
                resizeStats.GiStats.LastResetReasons,
                HEngine::GlobalIlluminationHistoryResetReason::Resize) ||
            resizeStats.RayTracingBlasBuildCount != 0u ||
            resizeStats.RayTracingTlasBuildCount != 0u ||
            resizeStats.RayTracingTlasUpdateCount != 0u ||
            resizeStats.RayTracingActiveAsAllocationRequirementBytes != 0u ||
            bResizeHasHardwareRtPass)
        {
            throw std::runtime_error(
                "SSGI resize did not reset history or created hardware RT work");
        }
        std::cout
            << "SSGI_RESIZE_READBACK: resolution=" << ResizeWidth << 'x'
            << ResizeHeight
            << " frames=2 historyResetDelta=1 resizeReason=1 "
            << "finiteNonEmpty=1 BLAS=0 TLAS=0 asBytes=0 rtPasses=0 "
            << "maxLuminance=" << resizeMaximumLuminance << " directory="
            << resizeDirectory.generic_string() << '\n';
    }

    void RunImportedCornellSmoke(HEngine::RenderDevice& renderDevice)
    {
        if (!HEngine::OfflineViewportRendererProviderRegistry::Get().Contains(
                "he.viewport.path-tracing"))
        {
            std::cout
                << "CORNELL_OFFLINE_RENDERER_SKIP: provider disabled for RenderSmoke\n";
            return;
        }
        constexpr uint32_t Width = 512;
        constexpr uint32_t Height = 512;
        ImportedCornellWorld importedCornell = BuildImportedCornellWorld();
        HEngine::RenderWorld& cornell = importedCornell.World;
        const std::filesystem::path dxilDirectory =
            HEngine::ConfigManager::GetEngineGeneratedContentFullPath(
                "Shaders/CACHES/DXIL");
        auto reference = std::make_unique<HEngine::SceneRenderer>(
            HEngine::SceneRendererDesc{
                .Device = renderDevice.GetDevice(),
                .FramesInFlight = renderDevice.GetCapabilities().FramesInFlight,
                .ShaderCacheDirectory = dxilDirectory,
                .AssetRootDirectory =
                    HEngine::ConfigManager::GetInstance().GetRootFolder(),
                .EngineAssetRootDirectory =
                    HEngine::ConfigManager::GetInstance().GetEngineContentFolder(),
                .Backend = renderDevice.GetBackend(),
                .Capabilities = renderDevice.GetCapabilities(),
                .bEnableGpuTiming = true,
            });
        reference->ConfigureOffscreenOutput(Width, Height, "Cornell Reference PT");
        reference->ActivateOfflineViewportRenderer(
            "he.viewport.path-tracing");
        HEngine::IOfflineViewportRendererSession* referenceSession =
            reference->GetOfflineViewportRendererSession();
        if (!referenceSession)
            throw std::runtime_error("Cornell offline reference session is missing");
        HEngine::OfflineViewportRendererSettings referenceSettings;
        referenceSettings.TargetSamples = 256;
        referenceSettings.SamplesPerFrame = 1;
        referenceSettings.MaxBounces = 8;
        referenceSettings.RenderScale = 1.0f;
        referenceSettings.FixedWidth = Width;
        referenceSettings.FixedHeight = Height;
        referenceSettings.SeedMode = HEngine::OfflineViewportSeedMode::Fixed;
        referenceSettings.Seed = 0x48454e47u;
        referenceSettings.bDirectOnly = false;
        referenceSettings.bAutoReset = true;
        referenceSettings.PreviewAov = HEngine::OfflineViewportAov::Beauty;
        referenceSession->ApplyOfflineSettings(referenceSettings);
        RenderOffscreenFrames(
            renderDevice,
            *reference,
            cornell,
            Width,
            Height,
            256);
        renderDevice.WaitForIdle();
        reference->CollectGpuTimingStats();
        const HEngine::SceneRendererStats& referenceStats = reference->GetStats();
        const HEngine::OfflineViewportRendererProgress referenceProgress =
            referenceSession->GetOfflineProgress();
        const std::span<const HEngine::OfflineViewportAov> referenceAovs =
            referenceSession->GetAvailableAovs();
        const HEngine::OfflineViewportNativeAov nativeBeauty =
            referenceSession->GetNativeAov(
                HEngine::OfflineViewportAov::Beauty);
        constexpr std::array<HEngine::OfflineViewportAov, 9> ExpectedAovs = {
            HEngine::OfflineViewportAov::Beauty,
            HEngine::OfflineViewportAov::DirectDiffuse,
            HEngine::OfflineViewportAov::IndirectDiffuse,
            HEngine::OfflineViewportAov::Albedo,
            HEngine::OfflineViewportAov::WorldNormal,
            HEngine::OfflineViewportAov::ViewDepth,
            HEngine::OfflineViewportAov::MaterialId,
            HEngine::OfflineViewportAov::InstanceId,
            HEngine::OfflineViewportAov::SampleCount,
        };
        std::cout
            << "CORNELL_REFERENCE_CONTRACT_DIAGNOSTIC: accumulated="
            << referenceStats.GiStats.AccumulatedSamples
            << " bounces=" << referenceStats.GiStats.BounceCount
            << " blas=" << referenceStats.RayTracingBlasBuildCount
            << " instances=" << referenceStats.RayTracingInstanceCount
            << " excluded=" << referenceStats.RayTracingExcludedInstanceCount
            << " emissive=" << referenceStats.RayTracingEmissiveTriangleCount
            << " gpuSamples=" << referenceStats.GpuRenderGraphSampleCount
            << " gpuValid=" << (referenceStats.bGpuGiTimingValid ? 1 : 0)
            << " traceMs=" << referenceStats.GpuGiTraceMilliseconds << '\n';
        if (referenceStats.GiStats.AccumulatedSamples != 256u ||
            referenceStats.GiStats.BounceCount != 8u ||
            referenceStats.RayTracingBlasBuildCount != 6u ||
            referenceStats.RayTracingInstanceCount != 6u ||
            referenceStats.RayTracingExcludedInstanceCount != 0u ||
            referenceStats.RayTracingEmissiveTriangleCount != 2u ||
            referenceStats.GpuRenderGraphSampleCount != 256u ||
            !referenceStats.bGpuGiTimingValid ||
            referenceStats.GpuGiTraceMilliseconds <= 0.0 ||
            referenceProgress.State !=
                HEngine::OfflineViewportSessionState::Complete ||
            referenceProgress.TargetSamples != 256u ||
            !std::ranges::equal(referenceAovs, ExpectedAovs) ||
            !nativeBeauty || nativeBeauty.Format != nvrhi::Format::RGBA32_FLOAT ||
            nativeBeauty.Width != Width || nativeBeauty.Height != Height ||
            !nativeBeauty.bSceneLinear ||
            nativeBeauty.Texture != reference->GetOfflineViewportBeautyTexture())
        {
            throw std::runtime_error(
                "Imported Cornell reference path tracer contract failed");
        }
        std::cout
            << "CORNELL_REFERENCE_PT_SMOKE_PASS: resolution=512x512 samples=256 "
            << "bounces=8 BLAS=6 instances=6 emissiveTriangles=2 excluded=0\n";
        std::cout
            << "CORNELL_REFERENCE_GPU_STATS: samples="
            << referenceStats.GpuRenderGraphSampleCount
            << " averagePassBodyMs="
            << referenceStats.GpuRenderGraphAverageMilliseconds
            << " traceLastMs=" << referenceStats.GpuGiTraceMilliseconds
            << " blasBuildEventMs="
            << referenceStats.GpuRayTracingBlasBuildEventMilliseconds
            << " tlasBuildEventMs="
            << referenceStats.GpuRayTracingTlasBuildEventMilliseconds
            << " tlasUpdateLastMs="
            << referenceStats.GpuRayTracingTlasUpdateMilliseconds
            << " rtLogicalPayloadBytes="
            << (referenceStats.RayTracingGeometryBytes +
                referenceStats.RayTracingSceneTableBytes)
            << " rtActiveBufferAllocationRequirementBytes="
            << referenceStats.RayTracingActiveBufferAllocationRequirementBytes
            << " rtActiveAsAllocationRequirementBytes="
            << referenceStats.RayTracingActiveAsAllocationRequirementBytes
            << " persistentBytes=" << referenceStats.RenderGraphPersistentBytes
            << " transientReservedBytes="
            << referenceStats.RenderGraphTransientReservedBytes
            << " transientPeakBytes="
            << referenceStats.RenderGraphPeakFrameTransientBytes << '\n';
        std::cout << "CORNELL_REFERENCE_GPU_TIMINGS_BEGIN\n"
                  << reference->BuildGpuTimingDebugDump()
                  << "\nCORNELL_REFERENCE_GPU_TIMINGS_END\n";
        const std::filesystem::path cornellReferencePfm =
            std::filesystem::path(HE_DEFAULT_PROJECT_DIR).parent_path() /
            "out/agent/captures/cornell-reference-256spp-linear.pfm";
        HEngine::TextureReadbackHandle beautyExport =
            referenceSession->RequestExport(
                HEngine::OfflineViewportAov::Beauty,
                cornellReferencePfm,
                HEngine::OfflineViewportOutputFormat::Pfm);
        RenderOffscreenFrames(
            renderDevice,
            *reference,
            cornell,
            Width,
            Height,
            1);
        renderDevice.WaitForIdle();
        const HEngine::TextureReadbackResult beautyExportResult =
            WaitForOfflineExport(*referenceSession, beautyExport);
        if (beautyExportResult.Status !=
                HEngine::TextureReadbackStatus::Complete ||
            !std::filesystem::is_regular_file(cornellReferencePfm))
        {
            throw std::runtime_error(
                "Offline beauty PFM export failed: " +
                beautyExportResult.Error);
        }
        referenceSession->ReleaseExport(beautyExport);
        std::cout << "CORNELL_REFERENCE_LINEAR_CAPTURE: "
                  << cornellReferencePfm.generic_string() << '\n';

        const std::filesystem::path cornellReferencePng =
            std::filesystem::path(HE_DEFAULT_PROJECT_DIR).parent_path() /
            "out/agent/captures/cornell-reference-256spp-final.png";
        HEngine::TextureReadbackHandle previewExport =
            referenceSession->RequestExport(
                HEngine::OfflineViewportAov::Beauty,
                cornellReferencePng,
                HEngine::OfflineViewportOutputFormat::ToneMappedPng);
        RenderOffscreenFrames(
            renderDevice,
            *reference,
            cornell,
            Width,
            Height,
            1);
        renderDevice.WaitForIdle();
        const HEngine::TextureReadbackResult previewExportResult =
            WaitForOfflineExport(*referenceSession, previewExport);
        if (previewExportResult.Status !=
                HEngine::TextureReadbackStatus::Complete ||
            !std::filesystem::is_regular_file(cornellReferencePng))
        {
            throw std::runtime_error(
                "Offline beauty PNG export failed: " +
                previewExportResult.Error);
        }
        referenceSession->ReleaseExport(previewExport);
        std::cout << "CORNELL_REFERENCE_FINAL_PNG: "
                  << cornellReferencePng.generic_string() << '\n';

        const auto captureReferenceAov = [
            &renderDevice,
            &reference,
            &cornell,
            referenceSession,
            Width,
            Height](
            HEngine::OfflineViewportAov aov,
            std::string_view fileStem)
        {
            const HEngine::OfflineViewportNativeAov nativeAov =
                referenceSession->GetNativeAov(aov);
            if (!nativeAov || nativeAov.Format != nvrhi::Format::RGBA32_FLOAT ||
                !nativeAov.bSceneLinear)
            {
                throw std::runtime_error(
                    "Reference export AOV has an invalid native contract: " +
                    std::string(fileStem));
            }
            const std::filesystem::path debugPath =
                std::filesystem::path(HE_DEFAULT_PROJECT_DIR).parent_path() /
                "out/agent/captures" /
                ("cornell-reference-" + std::string(fileStem) +
                    "-256spp-linear.pfm");
            const HEngine::TextureReadbackHandle debugExport =
                referenceSession->RequestExport(
                    aov,
                    debugPath,
                    HEngine::OfflineViewportOutputFormat::Pfm);
            RenderOffscreenFrames(
                renderDevice,
                *reference,
                cornell,
                Width,
                Height,
                1);
            renderDevice.WaitForIdle();
            const HEngine::TextureReadbackResult result =
                WaitForOfflineExport(*referenceSession, debugExport);
            if (result.Status != HEngine::TextureReadbackStatus::Complete ||
                !std::filesystem::is_regular_file(debugPath) ||
                referenceSession->GetOfflineProgress().AccumulatedSamples != 256u)
            {
                throw std::runtime_error(
                    "Reference debug AOV readback failed for " +
                    std::string(fileStem) + ": " + result.Error);
            }
            referenceSession->ReleaseExport(debugExport);
        };
        captureReferenceAov(
            HEngine::OfflineViewportAov::DirectDiffuse,
            "direct-diffuse");
        captureReferenceAov(
            HEngine::OfflineViewportAov::IndirectDiffuse,
            "indirect-diffuse");

        const std::filesystem::path rawExportPath =
            std::filesystem::path(HE_DEFAULT_PROJECT_DIR).parent_path() /
            "out/agent/captures/cornell-reference-sample-count.raw";
        const HEngine::TextureReadbackHandle rawExport =
            referenceSession->RequestExport(
                HEngine::OfflineViewportAov::SampleCount,
                rawExportPath,
                HEngine::OfflineViewportOutputFormat::Raw);
        RenderOffscreenFrames(
            renderDevice,
            *reference,
            cornell,
            Width,
            Height,
            1);
        renderDevice.WaitForIdle();
        const HEngine::TextureReadbackResult rawExportResult =
            WaitForOfflineExport(*referenceSession, rawExport);
        if (rawExportResult.Status != HEngine::TextureReadbackStatus::Complete ||
            !std::filesystem::is_regular_file(rawExportPath) ||
            !std::filesystem::is_regular_file(rawExportResult.MetadataPath))
        {
            throw std::runtime_error(
                "Offline raw export submission failed: " +
                rawExportResult.Error);
        }
        referenceSession->ReleaseExport(rawExport);

        const std::filesystem::path abortedExportPath =
            std::filesystem::path(HE_DEFAULT_PROJECT_DIR).parent_path() /
            "out/agent/captures/cornell-reference-aborted.raw";
        std::error_code removeAbortedError;
        std::filesystem::remove(abortedExportPath, removeAbortedError);
        std::filesystem::path abortedMetadataPath = abortedExportPath;
        abortedMetadataPath += ".json";
        removeAbortedError.clear();
        std::filesystem::remove(abortedMetadataPath, removeAbortedError);
        const HEngine::TextureReadbackHandle abortedExport =
            referenceSession->RequestExport(
                HEngine::OfflineViewportAov::WorldNormal,
                abortedExportPath,
                HEngine::OfflineViewportOutputFormat::Raw);
        ++cornell.FrameNumber;
        HEngine::FrameContext& abortedFrame = renderDevice.BeginFrame();
        try
        {
            HEngine::EffekseerRuntime::BeginHostFrame(abortedFrame.FrameIndex, abortedFrame.CompletedSubmissionInstance, abortedFrame.GraphicsCommandList);
            reference->RecordFrame(
                cornell,
                HEngine::SceneRendererFrameDesc{
                    .CommandList = abortedFrame.GraphicsCommandList,
                    .Width = Width,
                    .Height = Height,
                    .FrameSlot = abortedFrame.FrameIndex,
                    .CompletedSubmission =
                        abortedFrame.CompletedSubmissionInstance,
                    .OutputMode =
                        HEngine::SceneRendererOutputMode::OffscreenOnly,
                });
            renderDevice.AbortFrame(abortedFrame);
            HEngine::EffekseerRuntime::AbortHostFrame(abortedFrame.FrameIndex);
            reference->AbortFrame(abortedFrame.FrameIndex);
        }
        catch (...)
        {
            if (abortedFrame.bSubmissionIssued)
            {
                HEngine::EffekseerRuntime::NotifyHostFrameSubmitted(abortedFrame.FrameIndex, abortedFrame.SubmissionInstance);
                reference->NotifyFrameSubmitted(
                    abortedFrame.FrameIndex,
                    abortedFrame.SubmissionInstance);
            }
            else
            {
                renderDevice.AbortFrame(abortedFrame);
                HEngine::EffekseerRuntime::AbortHostFrame(abortedFrame.FrameIndex);
                reference->AbortFrame(abortedFrame.FrameIndex);
            }
            throw;
        }
        if (referenceSession->IsExportValid(abortedExport) ||
            std::filesystem::exists(abortedExportPath) ||
            std::filesystem::exists(abortedMetadataPath))
        {
            throw std::runtime_error(
                "Aborted offline export was not cancelled before submission");
        }
        referenceSession->Cancel();
        if (referenceSession->GetOfflineProgress().State !=
            HEngine::OfflineViewportSessionState::Cancelled)
        {
            throw std::runtime_error(
                "Offline reference Cancel did not publish Cancelled state");
        }
        referenceSettings.TargetSamples = 512;
        referenceSession->ApplyOfflineSettings(referenceSettings);
        referenceSession->Restart();
        std::cout
            << "OFFLINE_EXPORT_CONTRACT_PASS: pfm=complete raw=complete "
            << "rawMetadata=1 abortedExport=cancelled control=restart\n";

        const HEngine::SceneRendererStats& referenceLifecycleBaseline =
            reference->GetStats();
        const uint32_t baselineReferenceResetCount =
            referenceLifecycleBaseline.GiStats.HistoryResetCount;
        const uint32_t baselineReferenceBlasBuildCount =
            referenceLifecycleBaseline.RayTracingBlasBuildCount;
        const uint32_t baselineReferenceTlasUpdateCount =
            referenceLifecycleBaseline.RayTracingTlasUpdateCount;
        HEngine::RenderWorld movedCornell = cornell;
        HEngine::RenderObject& movedReferenceShortBlock =
            movedCornell.Objects[importedCornell.ShortBlockObjectIndex];
        movedReferenceShortBlock.WorldTransform = glm::translate(
            glm::mat4(1.0f),
            glm::vec3(-0.05f, 0.0f, 0.0f));
        movedReferenceShortBlock.PreviousWorldTransform =
            movedReferenceShortBlock.WorldTransform;
        RenderOffscreenFrames(
            renderDevice,
            *reference,
            movedCornell,
            Width,
            Height,
            512);
        renderDevice.WaitForIdle();
        reference->CollectGpuTimingStats();
        const HEngine::SceneRendererStats& movedReferenceStats =
            reference->GetStats();
        if (movedReferenceStats.GiStats.AccumulatedSamples != 512u ||
            movedReferenceStats.GiStats.BounceCount != 8u ||
            movedReferenceStats.GiStats.HistoryResetCount <=
                baselineReferenceResetCount ||
            !HEngine::HasHistoryResetReason(
                movedReferenceStats.GiStats.LastResetReasons,
                HEngine::GlobalIlluminationHistoryResetReason::ExternalRendererState) ||
            movedReferenceStats.RayTracingBlasBuildCount !=
                baselineReferenceBlasBuildCount ||
            movedReferenceStats.RayTracingTlasUpdateCount !=
                baselineReferenceTlasUpdateCount + 1u ||
            movedReferenceStats.RayTracingInstanceCount != 6u ||
            movedReferenceStats.RayTracingExcludedInstanceCount != 0u ||
            movedReferenceStats.RayTracingEmissiveTriangleCount != 2u)
        {
            throw std::runtime_error(
                "Moved Cornell Reference PT did not reset accumulation while reusing BLAS");
        }
        const std::filesystem::path movedReferencePfm =
            std::filesystem::path(HE_DEFAULT_PROJECT_DIR).parent_path() /
            "out/agent/captures/"
            "cornell-disocclusion-short-negx50mm-reference-512spp-linear.pfm";
        const HEngine::TextureReadbackHandle movedBeautyExport =
            referenceSession->RequestExport(
                HEngine::OfflineViewportAov::Beauty,
                movedReferencePfm,
                HEngine::OfflineViewportOutputFormat::Pfm);
        RenderOffscreenFrames(
            renderDevice,
            *reference,
            movedCornell,
            Width,
            Height,
            1);
        renderDevice.WaitForIdle();
        const HEngine::TextureReadbackResult movedBeautyResult =
            WaitForOfflineExport(*referenceSession, movedBeautyExport);
        if (movedBeautyResult.Status !=
                HEngine::TextureReadbackStatus::Complete ||
            !std::filesystem::is_regular_file(movedReferencePfm))
        {
            throw std::runtime_error(
                "Moved Cornell offline PFM export failed: " +
                movedBeautyResult.Error);
        }
        referenceSession->ReleaseExport(movedBeautyExport);
        std::cout
            << "CORNELL_DISOCCLUSION_REFERENCE_PASS: samples=512 bounces=8 "
            << "BLASRebuilds=0 instances=6 translationM=-0.05,0,0 path="
            << movedReferencePfm.generic_string() << '\n';
        reference->DeactivateOfflineViewportRenderer();
        if (reference->GetOfflineViewportRendererSession() ||
            reference->GetOfflineViewportBeautyTexture())
        {
            throw std::runtime_error(
                "Cornell offline session survived explicit deactivation");
        }
        reference.reset();

        const HEngine::GlobalIlluminationSettings realtimeSettings{
            .Technique =
                HEngine::GlobalIlluminationTechniqueKind::HardwareRayTracing,
            .Denoiser = HEngine::GlobalIlluminationDenoiserKind::Svgf,
            .RenderScale = 0.5f,
            .SamplesPerPixel = 1,
            .MaxBounces = 4,
            .AtrousIterations = 4,
            .bBilateralUpsample = true,
        };
        auto realtime = std::make_unique<HEngine::SceneRenderer>(
            HEngine::SceneRendererDesc{
                .Device = renderDevice.GetDevice(),
                .FramesInFlight = renderDevice.GetCapabilities().FramesInFlight,
                .ShaderCacheDirectory = dxilDirectory,
                .AssetRootDirectory =
                    HEngine::ConfigManager::GetInstance().GetRootFolder(),
                .EngineAssetRootDirectory =
                    HEngine::ConfigManager::GetInstance().GetEngineContentFolder(),
                .Backend = renderDevice.GetBackend(),
                .Capabilities = renderDevice.GetCapabilities(),
                .GlobalIllumination = realtimeSettings,
                .bEnableGpuTiming = true,
            });
        realtime->ConfigureOffscreenOutput(Width, Height, "Cornell RTGI 120 Frame");
        const std::filesystem::path cornellRtgiPfm =
            std::filesystem::path(HE_DEFAULT_PROJECT_DIR).parent_path() /
            "out/agent/captures/cornell-rtgi-120f-linear.pfm";
        HEngine::TextureReadbackQueue cornellRtgiReadback(
            renderDevice.GetDevice());
        HEngine::TextureReadbackHandle cornellRtgiCapture;
        RenderOffscreenFrames(
            renderDevice,
            *realtime,
            cornell,
            Width,
            Height,
            120,
            &cornellRtgiReadback,
            &cornellRtgiCapture,
            cornellRtgiPfm);
        renderDevice.WaitForIdle();
        cornellRtgiReadback.Drain(std::numeric_limits<uint64_t>::max());
        const HEngine::TextureReadbackResult cornellRtgiCaptureResult =
            cornellRtgiReadback.GetResult(cornellRtgiCapture);
        if (cornellRtgiCaptureResult.Status !=
                HEngine::TextureReadbackStatus::Complete ||
            !std::filesystem::is_regular_file(cornellRtgiPfm))
        {
            throw std::runtime_error(
                "Cornell RTGI linear HDR capture failed: " +
                cornellRtgiCaptureResult.Error);
        }
        realtime->CollectGpuTimingStats();
        const HEngine::SceneRendererStats& realtimeStats = realtime->GetStats();
        if (realtimeStats.RayTracingBlasBuildCount != 6u ||
            realtimeStats.RayTracingTlasBuildCount != 1u ||
            realtimeStats.RayTracingTlasUpdateCount != 0u ||
            realtimeStats.RayTracingInstanceCount != 6u ||
            realtimeStats.RayTracingExcludedInstanceCount != 0u ||
            realtimeStats.RayTracingEmissiveTriangleCount != 2u ||
            realtimeStats.GiStats.HistoryResetCount != 1u ||
            realtimeStats.GpuRenderGraphSampleCount != 120u ||
            !realtimeStats.bGpuGiTimingValid ||
            realtimeStats.GpuGiTraceMilliseconds <= 0.0 ||
            realtimeStats.GpuSvgfMilliseconds <= 0.0)
        {
            throw std::runtime_error(
                "Imported Cornell 120-frame RTGI history contract failed");
        }
        std::cout
            << "CORNELL_RTGI_120_FRAME_PASS: resolution=512x512 spp=1 bounces=4 "
            << "scale=0.5 atrous=4 tlasBuilds=1 tlasUpdates=0 historyResets=1 "
            << "instances=6 excluded=0\n";
        std::cout
            << "CORNELL_RTGI_GPU_STATS: samples="
            << realtimeStats.GpuRenderGraphSampleCount
            << " averagePassBodyMs="
            << realtimeStats.GpuRenderGraphAverageMilliseconds
            << " traceLastMs=" << realtimeStats.GpuGiTraceMilliseconds
            << " svgfLastMs=" << realtimeStats.GpuSvgfMilliseconds
            << " compositeLastMs="
            << realtimeStats.GpuGiCompositeMilliseconds
            << " blasBuildEventMs="
            << realtimeStats.GpuRayTracingBlasBuildEventMilliseconds
            << " tlasBuildEventMs="
            << realtimeStats.GpuRayTracingTlasBuildEventMilliseconds
            << " tlasUpdateLastMs="
            << realtimeStats.GpuRayTracingTlasUpdateMilliseconds << '\n';
        std::cout << "CORNELL_RTGI_GPU_TIMINGS_BEGIN\n"
                  << realtime->BuildGpuTimingDebugDump()
                  << "\nCORNELL_RTGI_GPU_TIMINGS_END\n";
        std::cout << "CORNELL_RTGI_LINEAR_CAPTURE: "
                  << cornellRtgiPfm.generic_string() << '\n';
        cornellRtgiReadback.Release(cornellRtgiCapture);

        const std::filesystem::path staticSequenceDirectory =
            std::filesystem::path(HE_DEFAULT_PROJECT_DIR).parent_path() /
            "out/agent/captures/cornell-rtgi-static";
        std::filesystem::create_directories(staticSequenceDirectory);
        for (uint32_t sequenceFrame = 0; sequenceFrame < 8u; ++sequenceFrame)
        {
            std::ostringstream frameName;
            frameName << "frame_" << std::setw(3) << std::setfill('0')
                      << sequenceFrame << ".pfm";
            const std::filesystem::path sequencePath =
                staticSequenceDirectory / frameName.str();
            HEngine::TextureReadbackQueue sequenceReadback(
                renderDevice.GetDevice());
            HEngine::TextureReadbackHandle sequenceCapture;
            RenderOffscreenFrames(
                renderDevice,
                *realtime,
                cornell,
                Width,
                Height,
                1,
                &sequenceReadback,
                &sequenceCapture,
                sequencePath);
            renderDevice.WaitForIdle();
            sequenceReadback.Drain(std::numeric_limits<uint64_t>::max());
            if (sequenceReadback.GetResult(sequenceCapture).Status !=
                HEngine::TextureReadbackStatus::Complete)
            {
                throw std::runtime_error("Cornell static RTGI sequence capture failed");
            }
            sequenceReadback.Release(sequenceCapture);
        }
        std::cout
            << "CORNELL_RTGI_STATIC_SEQUENCE_PASS: frames=8 startHistory=121 "
            << "path=" << staticSequenceDirectory.generic_string() << '\n';

        const auto captureDebugView = [
            &renderDevice,
            &realtime,
            &cornell,
            &realtimeSettings,
            Width,
            Height](
            HEngine::GlobalIlluminationDebugView view,
            std::string_view fileStem)
        {
            HEngine::GlobalIlluminationSettings debugSettings =
                realtimeSettings;
            debugSettings.DebugView = view;
            realtime->SetGlobalIlluminationSettings(debugSettings);
            const std::filesystem::path outputPath =
                std::filesystem::path(HE_DEFAULT_PROJECT_DIR).parent_path() /
                "out/agent/captures" /
                ("cornell-rtgi-" + std::string(fileStem) + "-linear.pfm");
            HEngine::TextureReadbackQueue readback(renderDevice.GetDevice());
            HEngine::TextureReadbackHandle capture;
            RenderOffscreenFrames(
                renderDevice,
                *realtime,
                cornell,
                Width,
                Height,
                1,
                &readback,
                &capture,
                outputPath);
            renderDevice.WaitForIdle();
            readback.Drain(std::numeric_limits<uint64_t>::max());
            const HEngine::TextureReadbackResult result =
                readback.GetResult(capture);
            if (result.Status != HEngine::TextureReadbackStatus::Complete ||
                !std::filesystem::is_regular_file(outputPath))
            {
                throw std::runtime_error(
                    "Cornell RTGI debug AOV capture failed for " +
                    std::string(fileStem) + ": " + result.Error);
            }
            readback.Release(capture);
            std::cout << "CORNELL_RTGI_DEBUG_AOV: view="
                      << static_cast<uint32_t>(view)
                      << " path=" << outputPath.generic_string() << '\n';
        };
        for (const auto& [view, fileStem] : std::array{
            std::pair{
                HEngine::GlobalIlluminationDebugView::RawDiffuse,
                std::string_view("raw-diffuse") },
            std::pair{
                HEngine::GlobalIlluminationDebugView::RawDirectDiffuse,
                std::string_view("raw-direct-diffuse") },
            std::pair{
                HEngine::GlobalIlluminationDebugView::RawIndirectDiffuse,
                std::string_view("raw-indirect-diffuse") },
            std::pair{
                HEngine::GlobalIlluminationDebugView::Albedo,
                std::string_view("albedo") },
            std::pair{
                HEngine::GlobalIlluminationDebugView::DemodulatedDiffuseIllumination,
                std::string_view("demodulated-illumination") },
            std::pair{
                HEngine::GlobalIlluminationDebugView::FilteredDiffuseIllumination,
                std::string_view("filtered-illumination") },
            std::pair{
                HEngine::GlobalIlluminationDebugView::RemodulatedDiffuse,
                std::string_view("remodulated-radiance") },
            std::pair{
                HEngine::GlobalIlluminationDebugView::DenoisedDiffuse,
                std::string_view("denoised-diffuse") },
            std::pair{
                HEngine::GlobalIlluminationDebugView::Normal,
                std::string_view("normal") },
            std::pair{
                HEngine::GlobalIlluminationDebugView::HitDistance,
                std::string_view("hit-distance") },
            std::pair{
                HEngine::GlobalIlluminationDebugView::HistoryLength,
                std::string_view("history-length") },
            std::pair{
                HEngine::GlobalIlluminationDebugView::Confidence,
                std::string_view("confidence") },
            std::pair{
                HEngine::GlobalIlluminationDebugView::Coverage,
                std::string_view("coverage") },
            std::pair{
                HEngine::GlobalIlluminationDebugView::Variance,
                std::string_view("variance") },
        })
        {
            captureDebugView(view, fileStem);
        }
        realtime->SetGlobalIlluminationSettings(realtimeSettings);

        const std::filesystem::path cornellRtgi512Pfm =
            std::filesystem::path(HE_DEFAULT_PROJECT_DIR).parent_path() /
            "out/agent/captures/cornell-rtgi-512f-linear.pfm";
        HEngine::TextureReadbackQueue rtgi512Readback(renderDevice.GetDevice());
        HEngine::TextureReadbackHandle rtgi512Capture;
        // 120 quality frames + 8 temporal frames + 14 debug-view frames have
        // already advanced the unchanged Cornell history to frame 142.
        RenderOffscreenFrames(
            renderDevice,
            *realtime,
            cornell,
            Width,
            Height,
            370,
            &rtgi512Readback,
            &rtgi512Capture,
            cornellRtgi512Pfm);
        renderDevice.WaitForIdle();
        rtgi512Readback.Drain(std::numeric_limits<uint64_t>::max());
        if (rtgi512Readback.GetResult(rtgi512Capture).Status !=
                HEngine::TextureReadbackStatus::Complete ||
            !std::filesystem::is_regular_file(cornellRtgi512Pfm))
        {
            throw std::runtime_error("Cornell 512-frame RTGI capture failed");
        }
        rtgi512Readback.Release(rtgi512Capture);
        std::cout
            << "CORNELL_RTGI_512_FRAME_PASS: resolution=512x512 scale=0.5 "
            << "spp=1 bounces=4 staticFrames=512 historyResets=1 path="
            << cornellRtgi512Pfm.generic_string() << '\n';

        HEngine::GlobalIlluminationSettings fullResolutionSettings =
            realtimeSettings;
        fullResolutionSettings.RenderScale = 1.0f;
        auto fullResolutionRealtime = std::make_unique<HEngine::SceneRenderer>(
            HEngine::SceneRendererDesc{
                .Device = renderDevice.GetDevice(),
                .FramesInFlight =
                    renderDevice.GetCapabilities().FramesInFlight,
                .ShaderCacheDirectory = dxilDirectory,
                .AssetRootDirectory =
                    HEngine::ConfigManager::GetInstance().GetRootFolder(),
                .EngineAssetRootDirectory = HEngine::ConfigManager::GetInstance()
                    .GetEngineContentFolder(),
                .Backend = renderDevice.GetBackend(),
                .Capabilities = renderDevice.GetCapabilities(),
                .GlobalIllumination = fullResolutionSettings,
                .bEnableGpuTiming = true,
            });
        fullResolutionRealtime->ConfigureOffscreenOutput(
            Width,
            Height,
            "Cornell Full Resolution RTGI");
        const std::filesystem::path cornellRtgiFullPfm =
            std::filesystem::path(HE_DEFAULT_PROJECT_DIR).parent_path() /
            "out/agent/captures/"
            "cornell-rtgi-optimized-full-120f-linear.pfm";
        const std::filesystem::path cornellRtgiFullPng =
            std::filesystem::path(HE_DEFAULT_PROJECT_DIR).parent_path() /
            "out/agent/captures/cornell-rtgi-optimized-full-120f-final.png";
        HEngine::TextureReadbackQueue rtgiFullReadback(renderDevice.GetDevice());
        HEngine::TextureReadbackHandle rtgiFullCapture;
        HEngine::TextureReadbackQueue rtgiFullLdrReadback(
            renderDevice.GetDevice());
        HEngine::TextureReadbackHandle rtgiFullLdrCapture;
        RenderOffscreenFrames(
            renderDevice,
            *fullResolutionRealtime,
            cornell,
            Width,
            Height,
            120,
            &rtgiFullReadback,
            &rtgiFullCapture,
            cornellRtgiFullPfm,
            &rtgiFullLdrReadback,
            &rtgiFullLdrCapture,
            cornellRtgiFullPng);
        renderDevice.WaitForIdle();
        rtgiFullReadback.Drain(std::numeric_limits<uint64_t>::max());
        rtgiFullLdrReadback.Drain(std::numeric_limits<uint64_t>::max());
        fullResolutionRealtime->CollectGpuTimingStats();
        const HEngine::SceneRendererStats& fullResolutionStats =
            fullResolutionRealtime->GetStats();
        if (rtgiFullReadback.GetResult(rtgiFullCapture).Status !=
                HEngine::TextureReadbackStatus::Complete ||
            rtgiFullLdrReadback.GetResult(rtgiFullLdrCapture).Status !=
                HEngine::TextureReadbackStatus::Complete ||
            !std::filesystem::is_regular_file(cornellRtgiFullPfm) ||
            !std::filesystem::is_regular_file(cornellRtgiFullPng) ||
            fullResolutionStats.GpuRenderGraphSampleCount != 120u ||
            fullResolutionStats.RayTracingTlasBuildCount != 1u ||
            fullResolutionStats.RayTracingTlasUpdateCount != 0u)
        {
            throw std::runtime_error("Cornell full-resolution RTGI smoke failed");
        }
        rtgiFullReadback.Release(rtgiFullCapture);
        rtgiFullLdrReadback.Release(rtgiFullLdrCapture);
        std::cout
            << "CORNELL_RTGI_FULL_120_FRAME_PASS: resolution=512x512 scale=1 "
            << "spp=1 bounces=4 frames=120 gpuMedian/P95/P99=see-following-dump "
            << "transientPeakBytes="
            << fullResolutionStats.RenderGraphPeakFrameTransientBytes
            << " path=" << cornellRtgiFullPfm.generic_string()
            << " png=" << cornellRtgiFullPng.generic_string() << '\n';
        std::cout << "CORNELL_RTGI_FULL_GPU_TIMINGS_BEGIN\n"
                  << fullResolutionRealtime->BuildGpuTimingDebugDump()
                  << "\nCORNELL_RTGI_FULL_GPU_TIMINGS_END\n";
        fullResolutionRealtime.reset();

        const std::filesystem::path disocclusionSequenceDirectory =
            std::filesystem::path(HE_DEFAULT_PROJECT_DIR).parent_path() /
            "out/agent/captures/"
            "cornell-disocclusion-short-negx50mm-16f";
        std::filesystem::create_directories(disocclusionSequenceDirectory);
        const auto captureDisocclusionFrame = [
            &renderDevice,
            &realtime,
            &cornell,
            Width,
            Height](const std::filesystem::path& outputPath)
        {
            HEngine::TextureReadbackQueue readback(renderDevice.GetDevice());
            HEngine::TextureReadbackHandle capture;
            RenderOffscreenFrames(
                renderDevice,
                *realtime,
                cornell,
                Width,
                Height,
                1,
                &readback,
                &capture,
                outputPath);
            renderDevice.WaitForIdle();
            readback.Drain(std::numeric_limits<uint64_t>::max());
            const HEngine::TextureReadbackResult result =
                readback.GetResult(capture);
            if (result.Status != HEngine::TextureReadbackStatus::Complete ||
                !std::filesystem::is_regular_file(outputPath))
            {
                throw std::runtime_error(
                    "Cornell disocclusion frame capture failed: " +
                    result.Error);
            }
            readback.Release(capture);
        };
        captureDisocclusionFrame(
            disocclusionSequenceDirectory / "pre_move.pfm");
        const HEngine::SceneRendererStats& beforeMotionStats =
            realtime->GetStats();
        const uint32_t beforeMotionHistoryResetCount =
            beforeMotionStats.GiStats.HistoryResetCount;
        const uint32_t beforeMotionBlasBuildCount =
            beforeMotionStats.RayTracingBlasBuildCount;
        const uint32_t beforeMotionTlasUpdateCount =
            beforeMotionStats.RayTracingTlasUpdateCount;
        HEngine::RenderObject& realtimeShortBlock =
            cornell.Objects[importedCornell.ShortBlockObjectIndex];
        if (realtimeShortBlock.EntityId != 2002 ||
            !AreMatricesExactlyEqual(
                realtimeShortBlock.WorldTransform,
                glm::mat4(1.0f)) ||
            !AreMatricesExactlyEqual(
                realtimeShortBlock.PreviousWorldTransform,
                glm::mat4(1.0f)))
        {
            throw std::runtime_error(
                "Cornell short-block stable identity or initial transform changed");
        }
        realtimeShortBlock.PreviousWorldTransform =
            realtimeShortBlock.WorldTransform;
        realtimeShortBlock.WorldTransform = glm::translate(
            glm::mat4(1.0f),
            glm::vec3(-0.05f, 0.0f, 0.0f));
        constexpr uint32_t DisocclusionFrameCount = 16;
        for (uint32_t sequenceFrame = 0;
            sequenceFrame < DisocclusionFrameCount;
            ++sequenceFrame)
        {
            std::ostringstream frameName;
            frameName << "frame_" << std::setw(3) << std::setfill('0')
                      << sequenceFrame << ".pfm";
            captureDisocclusionFrame(
                disocclusionSequenceDirectory / frameName.str());
            if (sequenceFrame == 0u &&
                AreMatricesExactlyEqual(
                    realtimeShortBlock.PreviousWorldTransform,
                    realtimeShortBlock.WorldTransform))
            {
                throw std::runtime_error(
                    "Cornell first disocclusion frame did not preserve the old transform");
            }
            realtimeShortBlock.PreviousWorldTransform =
                realtimeShortBlock.WorldTransform;
        }
        const HEngine::SceneRendererStats& afterMotionStats =
            realtime->GetStats();
        if (afterMotionStats.GiStats.HistoryResetCount !=
                beforeMotionHistoryResetCount ||
            afterMotionStats.RayTracingBlasBuildCount !=
                beforeMotionBlasBuildCount ||
            afterMotionStats.RayTracingTlasUpdateCount !=
                beforeMotionTlasUpdateCount + 2u ||
            afterMotionStats.RayTracingInstanceCount != 6u ||
            afterMotionStats.RayTracingExcludedInstanceCount != 0u ||
            afterMotionStats.RayTracingEmissiveTriangleCount != 2u ||
            !AreMatricesExactlyEqual(
                realtimeShortBlock.PreviousWorldTransform,
                realtimeShortBlock.WorldTransform))
        {
            throw std::runtime_error(
                "Cornell moving-block RTGI invalidated history, rebuilt BLAS, or lost TLAS motion");
        }
        const std::filesystem::path disocclusionMask =
            std::filesystem::path(HE_DEFAULT_PROJECT_DIR).parent_path() /
            "Project/SourceAssets/CornellBox/Generated/"
            "CornellBox.disocclusion-short-block-negx50mm.pgm";
        if (!std::filesystem::is_regular_file(disocclusionMask))
            throw std::runtime_error("Generated Cornell disocclusion ROI is missing");
        std::cout
            << "CORNELL_DISOCCLUSION_SEQUENCE_PASS: translationM=-0.05,0,0 "
            << "frames=16 historyResets=0 BLASRebuilds=0 TLASUpdates=2 "
            << "instances=6 roi="
            << disocclusionMask.generic_string() << " path="
            << disocclusionSequenceDirectory.generic_string() << '\n';

        HEngine::GlobalIlluminationSettings historyDebugSettings =
            realtimeSettings;
        historyDebugSettings.DebugView =
            HEngine::GlobalIlluminationDebugView::HistoryLength;
        realtime->SetGlobalIlluminationSettings(historyDebugSettings);
        const std::filesystem::path historyDebugPfm =
            std::filesystem::path(HE_DEFAULT_PROJECT_DIR).parent_path() /
            "out/agent/captures/cornell-rtgi-history-debug-linear.pfm";
        HEngine::TextureReadbackQueue historyDebugReadback(
            renderDevice.GetDevice());
        HEngine::TextureReadbackHandle historyDebugCapture;
        RenderOffscreenFrames(
            renderDevice,
            *realtime,
            cornell,
            Width,
            Height,
            2,
            &historyDebugReadback,
            &historyDebugCapture,
            historyDebugPfm);
        renderDevice.WaitForIdle();
        historyDebugReadback.Drain(std::numeric_limits<uint64_t>::max());
        if (historyDebugReadback.GetResult(historyDebugCapture).Status !=
            HEngine::TextureReadbackStatus::Complete)
        {
            throw std::runtime_error("Half-resolution history debug capture failed");
        }
        const std::array<float, 4> historyCoverage =
            ValidatePfmQuadrantCoverage(
                historyDebugPfm,
                "RTGI history debug");
        if (*std::min_element(
                historyCoverage.begin(),
                historyCoverage.end()) < 64.0f)
        {
            throw std::runtime_error(
                "RTGI debug-view switch reset or discarded mature history");
        }
        historyDebugReadback.Release(historyDebugCapture);

        HEngine::GlobalIlluminationSettings rawHalfSettings = realtimeSettings;
        rawHalfSettings.Denoiser =
            HEngine::GlobalIlluminationDenoiserKind::None;
        rawHalfSettings.DebugView =
            HEngine::GlobalIlluminationDebugView::Composite;
        realtime->SetGlobalIlluminationSettings(rawHalfSettings);
        const std::filesystem::path rawHalfPfm =
            std::filesystem::path(HE_DEFAULT_PROJECT_DIR).parent_path() /
            "out/agent/captures/cornell-rtgi-raw-half-linear.pfm";
        HEngine::TextureReadbackQueue rawHalfReadback(renderDevice.GetDevice());
        HEngine::TextureReadbackHandle rawHalfCapture;
        RenderOffscreenFrames(
            renderDevice,
            *realtime,
            cornell,
            Width,
            Height,
            2,
            &rawHalfReadback,
            &rawHalfCapture,
            rawHalfPfm);
        renderDevice.WaitForIdle();
        rawHalfReadback.Drain(std::numeric_limits<uint64_t>::max());
        if (rawHalfReadback.GetResult(rawHalfCapture).Status !=
            HEngine::TextureReadbackStatus::Complete)
        {
            throw std::runtime_error("Half-resolution raw GI capture failed");
        }
        (void)ValidatePfmQuadrantCoverage(
            rawHalfPfm,
            "RTGI raw half composite");
        rawHalfReadback.Release(rawHalfCapture);
        std::cout
            << "HALF_RES_GI_COMPOSITE_DEBUG_PASS: historyLength=fullCoverage "
            << "denoiserNone=fullCoverage\n";
        realtime.reset();
    }

    void RunHardwareRayTracingSmoke(
        HEngine::Window& window,
        HEngine::RenderDevice& renderDevice,
        bool bRunLegacyReferenceContracts = true)
    {
        const HEngine::HardwareRayTracingSupport support =
            HEngine::QueryHardwareRayTracingSupport(
                renderDevice.GetCapabilities());
        if (!support.bRealtimeRtgi)
        {
            std::cout << "HARDWARE_RT_SMOKE_SKIP: " << support.Reason << '\n';
            return;
        }

        constexpr uint32_t Width = 64;
        constexpr uint32_t Height = 64;
        HEngine::RenderWorld world = BuildHardwareRtSmokeWorld();
        HEngine::RayTracingScene scene({
            .Device = renderDevice.GetDevice(),
            .FramesInFlight = renderDevice.GetCapabilities().FramesInFlight,
            .InitialInstanceCapacity = 16,
        });
        scene.Prepare(
            world,
            HEngine::RayTracingSceneRevisions{
                .Scene = 1,
                .GeometryTopology = 1,
                .Material = 1,
                .Light = 1,
            },
            0);

        // Exercise the non-submitted recording rollback before the real trace.
        // This catches stale upload/AS initial-state assumptions that D3D12
        // promotion may hide but Vulkan validation would reject.
        {
            HEngine::RenderGraphResourcePool abortPool(
                renderDevice.GetDevice(),
                renderDevice.GetCapabilities().FramesInFlight);
            HEngine::RenderGraph abortGraph;
            abortGraph.Resize(Width, Height);
            const HEngine::RayTracingSceneGraphResources abortResources =
                scene.AddBuildPasses(abortGraph);
            abortGraph.AddPass<HardwareRtIntentionalAbortPassData>(
                "HardwareRTIntentionalAbort",
                HEngine::RenderGraphQueue::Graphics,
                [sceneTlas = abortResources.SceneTlas](
                    HEngine::RenderGraphBuilder& builder,
                    HardwareRtIntentionalAbortPassData& data)
                {
                    data.SceneTlas = builder.AccelerationStructureRead(sceneTlas);
                    builder.SetSideEffect();
                },
                [](HEngine::RenderGraphContext& context,
                    const HardwareRtIntentionalAbortPassData& data)
                {
                    (void)context.GetAccelerationStructure(
                        data.SceneTlas,
                        HEngine::RenderGraphAccess::Read);
                    throw std::runtime_error(
                        "intentional hardware RT recording abort");
                });
            abortGraph.CompileOrThrow();
            HEngine::FrameContext& abortFrame = renderDevice.BeginFrame();
            bool bAbortPoolActive = false;
            bool bObservedIntentionalAbort = false;
            try
            {
                abortPool.BeginFrame(
                    abortFrame.FrameIndex,
                    abortFrame.CompletedSubmissionInstance);
                bAbortPoolActive = true;
                abortGraph.Execute(HEngine::RenderGraphExecutionParams{
                    .CommandList = abortFrame.GraphicsCommandList,
                    .ResourcePool = &abortPool,
                    .FrameSlot = abortFrame.FrameIndex,
                });
            }
            catch (...)
            {
                const std::exception_ptr failure = std::current_exception();
                renderDevice.AbortFrame(abortFrame);
                if (bAbortPoolActive)
                    abortPool.AbortFrame(abortFrame.FrameIndex);
                scene.AbortFrame();
                try
                {
                    std::rethrow_exception(failure);
                }
                catch (const std::runtime_error& error)
                {
                    if (std::string_view(error.what()) ==
                        "intentional hardware RT recording abort")
                    {
                        bObservedIntentionalAbort = true;
                    }
                    else
                    {
                        std::rethrow_exception(failure);
                    }
                }
                catch (...)
                {
                    std::rethrow_exception(failure);
                }
            }
            if (!bObservedIntentionalAbort ||
                scene.GetStats().BlasBuildCount != 0u ||
                scene.GetStats().TlasBuildCount != 0u ||
                scene.GetStats().TlasUpdateCount != 0u)
            {
                throw std::runtime_error(
                    "Hardware RT aborted recording did not roll back scene state/statistics");
            }
            std::cout
                << "HARDWARE_RT_ABORT_ROLLBACK_PASS: uploadState=rolledBack "
                << "BLAS=0 TLASBuild=0 TLASUpdate=0\n";
        }

#if HE_WITH_OFFLINE_REFERENCE_RENDERER_SMOKE
        HEngine::ReferencePathTracer pathTracer({
            .Device = renderDevice.GetDevice(),
            .Scene = &scene,
            .ShaderArtifactCacheRoot =
                HEngine::ConfigManager::GetEngineGeneratedContentFullPath(
                    "Plugins/OfflineReferenceRenderer/Shaders/CACHES"),
            .Backend = renderDevice.GetBackend(),
            .FramesInFlight = renderDevice.GetCapabilities().FramesInFlight,
        });
        HEngine::GlobalIlluminationSettings settings{
            .Technique = HEngine::GlobalIlluminationTechniqueKind::ExternalViewportRenderer,
            .Denoiser = HEngine::GlobalIlluminationDenoiserKind::None,
            .RenderScale = 1.0f,
            .SamplesPerPixel = 1,
            .MaxBounces = 4,
        };
        pathTracer.PrepareFrame(
            HEngine::ReferencePathTracerFrameParameters{
                .Camera = world.Camera,
                .HistorySignature = {
                    .ProjectionFingerprint = 1,
                    .JitterSequenceGeneration = 1,
                    .SceneGeneration = 1,
                    .GeometryTopologyGeneration = 1,
                    .MaterialGeneration = 1,
                    .LightGeneration = 1,
                },
                .Width = Width,
                .Height = Height,
                .MaximumRayDistance = 100.0f,
            },
            settings);

        const HEngine::ReferencePathTracerNativeOutputs& nativeOutputs =
            pathTracer.GetNativeOutputs();
        nvrhi::StagingTextureHandle radianceStaging =
            renderDevice.GetDevice()->createStagingTexture(
                nativeOutputs.AccumulatedRadiance->getDesc(),
                nvrhi::CpuAccessMode::Read);
        nvrhi::StagingTextureHandle sampleCountStaging =
            renderDevice.GetDevice()->createStagingTexture(
                nativeOutputs.SampleCount->getDesc(),
                nvrhi::CpuAccessMode::Read);
        if (!radianceStaging || !sampleCountStaging)
            throw std::runtime_error("Hardware RT smoke failed to create staging textures");
        const std::filesystem::path hdrCaptureDirectory =
            std::filesystem::path(HE_DEFAULT_PROJECT_DIR).parent_path() /
            "out/agent/captures";
        std::filesystem::create_directories(hdrCaptureDirectory);
        const std::filesystem::path referencePfmPath =
            hdrCaptureDirectory / "hardware-rt-reference-linear.pfm";
        HEngine::TextureReadbackQueue referenceReadback(renderDevice.GetDevice());
        HEngine::TextureReadbackHandle referenceCapture;

        HEngine::RenderGraphResourcePool resourcePool(
            renderDevice.GetDevice(),
            renderDevice.GetCapabilities().FramesInFlight);
        HEngine::RenderGraph graph;
        graph.Resize(Width, Height);
        const HEngine::RayTracingSceneGraphResources sceneResources =
            scene.AddBuildPasses(graph);
        const HEngine::GlobalIlluminationOutputs outputs = pathTracer.AddPasses(
            graph,
            HEngine::GlobalIlluminationInputs{
                .SceneTlas = sceneResources.SceneTlas,
                .MaterialBuffer = sceneResources.Materials,
                .GeometryBuffer = sceneResources.Geometries,
                .LightBuffer = sceneResources.EmissiveTriangles,
                .RayTracingVertexBuffer = sceneResources.Vertices,
                .RayTracingIndexBuffer = sceneResources.Indices,
                .RayTracingInstanceBuffer = sceneResources.Instances,
                .AnalyticLightBuffer = sceneResources.AnalyticLights,
                .CurrentInverseViewProjection = glm::inverse(
                    world.Camera.ViewProjection),
                .PreviousViewProjection = world.Camera.PreviousViewProjection,
                .Width = Width,
                .Height = Height,
            },
            settings);
        graph.AddPass<HardwareRtReadbackPassData>(
            "HardwareRTSmokeReadback",
            HEngine::RenderGraphQueue::Copy,
            [radiance = pathTracer.GetOutputs().AccumulatedRadiance,
                sampleCount = pathTracer.GetOutputs().SampleCount](
                HEngine::RenderGraphBuilder& builder,
                HardwareRtReadbackPassData& data)
            {
                data.Radiance = builder.CopySource(radiance);
                data.SampleCount = builder.CopySource(sampleCount);
                builder.SetSideEffect();
            },
            [radianceStaging,
                sampleCountStaging,
                &referenceReadback,
                &referenceCapture,
                referencePfmPath](
                HEngine::RenderGraphContext& context,
                const HardwareRtReadbackPassData& data)
            {
                nvrhi::ITexture* radiance = context.GetTexture(
                    data.Radiance,
                    HEngine::RenderGraphAccess::Read);
                nvrhi::ITexture* sampleCount = context.GetTexture(
                    data.SampleCount,
                    HEngine::RenderGraphAccess::Read);
                context.GetCommandList()->copyTexture(
                    radianceStaging,
                    nvrhi::TextureSlice(),
                    radiance,
                    nvrhi::TextureSlice());
                context.GetCommandList()->copyTexture(
                    sampleCountStaging,
                    nvrhi::TextureSlice(),
                    sampleCount,
                    nvrhi::TextureSlice());
                referenceCapture = referenceReadback.Record(
                    context.GetCommandList(),
                    radiance,
                    HEngine::TextureReadbackDesc{
                        .OutputPath = referencePfmPath,
                        .OutputFormat = HEngine::TextureReadbackOutputFormat::Pfm,
                        .AlphaMode = HEngine::TextureReadbackAlphaMode::Discard,
                        .NonFinitePolicy =
                            HEngine::TextureReadbackNonFinitePolicy::Reject,
                        .bReplaceExisting = true,
                    });
            });
        (void)outputs;
        graph.CompileOrThrow();

        HEngine::FrameContext& frame = renderDevice.BeginFrame();
        bool bPoolActive = false;
        try
        {
            resourcePool.BeginFrame(
                frame.FrameIndex,
                frame.CompletedSubmissionInstance);
            bPoolActive = true;
            graph.Execute(HEngine::RenderGraphExecutionParams{
                .CommandList = frame.GraphicsCommandList,
                .ResourcePool = &resourcePool,
                .FrameSlot = frame.FrameIndex,
            });
            renderDevice.SubmitAndPresent(frame, false);
            resourcePool.EndFrame(frame.FrameIndex, frame.SubmissionInstance);
            bPoolActive = false;
            scene.NotifyFrameSubmitted(frame.SubmissionInstance);
            pathTracer.NotifyFrameSubmitted(frame.SubmissionInstance);
            referenceReadback.NotifySubmitted(
                referenceCapture,
                frame.SubmissionInstance);
        }
        catch (...)
        {
            if (frame.bSubmissionIssued)
            {
                if (bPoolActive)
                    resourcePool.EndFrame(frame.FrameIndex, frame.SubmissionInstance);
                scene.NotifyFrameSubmitted(frame.SubmissionInstance);
                pathTracer.NotifyFrameSubmitted(frame.SubmissionInstance);
                if (referenceCapture && referenceReadback.IsValid(referenceCapture) &&
                    referenceReadback.GetResult(referenceCapture).Status ==
                        HEngine::TextureReadbackStatus::PendingSubmission)
                {
                    referenceReadback.NotifySubmitted(
                        referenceCapture,
                        frame.SubmissionInstance);
                }
            }
            else
            {
                renderDevice.AbortFrame(frame);
                if (bPoolActive)
                    resourcePool.AbortFrame(frame.FrameIndex);
                scene.AbortFrame();
                pathTracer.AbortFrame();
                if (referenceCapture && referenceReadback.IsValid(referenceCapture))
                    referenceReadback.CancelPending(referenceCapture);
            }
            throw;
        }

        renderDevice.WaitForIdle();
        pathTracer.Collect(frame.SubmissionInstance); // Submission alone is not completed spp.
        referenceReadback.Drain(std::numeric_limits<uint64_t>::max());
        const HEngine::TextureReadbackResult referenceCaptureResult =
            referenceReadback.GetResult(referenceCapture);
        if (referenceCaptureResult.Status != HEngine::TextureReadbackStatus::Complete ||
            !std::filesystem::is_regular_file(referencePfmPath) ||
            std::filesystem::file_size(referencePfmPath) < 1024u)
        {
            throw std::runtime_error(
                "Reference path tracer did not produce a valid linear HDR PFM: " +
                referenceCaptureResult.Error);
        }
        size_t sampleRowPitch = 0;
        const void* sampleData = renderDevice.GetDevice()->mapStagingTexture(
            sampleCountStaging,
            nvrhi::TextureSlice(),
            nvrhi::CpuAccessMode::Read,
            &sampleRowPitch);
        if (!sampleData)
            throw std::runtime_error("Hardware RT smoke failed to map sample-count output");
        bool bSampleCountValid = true;
        for (uint32_t y = 0; y < Height && bSampleCountValid; ++y)
        {
            const auto* row = reinterpret_cast<const uint32_t*>(
                static_cast<const uint8_t*>(sampleData) + y * sampleRowPitch);
            for (uint32_t x = 0; x < Width; ++x)
            {
                if (row[x] != 1u)
                {
                    bSampleCountValid = false;
                    break;
                }
            }
        }
        renderDevice.GetDevice()->unmapStagingTexture(sampleCountStaging);

        size_t radianceRowPitch = 0;
        const void* radianceData = renderDevice.GetDevice()->mapStagingTexture(
            radianceStaging,
            nvrhi::TextureSlice(),
            nvrhi::CpuAccessMode::Read,
            &radianceRowPitch);
        if (!radianceData)
            throw std::runtime_error("Hardware RT smoke failed to map radiance output");
        float maximumLuminance = 0.0f;
        bool bRadianceFinite = true;
        for (uint32_t y = 0; y < Height; ++y)
        {
            const auto* row = reinterpret_cast<const float*>(
                static_cast<const uint8_t*>(radianceData) + y * radianceRowPitch);
            for (uint32_t x = 0; x < Width; ++x)
            {
                const float red = row[x * 4u + 0u];
                const float green = row[x * 4u + 1u];
                const float blue = row[x * 4u + 2u];
                if (!std::isfinite(red) || !std::isfinite(green) ||
                    !std::isfinite(blue))
                {
                    bRadianceFinite = false;
                    continue;
                }
                maximumLuminance = std::max(
                    maximumLuminance,
                    0.2126f * red + 0.7152f * green + 0.0722f * blue);
            }
        }
        renderDevice.GetDevice()->unmapStagingTexture(radianceStaging);

        const HEngine::RayTracingSceneStats& sceneStats = scene.GetStats();
        if (!bSampleCountValid || !bRadianceFinite || maximumLuminance <= 0.0f ||
            sceneStats.BlasBuildCount != 2u || sceneStats.TlasBuildCount != 1u ||
            sceneStats.TlasInstanceCount != 2u ||
            sceneStats.EmissiveTriangleCount != 2u ||
            pathTracer.GetStats().AccumulatedSamples != 1u)
        {
            throw std::runtime_error(
                "Hardware RT smoke did not build/trace a finite non-zero sample");
        }
        std::cout
            << "HARDWARE_RT_SMOKE_PASS: backend="
            << HEngine::ToString(renderDevice.GetBackend())
            << " BLAS=2 TLAS=1 instances=2 emissiveTriangles=2 samples=1 maxLuminance="
            << maximumLuminance << '\n';
        std::cout << "HARDWARE_RT_LINEAR_CAPTURE: "
                  << referencePfmPath.generic_string() << '\n';
        referenceReadback.Release(referenceCapture);
#endif
        RunRealtimeRtgiSmoke(window, renderDevice, scene, world, bRunLegacyReferenceContracts);
        if (bRunLegacyReferenceContracts) RunImportedCornellSmoke(renderDevice);
    }
    struct EarlyVfxSetupData
    {
        HEngine::TextureHandle SceneColor;
        HEngine::TextureHandle SceneDepth;
    };

    struct EarlyVfxConsumeData
    {
        HEngine::TextureHandle SceneColor;
    };

    [[nodiscard]] HEngine::RenderGraphTextureDesc MakeEarlyVfxTextureDesc(
        const char* debugName,
        nvrhi::Format format)
    {
        HEngine::RenderGraphTextureDesc desc;
        desc.DebugName = debugName;
        desc.Width = c_InitialWidth;
        desc.Height = c_InitialHeight;
        desc.Format = format;
        desc.Dimension = nvrhi::TextureDimension::Texture2D;
        desc.bUseClearValue = true;
        desc.ClearValue = format == nvrhi::Format::D32
            ? nvrhi::Color(1.0f, 0.0f, 0.0f, 0.0f)
            : nvrhi::Color(0.025f, 0.04f, 0.08f, 1.0f);
        return desc;
    }

    class EarlyVfxGraph final
    {
    public:
        EarlyVfxGraph(
            nvrhi::IDevice* device,
            uint32_t framesInFlight,
            const std::filesystem::path& shaderCacheDirectory)
            : mResourcePool(device, framesInFlight)
            , mVfxPass({
                .Device = device,
                .ShaderCacheDirectory = shaderCacheDirectory,
            })
        {
            mGraph.Resize(c_InitialWidth, c_InitialHeight);
            mGraph.AddPass<EarlyVfxSetupData>(
                "VFXSmokeSceneSetup",
                [this](HEngine::RenderGraphBuilder& builder, EarlyVfxSetupData& data)
                {
                    data.SceneColor = builder.CreateTexture(MakeEarlyVfxTextureDesc(
                        "VFX Smoke HDR Scene Color",
                        nvrhi::Format::RGBA16_FLOAT));
                    data.SceneDepth = builder.CreateTexture(MakeEarlyVfxTextureDesc(
                        "VFX Smoke Shader-Readable Depth",
                        nvrhi::Format::D32));
                    data.SceneColor = builder.RenderTarget(data.SceneColor);
                    data.SceneDepth = builder.DepthWrite(data.SceneDepth);
                    mSceneColor = data.SceneColor;
                    mSceneDepth = data.SceneDepth;
                },
                [](HEngine::RenderGraphContext& context, const EarlyVfxSetupData& data)
                {
                    context.GetCommandList()->clearTextureFloat(
                        context.GetTexture(data.SceneColor, HEngine::RenderGraphAccess::Write),
                        nvrhi::AllSubresources,
                        nvrhi::Color(0.025f, 0.04f, 0.08f, 1.f));
                    context.GetCommandList()->clearDepthStencilTexture(
                        context.GetTexture(data.SceneDepth, HEngine::RenderGraphAccess::Write),
                        nvrhi::AllSubresources,
                        true,
                        1.0f,
                        false,
                        0);
                });

            // Keep both optional resource paths active while the regression
            // effect records real CPU-sprite draws through the native bridge.
            const glm::vec3 cameraPosition(0.0f, 0.0f, 5.0f);
            const glm::mat4 view = glm::lookAt(
                cameraPosition,
                glm::vec3(0.0f),
                glm::vec3(0.0f, 1.0f, 0.0f));
            const glm::mat4 projection = glm::perspective(
                glm::radians(60.0f),
                static_cast<float>(c_InitialWidth) / static_cast<float>(c_InitialHeight),
                0.1f,
                1000.0f);
            const HEngine::EffekseerRenderGraphPassResult vfxResult = mVfxPass.AddToGraph(
                mGraph,
                HEngine::EffekseerRenderGraphInputs{
                    .SceneColor = mSceneColor,
                    .SceneDepth = mSceneDepth,
                    .Width = c_InitialWidth,
                    .Height = c_InitialHeight,
                    .View = view,
                    .Projection = projection,
                    .CameraPosition = cameraPosition,
                    .ZNear = 0.1f,
                    .ZFar = 1000.0f,
                },
                HEngine::VFXRenderRequirements{
                    .bHasActiveEffects = true,
                    .bRequiresDepthSnapshot = true,
                    .bRequiresBackgroundSnapshot = true,
                },
                true);
            if (!vfxResult.bAdded || !vfxResult.bDepthSnapshotAdded ||
                !vfxResult.bBackgroundCopyAdded)
            {
                throw std::runtime_error(
                    "Effekseer graph smoke did not declare every required optional resource path");
            }

            mGraph.AddPass<EarlyVfxConsumeData>(
                "VFXSmokeConsume",
                [this](HEngine::RenderGraphBuilder& builder, EarlyVfxConsumeData& data)
                {
                    data.SceneColor = builder.ShaderResource(mSceneColor);
                    builder.SetSideEffect();
                },
                [](HEngine::RenderGraphContext& context, const EarlyVfxConsumeData& data)
                {
                    (void)context.GetTexture(data.SceneColor, HEngine::RenderGraphAccess::Read);
                });

            mGraph.CompileOrThrow();
            const std::vector<std::string> expectedPasses = {
                "VFXSmokeSceneSetup",
                "VFXDepthSnapshot",
                "VFXBackgroundCopy",
                "VFXTransparent",
                "VFXSmokeConsume",
            };
            if (mGraph.GetCompiledPassNames() != expectedPasses)
                throw std::runtime_error("Effekseer graph smoke compiled an unexpected pass order");
        }

        void RecordFrame(const HEngine::FrameContext& frame)
        {
            mResourcePool.Collect(frame.CompletedSubmissionInstance);
            mResourcePool.BeginFrame(frame.FrameIndex, frame.CompletedSubmissionInstance);
            try
            {
                mGraph.Execute(HEngine::RenderGraphExecutionParams{
                    .CommandList = frame.GraphicsCommandList,
                    .ResourcePool = &mResourcePool,
                    .FrameSlot = frame.FrameIndex,
                });
            }
            catch (...)
            {
                mResourcePool.AbortFrame(frame.FrameIndex);
                throw;
            }
        }

        void NotifySubmitted(uint32_t frameSlot, uint64_t submission)
        {
            mResourcePool.EndFrame(frameSlot, submission);
        }

        void AbortFrame(uint32_t frameSlot) noexcept
        {
            mResourcePool.AbortFrame(frameSlot);
        }

    private:
        HEngine::RenderGraphResourcePool mResourcePool;
        HEngine::RenderGraph mGraph;
        HEngine::EffekseerRenderGraphPass mVfxPass;
        HEngine::TextureHandle mSceneColor;
        HEngine::TextureHandle mSceneDepth;
    };

}

namespace
{
    void RunEarlyVfxSmoke(HEngine::Window& window, HEngine::RenderDevice& device,
        const HEngine::EffekseerRuntimeDesc& runtimeDesc)
    {
        using Runtime = HEngine::EffekseerRuntime;
        const auto require = [](bool condition, const char* label) {
            if (!condition) throw std::runtime_error(std::string("EARLY_VFX_FAIL: ") + label);
            std::cout << "EARLY_VFX_CHECK_PASS: " << label << '\n';
        };
        const auto evidence = [](const char* stage) {
            const auto s = Runtime::GetStats();
            std::cout << "EARLY_VFX_STATS: stage=" << stage << " updates=" << s.UpdateSerial
                << " recordSerial=" << s.RecordingSerial << " dispatch=" << s.TotalRecordedDispatchCount
                << " draw=" << s.TotalRecordedDrawCount << " simulationMs=" << s.GpuSimulationMs.Current
                << " simulationSubmission=" << s.LastGpuSimulationSubmission
                << " renderMs=" << s.GpuRenderMs.Current << " renderSubmission=" << s.LastGpuRenderSubmission
                << " slotFailures=" << s.GpuEmitterSlotFailures << " particleFailures=" << s.GpuParticleBlockFailures
                << " trailFailures=" << s.GpuTrailBlockFailures << " emitters=" << s.GpuAllocatedEmitters
                << " reservedParticles=" << s.GpuReservedParticles << " reservedTrails=" << s.GpuReservedTrails << '\n';
        };
        require(Runtime::IsGpuParticlesEnabled(), "GPU explicitly enabled (requires HE_VFX_GPU_PARTICLES=1)");
        require(Runtime::GetStats().bDetailedStatsEnabled, "GPU timing instrumentation enabled");
        const char* configuredEffect = std::getenv("HE_VFX_EARLY_SMOKE_EFFECT");
        const std::string effect = configuredEffect && *configuredEffect ? configuredEffect :
            "Assets/Effects/Stress/GpuSparks20k.efkpkg";
        auto firstView = std::make_unique<EarlyVfxGraph>(device.GetDevice(), runtimeDesc.FramesInFlight,
            HEngine::ConfigManager::GetEngineGeneratedContentFullPath("Shaders/CACHES/DXIL"));
        auto secondView = std::make_unique<EarlyVfxGraph>(device.GetDevice(), runtimeDesc.FramesInFlight,
            HEngine::ConfigManager::GetEngineGeneratedContentFullPath("Shaders/CACHES/DXIL"));
        struct ProbeRetirementScope
        {
            HEngine::RenderDevice& Device;
            ~ProbeRetirementScope() { try { Device.WaitForIdle(); } catch (...) {} }
        } retirement{device};
        HEngine::Level noCameraLevel;
        noCameraLevel.SetRuntimePaused(false);
        HEngine::RenderSystem3D noCameraRenderer(&noCameraLevel);
        const auto update = [&] {
            // Level normally owns this submission scope. Even without a camera,
            // RenderSystem drains the current primitive snapshot before returning.
            HEngine::RenderPrimitiveCollector collector;
            collector.BeginFrame();
            HEngine::Renderer2D::CollectorScope collectorScope(collector);
            try
            {
                noCameraRenderer.OnUpdateRuntime(HEngine::Timestep(1.0f / 60.0f));
                collector.EndFrame();
            }
            catch (...)
            {
                collector.CancelFrame();
                throw;
            }
        };
        const auto checkAllocations = [&] {
            const auto s = Runtime::GetStats();
            require(s.GpuEmitterSlotFailures == 0 && s.GpuParticleBlockFailures == 0 && s.GpuTrailBlockFailures == 0,
                "no GPU emitter allocation failures");
        };
        device.SetSubmissionTimingEnabled(true);
        nvrhi::BufferHandle abortedVolatileBuffer;
        if (device.GetBackend() == HEngine::RenderBackend::Vulkan)
        {
            nvrhi::BufferDesc desc;
            desc.byteSize = 16;
            desc.isConstantBuffer = true;
            desc.isVolatile = true;
            desc.maxVersions = 1;
            desc.debugName = "EarlyVfxAbortSharedVolatile";
            abortedVolatileBuffer = device.GetDevice()->createBuffer(desc);
            require(abortedVolatileBuffer != nullptr, "shared one-version Vulkan abort buffer created");
        }
        uint64_t probeCpuFrame = 0;
        std::vector<uint64_t> abortedTimingSamples;
        uint64_t exceptionTimingSample = 0;
        bool exceptionHostAborted = false;
        const auto frame = [&](const std::function<void()>& action, bool views, bool abort) {
            window.OnUpdate();
            device.BeginGpuTimingFrame(++probeCpuFrame);
            struct TimingCancellationScope
            {
                HEngine::RenderDevice& Device;
                ~TimingCancellationScope() { Device.CancelGpuTimingFrame(); }
            } timingCancellation{device};
            auto& host = device.BeginFrame();
            bool firstRecorded = false, secondRecorded = false;
            bool bHostAborted = false;
            try
            {
                Runtime::BeginHostFrame(host.FrameIndex, host.CompletedSubmissionInstance, host.GraphicsCommandList);
                auto& swap = device.GetSwapChain();
                host.GraphicsCommandList->clearTextureFloat(swap.GetCurrentBackBuffer(), nvrhi::AllSubresources,
                    nvrhi::Color(0.02f, 0.03f, 0.05f, 1.0f));
                if (abort && abortedVolatileBuffer)
                {
                    const std::array<uint32_t, 4> marker{static_cast<uint32_t>(probeCpuFrame), 0, 0, 0};
                    host.GraphicsCommandList->writeBuffer(abortedVolatileBuffer, marker.data(), sizeof(marker));
                    // A leaked pending version makes the second write emit an
                    // NVRHI error. Run validation must remain clean; no old work
                    // is submitted to free this sole shared version artificially.
                    std::cout << "EARLY_VFX_ABORT_VOLATILE_WRITE: frame=" << probeCpuFrame
                        << " maxVersions=1 (requires zero NVRHI validation errors)\n";
                }
                action();
                if (views)
                {
                    const auto before = Runtime::GetStats();
                    firstView->RecordFrame(host); firstRecorded = true;
                    const auto middle = Runtime::GetStats();
                    secondView->RecordFrame(host); secondRecorded = true;
                    const auto after = Runtime::GetStats();
                    require(middle.TotalRecordedDrawCount > before.TotalRecordedDrawCount &&
                        after.TotalRecordedDrawCount > middle.TotalRecordedDrawCount, "two independent VFX view graphs record actual draws");
                    require(after.UpdateSerial == before.UpdateSerial &&
                        middle.TotalRecordedDispatchCount == before.TotalRecordedDispatchCount &&
                        after.TotalRecordedDispatchCount == before.TotalRecordedDispatchCount,
                        "two views do not repeat simulation");
                }
                if (abort)
                {
                    require(host.TimingSampleId != 0 && host.FullFrameTimer != nullptr,
                        "aborted frame has a real submitted timing begin marker");
                    abortedTimingSamples.push_back(host.TimingSampleId);
                    bHostAborted = true;
                    Runtime::AbortHostFrame(host.FrameIndex);
                    if (firstRecorded) firstView->AbortFrame(host.FrameIndex);
                    if (secondRecorded) secondView->AbortFrame(host.FrameIndex);
                    device.AbortFrame(host);
                    // Vulkan may submit an empty acquire-semaphore drain. It
                    // must never publish the abandoned SDK work as submitted.
                    require(Runtime::GetRenderBackendStatus() == HEngine::VFXRenderBackendStatus::Unconfigured,
                        "aborted simulation remains unconfigured after optional synchronization drain");
                    require(!host.bFullFrameTimerEnded, "aborted frame does not complete its GPU timer");
                }
                else
                {
                    device.SubmitAndPresent(host, false);
                    Runtime::NotifyHostFrameSubmitted(host.FrameIndex, host.SubmissionInstance);
                    if (firstRecorded) firstView->NotifySubmitted(host.FrameIndex, host.SubmissionInstance);
                    if (secondRecorded) secondView->NotifySubmitted(host.FrameIndex, host.SubmissionInstance);
                }
            }
            catch (...)
            {
                if (!bHostAborted && host.bSubmissionIssued)
                {
                    Runtime::NotifyHostFrameSubmitted(host.FrameIndex, host.SubmissionInstance);
                    if (firstRecorded) firstView->NotifySubmitted(host.FrameIndex, host.SubmissionInstance);
                    if (secondRecorded) secondView->NotifySubmitted(host.FrameIndex, host.SubmissionInstance);
                }
                else if (!bHostAborted)
                {
                    exceptionTimingSample = host.TimingSampleId;
                    bHostAborted = true;
                    Runtime::AbortHostFrame(host.FrameIndex);
                    if (firstRecorded) firstView->AbortFrame(host.FrameIndex);
                    if (secondRecorded) secondView->AbortFrame(host.FrameIndex);
                    device.AbortFrame(host);
                    exceptionHostAborted = !host.bFullFrameTimerEnded &&
                        Runtime::GetRenderBackendStatus() == HEngine::VFXRenderBackendStatus::Unconfigured;
                }
                throw;
            }
        };
        const auto retire = [&] {
            for (uint32_t i = 0; i < runtimeDesc.FramesInFlight + 2; ++i) frame([] {}, false, false);
            device.WaitForIdle();
        };
        const char* injectUploadFailure = std::getenv("HE_VFX_EARLY_SMOKE_INJECT_UPLOAD_FAILURE");
        const bool bInjectUploadFailure = injectUploadFailure && std::string_view(injectUploadFailure) == "1";
        if (bInjectUploadFailure)
        {
            // Do not rely on a GPU owner node also having a CPU renderer. This
            // fixture guarantees the nonempty StandardRenderer upload path.
            constexpr const char* cpuEffect = "Assets/Effects/AuthoringPathValidation/CpuSpriteControl.efkpkg";
            require(Runtime::PlayEffect(effect, glm::vec3(0.0f)) >= 0 &&
                Runtime::PlayEffect(cpuEffect, glm::vec3(0.0f)) >= 0,
                "upload failure fixtures load before the injected rendering failure");
            bool caughtSentinel = false;
            const auto beforeFault = Runtime::GetStats();
            try
            {
                frame([&] { for (int i = 0; i < 4; ++i) update(); }, true, false);
            }
            catch (const std::runtime_error& error)
            {
                if (std::string_view(error.what()) != "HE_VFX_EARLY_UPLOAD_FAILURE") throw;
                caughtSentinel = true;
            }
            require(caughtSentinel, "exact one-shot post-upload sentinel observed (not a load rejection)");
            require(exceptionHostAborted && exceptionTimingSample != 0,
                "exception abort invalidates SDK/backend recording and leaves full-frame timer incomplete");
            require(Runtime::GetStats().TotalRecordedDispatchCount > beforeFault.TotalRecordedDispatchCount,
                "exception abandons a host frame containing actual GPU simulation recordings");
            const auto faultSerial = Runtime::GetStats().UpdateSerial;
            bool rejected = false;
            frame([&] {
                try { update(); } catch (const std::logic_error&) { rejected = true; }
            }, false, false);
            require(rejected && Runtime::GetStats().UpdateSerial == faultSerial,
                "post-upload-failure update rejects before advancing simulation");
            device.WaitForIdle();
            checkAllocations();
            evidence("upload-failure-retired-before-reinitialize");
            const auto timings = device.TakeSubmissionTimings();
            const auto faultTiming = std::find_if(timings.begin(), timings.end(),
                [&](const auto& value) { return value.SampleId == exceptionTimingSample; });
            require(faultTiming != timings.end() && faultTiming->Status == HEngine::RenderSubmissionTimingStatus::Aborted &&
                faultTiming->BeginSubmissionId != 0 && faultTiming->MarkerSubmissionCount == 1,
                "post-upload-failure real full-frame timing retires as Aborted");
            std::cout << "EARLY_VFX_UPLOAD_FAILURE_TIMING: sample=" << faultTiming->SampleId
                << " beginSubmission=" << faultTiming->BeginSubmissionId << " endSubmission=" << faultTiming->EndSubmissionId
                << " status=Aborted milliseconds=INVALID\n";
            Runtime::Shutdown();
            Runtime::Initialize(runtimeDesc);
            require(Runtime::IsGpuParticlesEnabled(), "post-upload-failure reinitialize restores GPU capability");
            require(Runtime::PlayEffect(cpuEffect, glm::vec3(0.0f)) >= 0,
                "post-upload-failure CPU fixture restarts for real upload and draw recovery");
            const auto cpuRecovery = Runtime::GetStats();
            frame([&] { for (int i = 0; i < 4; ++i) update(); }, true, false);
            retire();
            require(Runtime::GetStats().TotalRecordedDrawCount > cpuRecovery.TotalRecordedDrawCount &&
                Runtime::GetStats().GpuRenderMs.Current > 0,
                "CPU-only fixture uploads and actually draws after exception reinitialize");
            // The original full early sequence below remains unchanged. Its
            // view draws now also exercise CPU upload after the consumed fault.
        }
        auto handle = Runtime::PlayEffect(effect, glm::vec3(0.0f));
        require(handle >= 0, "GPU effect starts");
        const auto initial = Runtime::GetStats();
        frame([&] {
            update();
            auto first = Runtime::GetStats();
            require(first.UpdateSerial == initial.UpdateSerial + 1 && first.TotalRecordedDispatchCount > initial.TotalRecordedDispatchCount,
                "no-camera update records compute before any view");
            require(Runtime::SetEffectTransform(handle, {1,0,0}, {0,0,1}), "intermediate transform accepted");
            update();
            const auto second = Runtime::GetStats();
            require(second.TotalRecordedDispatchCount > first.TotalRecordedDispatchCount, "second same-host update records distinct compute");
            require(Runtime::SetEffectPaused(handle, true), "intermediate pause accepted");
            update();
            require(Runtime::GetEffectStatus(handle).at("paused").get<bool>(), "paused state survives update");
            require(Runtime::SetEffectPaused(handle, false) && Runtime::SetEffectSpeed(handle, 0.5f), "resume and half-speed accepted");
            update();
            require(Runtime::SetEffectSpeed(handle, 1.0f), "normal speed restored");
            const auto end = Runtime::GetStats();
            require(end.UpdateSerial == initial.UpdateSerial + 4 && end.RecordingSerial == initial.RecordingSerial + 4,
                "four same-host updates produce four independent compute recordings");
            require(end.TotalRecordedDrawCount == initial.TotalRecordedDrawCount, "simulation submits without VFX draw");
            checkAllocations(); evidence("same-host-no-camera");
        }, false, false);
        frame(update, true, false);
        retire();
        auto timed = Runtime::GetStats();
        evidence("retired-before-abort");
        require(timed.GpuSimulationMs.SampleCount > 0 && timed.GpuSimulationMs.Current > 0 && timed.LastGpuSimulationSubmission != 0,
            "submitted simulation has completed positive GPU timer");
        require(timed.GpuRenderMs.SampleCount > 0 && timed.GpuRenderMs.Current > 0 && timed.LastGpuRenderSubmission != 0,
            "submitted views have completed positive GPU timer");
        frame(update, false, true);
        require(Runtime::GetRenderBackendStatus() == HEngine::VFXRenderBackendStatus::Unconfigured, "abort invalidates advanced simulation");
        const auto abortedSerial = Runtime::GetStats().UpdateSerial;
        frame([&] {
            bool rejected = false;
            try { update(); } catch (const std::logic_error&) { rejected = true; }
            require(rejected && Runtime::GetStats().UpdateSerial == abortedSerial,
                "post-abort update rejected before CPU simulation advances");
        }, false, true);
        device.WaitForIdle();
        checkAllocations();
        evidence("aborted-before-reinitialize");
        const auto submissionTimings = device.TakeSubmissionTimings();
        for (const auto sampleId : abortedTimingSamples)
        {
            const auto sample = std::find_if(submissionTimings.begin(), submissionTimings.end(),
                [sampleId](const auto& value) { return value.SampleId == sampleId; });
            require(sample != submissionTimings.end() &&
                sample->Status == HEngine::RenderSubmissionTimingStatus::Aborted &&
                sample->BeginSubmissionId != 0 && sample->MarkerSubmissionCount == 1,
                "actual aborted full-frame timing sample retires without becoming Complete");
            std::cout << "EARLY_VFX_ABORT_TIMING: sample=" << sample->SampleId
                << " cpuFrame=" << sample->CpuFrameId << " beginSubmission=" << sample->BeginSubmissionId
                << " endSubmission=" << sample->EndSubmissionId
                << " status=Aborted milliseconds=INVALID\n";
        }
        require(abortedTimingSamples.size() == 2, "both aborted host frames have retired timing evidence");
        Runtime::Shutdown();
        Runtime::Initialize(runtimeDesc);
        require(Runtime::IsGpuParticlesEnabled(), "same-process reinitialize restores GPU capability");
        const auto reinitialized = Runtime::GetStats();
        handle = Runtime::PlayEffect(effect, glm::vec3(0.0f));
        require(handle >= 0, "reinitialized GPU effect starts");
        frame(update, true, false);
        retire();
        const auto restored = Runtime::GetStats();
        evidence("reinitialized");
        require(restored.TotalRecordedDispatchCount > reinitialized.TotalRecordedDispatchCount &&
            restored.GpuSimulationMs.Current > 0 && restored.LastGpuSimulationSubmission > reinitialized.LastGpuSimulationSubmission,
            "reinitialized GPU simulation executes and retires");
        Runtime::StopAllEffects();
        for (uint32_t i = 0; i < runtimeDesc.FramesInFlight + 2; ++i) frame(update, false, false);
        retire();
        checkAllocations();
        const auto stopped = Runtime::GetStats();
        evidence("stopped");
        require(stopped.GpuAllocatedEmitters == 0 && stopped.GpuReservedParticles == 0 && stopped.GpuReservedTrails == 0,
            "stopped CPU allocator ownership returns to zero");
        using Quality = HEngine::VFXQualityTier;
        const auto requirePolicy = [&](Quality quality, bool requested, bool enabled) {
            const auto gpu = Runtime::Describe().at("gpu");
            require(Runtime::GetQualityTier() == quality && gpu.at("requested").get<bool>() == requested &&
                gpu.at("enabled").get<bool>() == enabled && Runtime::IsGpuParticlesEnabled() == enabled,
                "quality, explicit request and actual GPU policy match");
        };
        const auto cleanPolicyEffects = [&] {
            Runtime::StopAllEffects();
            for (uint32_t i = 0; i < runtimeDesc.FramesInFlight + 2; ++i) frame(update, false, false);
            retire();
            checkAllocations();
            const auto stats = Runtime::GetStats();
            require(!Runtime::HasActiveEffects() && stats.ActiveCpuInstances.Current == 0 &&
                stats.GpuAllocatedEmitters == 0 && stats.GpuReservedParticles == 0 && stats.GpuReservedTrails == 0,
                "quality probe effects and allocator ownership retire to clean baseline");
        };
        const auto provePolicyGpuWork = [&](const char* stage) {
            require(Runtime::PlayEffect(effect, glm::vec3(0.0f)) >= 0, "quality probe GPU effect starts");
            const auto before = Runtime::GetStats();
            frame([&] { for (int i = 0; i < 4; ++i) update(); }, true, false);
            retire();
            const auto after = Runtime::GetStats();
            require(after.TotalRecordedDispatchCount > before.TotalRecordedDispatchCount &&
                after.TotalRecordedDrawCount > before.TotalRecordedDrawCount &&
                after.GpuSimulationMs.Current > 0 && after.GpuRenderMs.Current > 0 &&
                after.LastGpuSimulationSubmission > before.LastGpuSimulationSubmission &&
                after.LastGpuRenderSubmission > before.LastGpuRenderSubmission,
                "quality selection produces fresh real dispatch, draws and completed positive GPU timers");
            evidence(stage);
            cleanPolicyEffects();
        };
        Runtime::SetQualityTier(Quality::High);
        requirePolicy(Quality::High, true, true);
        provePolicyGpuWork("policy-initial-high");
        Runtime::SetQualityTier(Quality::Low);
        requirePolicy(Quality::Low, true, false);
        Runtime::SetQualityTier(Quality::Medium);
        requirePolicy(Quality::Medium, true, true);
        provePolicyGpuWork("policy-low-to-medium");
        Runtime::SetQualityTier(Quality::High);
        requirePolicy(Quality::High, true, true);
        provePolicyGpuWork("policy-medium-to-high");

        // Capacity changes and disabling a live system must leave all policy
        // fields untouched, both with live effects and with an open host frame.
        require(Runtime::PlayEffect(effect, glm::vec3(0.0f)) >= 0, "active policy rejection fixture starts");
        Runtime::SetQualityTier(Quality::Medium);
        requirePolicy(Quality::High, true, true);
        Runtime::SetQualityTier(Quality::Low);
        requirePolicy(Quality::High, true, true);
        require(!Runtime::SetGpuParticlesEnabled(false), "active effect rejects explicit GPU disable");
        requirePolicy(Quality::High, true, true);
        frame(update, true, false);
        cleanPolicyEffects();
        frame([&] {
            Runtime::SetQualityTier(Quality::Medium);
            requirePolicy(Quality::High, true, true);
            Runtime::SetQualityTier(Quality::Low);
            requirePolicy(Quality::High, true, true);
            require(!Runtime::SetGpuParticlesEnabled(false), "open host frame rejects explicit GPU disable");
            requirePolicy(Quality::High, true, true);
        }, false, false);
        retire();

        require(Runtime::SetGpuParticlesEnabled(false), "quiescent explicit GPU disable accepted");
        for (const auto quality : {Quality::Low, Quality::Medium, Quality::High})
        {
            Runtime::SetQualityTier(quality);
            requirePolicy(quality, false, false);
        }
        require(Runtime::PlayEffect("Assets/Effects/AuthoringPathValidation/CpuSpriteControl.efkpkg",
            glm::vec3(0.0f)) >= 0, "disabled policy CPU fixture starts");
        require(!Runtime::SetGpuParticlesEnabled(true), "active CPU effect rejects GPU enable");
        requirePolicy(Quality::High, false, false);
        frame(update, true, false);
        cleanPolicyEffects();
        frame([&] {
            require(!Runtime::SetGpuParticlesEnabled(true), "open host frame rejects GPU enable");
            requirePolicy(Quality::High, false, false);
        }, false, false);
        retire();
        Runtime::SetQualityTier(Quality::Low);
        requirePolicy(Quality::Low, false, false);
        require(Runtime::SetGpuParticlesEnabled(true), "Low accepts explicit GPU enable request");
        requirePolicy(Quality::Low, true, false);
        Runtime::SetQualityTier(Quality::Medium);
        requirePolicy(Quality::Medium, true, true);
        provePolicyGpuWork("policy-low-request-to-medium");
        Runtime::SetQualityTier(Quality::High);
        requirePolicy(Quality::High, true, true);
        provePolicyGpuWork("policy-restored-high");
        std::cout << "EARLY_VFX_QUALITY_POLICY_PASS: high-low-medium-high,explicit-disable,low-request,active-and-host-rejection,real-gpu-work,clean-baseline\n";
        device.SetSubmissionTimingEnabled(false);
        if (bInjectUploadFailure)
            std::cout << "EARLY_VFX_UPLOAD_FAILURE_RECOVERY_PASS: exact-sentinel,abort,retired-timing,reinitialize,draw,allocator-retirement"
                << " REQUIRES_EXTERNAL_CHECK=zero-native-validation-errors\n";
        std::cout << "EARLY_VFX_SMOKE_PASS: backend=" << HEngine::ToString(device.GetBackend())
            << " coverage=multi-update,no-camera,no-draw,two-view,abort,reinitialize"
            << " NOT_VERIFIED=particle-state-readback,visual,resize,scene-switch,device-recreation\n";
    }
}

int main()
{
    HEngine::Log::Init();
    HEngine::ConfigManager::GetInstance().Init(
        HE_DEFAULT_PROJECT_DIR,
        HE_ENGINE_CONTENT_DIR,
        {},
        HE_ENGINE_GENERATED_CONTENT_DIR,
        HE_PROJECT_GAME_CONFIG);

    HEngine::ModuleManager& moduleManager =
        HEngine::ModuleManager::GetInstance();
    moduleManager.ConfigurePluginHost(HEngine::PluginHostEnvironment{
        .Host = HEngine::PluginHost::RenderSmoke,
        .BuildDistribution = HEngine::PluginBuildDistribution::Development,
        .Platform = HEngine::PluginPlatform::Windows,
#if HE_DEBUG
        .Configuration = HEngine::PluginConfiguration::Debug,
#else
        .Configuration = HEngine::PluginConfiguration::Release,
#endif
    });
    HEngine::RegisterStaticPluginsForRenderSmoke(
        moduleManager.GetPluginManager());
    moduleManager.StartPluginsThroughPhase(HEngine::PluginLoadPhase::PreDevice);

    HEngine::Scope<HEngine::Window> window;
    HEngine::RenderDevice renderDevice;
    int result = 1;
    try
    {
        window = HEngine::Window::Create(HEngine::WindowProps(
            "HEngine DX12 Scene Smoke",
            c_InitialWidth,
            c_InitialHeight,
            false,
            true,
            false,
            false,
            false));
        window->SetEventCallback([](HEngine::Event&) {});
        auto* renderSurface = dynamic_cast<HEngine::RenderSurface*>(window.get());
        if (!renderSurface)
            throw std::runtime_error("Scene smoke window does not provide a RenderSurface");

        const char* disableValidation = std::getenv(
            "HE_DX12_SCENE_SMOKE_DISABLE_VALIDATION");
        const bool bEnableValidation = disableValidation == nullptr ||
            std::string_view(disableValidation) != "1";
        const char* heapDirect = std::getenv("HE_DX12_SCENE_SMOKE_HEAP_DIRECT");
        const bool bHeapDirect = heapDirect != nullptr &&
            std::string_view(heapDirect) == "1";
        const char* earlyProbe = std::getenv("HE_VFX_EARLY_SMOKE");
        const bool bEarlyVfxProbe = earlyProbe && std::string_view(earlyProbe) == "1";
        const char* materialProbe = std::getenv("HE_MATERIAL_SAMPLING_SMOKE");
        const bool bMaterialSamplingProbe = materialProbe && std::string_view(materialProbe) == "1";
        const char* depthShadowProbe = std::getenv("HE_MATERIAL_DEPTH_SHADOW_SMOKE");
        const bool bDepthShadowProbe = depthShadowProbe && std::string_view(depthShadowProbe) == "1";
        if (int(bMaterialSamplingProbe) + int(bEarlyVfxProbe) + int(bDepthShadowProbe) > 1)
            throw std::invalid_argument("Material sampling, depth/shadow and early VFX smoke selectors conflict");
        auto backend = HEngine::RenderBackend::DX12;
        const auto parseBackend = [](const char* value, const char* name)
        {
            if (std::string_view(value) == "DX12") return HEngine::RenderBackend::DX12;
            if (std::string_view(value) == "Vulkan") return HEngine::RenderBackend::Vulkan;
            throw std::invalid_argument(std::string(name) + " must be DX12 or Vulkan");
        };
        const char* sceneBackend = std::getenv("HE_SCENE_SMOKE_BACKEND");
        if (sceneBackend) backend = parseBackend(sceneBackend, "HE_SCENE_SMOKE_BACKEND");
#if !HE_WITH_OFFLINE_REFERENCE_RENDERER_SMOKE
        if (bMaterialSamplingProbe)
            throw std::runtime_error("Material sampling smoke requires the linked offline-reference-renderer plugin");
        if (const char* pbr = std::getenv("HE_PBRTEST_PT_SMOKE"); pbr && std::string_view(pbr) == "1")
            throw std::runtime_error("PBR reference smoke requires the linked offline-reference-renderer plugin");
#endif
        if (bEarlyVfxProbe)
        {
            std::cout << std::unitbuf;
            const char* requestedBackend = std::getenv("HE_VFX_EARLY_SMOKE_BACKEND");
            if (requestedBackend)
            {
                const auto earlyBackend = parseBackend(requestedBackend, "HE_VFX_EARLY_SMOKE_BACKEND");
                if (sceneBackend && earlyBackend != backend)
                    throw std::invalid_argument("Scene and early VFX smoke backend selectors conflict");
                backend = earlyBackend;
            }
        }
        if (!bEnableValidation)
        {
            HE_CORE_WARN(
                "DX12 scene smoke validation layer explicitly disabled; runtime coverage remains active");
        }
        renderDevice.Initialize(
            backend,
            *renderSurface,
            bEnableValidation);
        if (renderDevice.GetBackend() != backend)
            throw std::runtime_error("Scene smoke backend differs from requested backend");
        moduleManager.StartPluginsThroughPhase(
            HEngine::PluginLoadPhase::PostDevice);
        moduleManager.StartPluginsThroughPhase(
            HEngine::PluginLoadPhase::PostEngine);
        const HEngine::EffekseerRuntimeDesc vfxRuntimeDesc{
            .Device = renderDevice.GetDevice(),
            .ColorFormat = nvrhi::Format::RGBA16_FLOAT,
            .DepthFormat = nvrhi::Format::D32,
            .FramesInFlight = renderDevice.GetCapabilities().FramesInFlight,
            .bReversedDepth = false,
        };
        HEngine::EffekseerRuntime::Initialize(vfxRuntimeDesc);
        if (bEarlyVfxProbe)
        {
            RunEarlyVfxSmoke(*window, renderDevice, vfxRuntimeDesc);
            renderDevice.WaitForIdle();
            HEngine::EffekseerRuntime::Shutdown();
            renderDevice.Shutdown();
            moduleManager.StopPlugins();
            window.reset();
            return 0;
        }
        if (bDepthShadowProbe)
        {
            RunMaterialDepthShadowSmoke(renderDevice);
            renderDevice.WaitForIdle();
            HEngine::EffekseerRuntime::Shutdown();
            renderDevice.Shutdown();
            moduleManager.StopPlugins();
            window.reset();
            return 0;
        }
#if HE_WITH_OFFLINE_REFERENCE_RENDERER_SMOKE
        if (bMaterialSamplingProbe)
        {
            RunMaterialSamplingSmoke(renderDevice);
            renderDevice.WaitForIdle();
            HEngine::EffekseerRuntime::Shutdown();
            renderDevice.Shutdown();
            moduleManager.StopPlugins();
            window.reset();
            return 0;
        }
        if (const char* pbr = std::getenv("HE_PBRTEST_PT_SMOKE"); pbr && std::string_view(pbr) == "1")
        {
            RunPbrTestPathTracingSmoke(renderDevice);
            renderDevice.WaitForIdle();
            HEngine::EffekseerRuntime::Shutdown();
            renderDevice.Shutdown();
            moduleManager.StopPlugins();
            window.reset();
            return 0;
        }
        if (const char* progressive = std::getenv("HE_OFFLINE_PROGRESSIVE_SMOKE"); progressive && std::string_view(progressive) == "1")
        {
            RunOfflineProgressiveSmoke(renderDevice);
            renderDevice.WaitForIdle();
            HEngine::EffekseerRuntime::Shutdown();
            renderDevice.Shutdown();
            moduleManager.StopPlugins();
            window.reset();
            return 0;
        }
#endif
        if (const char* sharedRt = std::getenv("HE_SHARED_RT_SCENE_SMOKE"); sharedRt && std::string_view(sharedRt) == "1")
        {
            RunHardwareRayTracingSmoke(*window, renderDevice, false);
            auto cornell = BuildImportedCornellWorld();
            RunImportedCornellSurfaceAovSmoke(renderDevice, cornell.World,
                HEngine::ConfigManager::GetEngineGeneratedContentFullPath("Shaders/CACHES/DXIL"));
            renderDevice.WaitForIdle();
            HEngine::EffekseerRuntime::Shutdown();
            renderDevice.Shutdown();
            moduleManager.StopPlugins();
            window.reset();
            return 0;
        }
        auto sceneRenderer = std::make_unique<HEngine::SceneRenderer>(HEngine::SceneRendererDesc{
            .Device = renderDevice.GetDevice(),
            .FramesInFlight = renderDevice.GetCapabilities().FramesInFlight,
            .ShaderCacheDirectory =
                HEngine::ConfigManager::GetEngineGeneratedContentFullPath("Shaders/CACHES/DXIL"),
            // Smoke fixtures are engine-owned; keep project overrides from changing
            // the deterministic PBR/CSM validation inputs.
            .AssetRootDirectory = HEngine::ConfigManager::GetInstance().GetEngineContentFolder(),
            .EngineAssetRootDirectory = HEngine::ConfigManager::GetInstance().GetEngineContentFolder(),
            .bHeapDirectlyIndexed = bHeapDirect,
            .bRunEmptyEffekseerPassForValidation = true,
        });
        HEngine::RenderWorld world = BuildSyntheticWorld();
        sceneRenderer->ConfigureOffscreenOutput(
            c_InitialWidth,
            c_InitialHeight,
            "Scene smoke editor viewport");
        const uint64_t initialOffscreenGeneration =
            sceneRenderer->GetOffscreenOutput().Generation;
        if (!sceneRenderer->IsEnvironmentReady({}))
            throw std::runtime_error("Empty environment must be immediately ready");
        const HEngine::SceneEnvironmentPreparation environment =
            sceneRenderer->PrepareEnvironment(world.Environment.HdrTexturePath);
        if (environment.State != HEngine::SceneEnvironmentState::Ready ||
            !sceneRenderer->IsEnvironmentReady(world.Environment.HdrTexturePath))
        {
            throw std::runtime_error(
                "Modern environment IBL preparation failed: " + environment.Error);
        }

        RenderFrames(
            *window,
            renderDevice,
            *sceneRenderer,
            world,
            c_FramesPerSize,
            HEngine::SceneRendererOutputMode::SwapChainAndOffscreen,
            HEngine::RuntimeUiRenderMode::Offscreen);
        renderDevice.WaitForIdle();
        sceneRenderer->Resize(c_ResizedWidth, c_ResizedHeight);
        sceneRenderer->ConfigureOffscreenOutput(
            c_ResizedWidth,
            c_ResizedHeight,
            "Scene smoke editor viewport");
        {
            const HEngine::SceneRendererOffscreenOutput resizedOffscreen =
                sceneRenderer->GetOffscreenOutput();
            if (!resizedOffscreen ||
                resizedOffscreen.Generation <= initialOffscreenGeneration ||
                resizedOffscreen.Width != c_ResizedWidth ||
                resizedOffscreen.Height != c_ResizedHeight)
            {
                throw std::runtime_error("SceneRenderer offscreen output did not resize safely");
            }
        }
        renderDevice.Resize(c_ResizedWidth, c_ResizedHeight);
        RenderFrames(
            *window,
            renderDevice,
            *sceneRenderer,
            world,
            c_FramesPerSize,
            HEngine::SceneRendererOutputMode::SwapChainAndOffscreen,
            HEngine::RuntimeUiRenderMode::Offscreen);

        renderDevice.WaitForIdle();
        sceneRenderer->CollectGpuCullingStats();
        const HEngine::SceneRendererStats& stats = sceneRenderer->GetStats();
        const std::array<std::string_view, 21> expectedPasses = {
            "SceneUpload", "DirectionalShadow", "GpuFrustumCulling",
            "GpuCullingReadback", "SceneDepthPrepass", "OpaqueForwardHDR",
            "SceneSky", "SkinnedForwardHDR", "Primitive2D",
            "TransparentForwardHDR", "EditorOutline", "VFXTransparent", "ToneMapping",
            "RuntimeUiBackdropCopy", "RuntimeUiRender", "UiTransition", "UiComposite",
            "OffscreenOutputCopy", "UI", "Capture", "Present"
        };
        std::cout
            << "DX12_SCENE_SMOKE_STATS: staticOpaque=" << stats.StaticOpaqueDrawCount
            << " cachedMeshes=" << stats.CachedStaticMeshCount
            << " uploadedMeshes=" << stats.UploadedMeshCount
            << " bindlessMaterials=" << stats.BindlessMaterialCount
            << " bindlessTextures=" << stats.BindlessTextureCount
            << " gpuMaterials=" << stats.GpuMaterialCount
            << " customMaterialFallback=" << stats.CustomMaterialCpuFallbackDrawCount
            << " materialPrograms=" << stats.MaterialProgramCount
            << " materialParameterRows=" << stats.MaterialParameterRowCount
            << " materialGeneration=" << stats.MaterialProgramGenerationMax
            << " transparent=" << stats.CpuTransparentDrawCount
            << " alphaMask=" << stats.AlphaMaskDrawCount
            << " depth=" << stats.DepthPrepassDrawCount
            << " depthIndirect=" << stats.DepthPrepassIndirectDrawCount
            << " depthSkinned=" << stats.DepthPrepassSkinnedDrawCount
            << " depthMask=" << stats.DepthPrepassAlphaMaskDrawCount
            << " skinned=" << stats.SkinnedDrawCount
            << " bones=" << stats.SkinnedBoneMatrixCount
            << " nonIdentityBones=" << stats.SkinnedNonIdentityBoneMatrixCount
            << " outline=" << stats.EditorOutlineDrawCount
            << " materialBindings=" << stats.MaterialBindingSetCreationCount
            << " primitives=" << stats.Primitive2DCount
            << " primitiveBatches=" << stats.Primitive2DBatchCount
            << " primitiveVertices=" << stats.Primitive2DVertexCount
            << " passes=" << stats.CompiledPasses.size() << '\n';
        std::cout << "DX12_SCENE_SMOKE_PASSES:";
        for (const std::string& passName : stats.CompiledPasses)
            std::cout << ' ' << passName;
        std::cout << '\n';
        if (stats.SceneColorFormat != nvrhi::Format::RGBA16_FLOAT ||
            stats.StaticOpaqueDrawCount != 5 || stats.CachedStaticMeshCount != 2 ||
            stats.UploadedMeshCount != 2 || !stats.bVfxPassAdded ||
            stats.bVfxDepthSnapshotAdded || stats.bVfxBackgroundSnapshotAdded ||
            !stats.bOverlayPassRecorded || !stats.bCapturePassRecorded ||
            !stats.bOffscreenOutputRecorded ||
            !stats.bEnvironmentReady ||
            stats.EnvironmentPath != world.Environment.HdrTexturePath ||
            stats.BindlessMaterialCount != 6 || stats.BindlessTextureCount != 6 ||
            stats.GpuMaterialCount != 6 ||
            stats.CustomMaterialCpuFallbackDrawCount != 5 ||
            stats.MaterialProgramCount != 3 || stats.MaterialParameterRowCount != 22 ||
            stats.MaterialProgramGenerationMax == 0 ||
            stats.CpuTransparentDrawCount != 1 || stats.AlphaMaskDrawCount != 1 ||
            stats.DepthPrepassDrawCount != 6 ||
            stats.DepthPrepassIndirectDrawCount != 1 ||
            stats.DepthPrepassSkinnedDrawCount != 1 ||
            stats.DepthPrepassAlphaMaskDrawCount != 1 ||
            stats.SkinnedDrawCount != 1 || stats.SkinnedBoneMatrixCount != 100 ||
            stats.SkinnedNonIdentityBoneMatrixCount != 1 ||
            stats.EditorOutlineDrawCount != 2 ||
            stats.MaterialBindingSetCreationCount != 0 ||
            stats.Primitive2DCount != 3 || stats.Primitive2DBatchCount != 2 ||
            stats.Primitive2DVertexCount != 18 ||
            stats.CompiledPasses.size() != expectedPasses.size())
        {
            throw std::runtime_error("SceneRenderer reported unexpected HDR graph or mesh stats");
        }
        for (size_t index = 0; index < expectedPasses.size(); ++index)
        {
            if (stats.CompiledPasses[index] != expectedPasses[index])
                throw std::runtime_error("SceneRenderer compiled the passes in an unexpected order");
        }

        if (stats.GpuDrivenObjectCount != 3 || stats.GpuDrivenBatchCount != 1 ||
            stats.GpuVisibleObjectCount != 2 || stats.GpuCulledObjectCount != 1 ||
            stats.GpuIndirectDrawCount != 1 || stats.GpuComputeDispatchCount != 1 ||
            stats.GpuInstancedBatchCount != 1 ||
            stats.GpuMaxVisibleInstancesPerBatch != 2 ||
            stats.CpuFallbackDrawCount != 4 ||
            stats.DirectionalShadowCascadeCount != 4 ||
            stats.DirectionalShadowDrawCount != 32)
        {
            throw std::runtime_error(
                "SceneRenderer GPU culling, indirect draw, or CPU fallback stats are invalid");
        }

        // The original direct path remains available and does not allocate the
        // high-quality runtime-UI intermediates.
        RenderFrames(
            *window,
            renderDevice,
            *sceneRenderer,
            world,
            1,
            HEngine::SceneRendererOutputMode::SwapChainAndOffscreen,
            HEngine::RuntimeUiRenderMode::Direct);
        renderDevice.WaitForIdle();
        const std::vector<std::string> directPasses =
            sceneRenderer->GetStats().CompiledPasses;
        const std::array<std::string_view, 18> expectedDirectPasses = {
            "SceneUpload", "DirectionalShadow", "GpuFrustumCulling",
            "GpuCullingReadback", "SceneDepthPrepass", "OpaqueForwardHDR",
            "SceneSky", "SkinnedForwardHDR", "Primitive2D",
            "TransparentForwardHDR", "EditorOutline", "VFXTransparent", "ToneMapping",
            "SceneUI", "OffscreenOutputCopy", "UI", "Capture", "Present"
        };
        if (directPasses.size() != expectedDirectPasses.size())
            throw std::runtime_error("SceneRenderer Direct graph has an unexpected pass count");
        for (size_t index = 0; index < expectedDirectPasses.size(); ++index)
        {
            if (directPasses[index] != expectedDirectPasses[index])
                throw std::runtime_error("SceneRenderer Direct graph has an unexpected pass order");
        }

        // The same renderer can switch to a dedicated preview graph without a
        // backbuffer; color/depth remain typed ShaderResource outputs.
        RenderFrames(
            *window,
            renderDevice,
            *sceneRenderer,
            world,
            1,
            HEngine::SceneRendererOutputMode::OffscreenOnly,
            HEngine::RuntimeUiRenderMode::Offscreen);
        renderDevice.WaitForIdle();
        const std::vector<std::string> offscreenPasses =
            sceneRenderer->GetStats().CompiledPasses;
        const std::array<std::string_view, 14> expectedOffscreenOnlyPasses = {
            "SceneUpload", "DirectionalShadow", "GpuFrustumCulling",
            "GpuCullingReadback", "SceneDepthPrepass", "OpaqueForwardHDR",
            "SceneSky", "SkinnedForwardHDR", "Primitive2D",
            "TransparentForwardHDR", "EditorOutline", "VFXTransparent", "ToneMapping",
            "Capture"
        };
        if (offscreenPasses.size() != expectedOffscreenOnlyPasses.size())
            throw std::runtime_error("SceneRenderer OffscreenOnly graph has an unexpected pass count");
        for (size_t index = 0; index < expectedOffscreenOnlyPasses.size(); ++index)
        {
            if (offscreenPasses[index] != expectedOffscreenOnlyPasses[index])
            {
                throw std::runtime_error(
                    "SceneRenderer OffscreenOnly graph has unexpected UI or swap-chain passes");
            }
        }

        const std::filesystem::path captureDirectory =
            std::filesystem::path(HE_DEFAULT_PROJECT_DIR).parent_path() /
            "out/agent/captures";
        std::filesystem::create_directories(captureDirectory);
        const std::filesystem::path materialCapturePath =
            captureDirectory / (bHeapDirect
                ? "material-dx12-scene-smoke-heap-direct.png"
                : "material-dx12-scene-smoke-baseline.png");
        HEngine::TextureReadbackQueue materialReadback(renderDevice.GetDevice());
        HEngine::TextureReadbackHandle materialCapture;
        RenderFrames(
            *window,
            renderDevice,
            *sceneRenderer,
            world,
            1,
            HEngine::SceneRendererOutputMode::OffscreenOnly,
            HEngine::RuntimeUiRenderMode::Offscreen,
            &materialReadback,
            &materialCapture,
            materialCapturePath);
        renderDevice.WaitForIdle();
        materialReadback.Collect(std::numeric_limits<uint64_t>::max());
        for (uint32_t attempt = 0;
            attempt < 500 &&
                materialReadback.GetResult(materialCapture).Status ==
                    HEngine::TextureReadbackStatus::InFlight;
            ++attempt)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            materialReadback.Collect(std::numeric_limits<uint64_t>::max());
        }
        const HEngine::TextureReadbackResult materialCaptureResult =
            materialReadback.GetResult(materialCapture);
        if (materialCaptureResult.Status != HEngine::TextureReadbackStatus::Complete ||
            !std::filesystem::is_regular_file(materialCapturePath) ||
            std::filesystem::file_size(materialCapturePath) < 1024)
        {
            throw std::runtime_error(
                "Custom material DX12 capture did not produce a valid PNG: " +
                materialCaptureResult.Error);
        }
        if (materialReadback.GetPendingCount() != 0)
            throw std::runtime_error("Completed texture readback remained pending");
        materialReadback.Collect(std::numeric_limits<uint64_t>::max());
        if (materialReadback.GetPendingCount() != 0)
            throw std::runtime_error("Repeated texture readback collection corrupted the pending count");
        std::cout << "DX12_SCENE_SMOKE_CAPTURE: "
                  << materialCapturePath.generic_string() << '\n';
        materialReadback.Release(materialCapture);

        const char* screenSpaceGiSmoke = std::getenv("HE_SSGI_SMOKE");
        if (screenSpaceGiSmoke != nullptr &&
            std::string_view(screenSpaceGiSmoke) == "1")
        {
            RunScreenSpaceGlobalIlluminationSmoke(renderDevice);
        }

        const char* cornellAaSmoke = std::getenv("HE_CORNELL_AA_SMOKE");
        if (cornellAaSmoke != nullptr &&
            std::string_view(cornellAaSmoke) == "1")
        {
            RunCornellAntiAliasingSmoke(renderDevice);
        }

        const char* surfaceDataAovSmoke =
            std::getenv("HE_SURFACE_DATA_AOV_SMOKE");
        if (surfaceDataAovSmoke != nullptr &&
            std::string_view(surfaceDataAovSmoke) == "1")
        {
            ImportedCornellWorld importedCornell = BuildImportedCornellWorld();
            const std::filesystem::path dxilDirectory =
                HEngine::ConfigManager::GetEngineGeneratedContentFullPath(
                    "Shaders/CACHES/DXIL");
            RunImportedCornellSurfaceAovSmoke(
                renderDevice,
                importedCornell.World,
                dxilDirectory);
        }

        const char* hardwareRtSmoke = std::getenv("HE_HARDWARE_RT_SMOKE");
        if (hardwareRtSmoke != nullptr && std::string_view(hardwareRtSmoke) == "1")
            RunHardwareRayTracingSmoke(*window, renderDevice);

        renderDevice.WaitForIdle();
        sceneRenderer.reset();
        HEngine::EffekseerRuntime::Shutdown();
        renderDevice.Shutdown();
        std::cout << "DX12_SCENE_SMOKE_PASS: HDR environment -> NVRHI compute cubemap/irradiance/prefilter/BRDF -> PBR IBL, four-cascade D32-array directional-shadow submission including bindless MASK alpha clip and double-sided/static/GPU/skinned casters, GPU frustum culling compacted 2/3 same-batch instances (plus one MASK object) into indexed indirect draws + CpuOpaque fallback, independent SceneDepthPrepass (GPU indirect + CPU + skinned + bindless MASK alpha clip) -> depth-equal opaque/skinned forward, sorted BLEND transparent pass, immutable non-identity bone palette -> skinned vertex shader, selected static mesh -> editor silhouette pass, real bindless quad/circle/line Primitive2D HDR pass, initialized empty Effekseer VFXTransparent lifecycle (not visual VFX coverage), RGBA16F -> ACES/sRGB -> RuntimeUiBackdropCopy -> RGBA8+D24S8 RuntimeUiRender -> UiTransition -> UiComposite -> typed offscreen color/depth -> window UI -> Capture -> Present, Direct and OffscreenOnly graphs, resize, validation, shutdown\n";
        result = 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "DX12_SCENE_SMOKE_FAIL: " << exception.what() << '\n';
        if (renderDevice.IsInitialized())
        {
            try
            {
                renderDevice.WaitForIdle();
                HEngine::EffekseerRuntime::Shutdown();
                renderDevice.Shutdown();
            }
            catch (const std::exception& shutdownException)
            {
                std::cerr << "DX12_SCENE_SMOKE_SHUTDOWN_FAIL: " << shutdownException.what() << '\n';
            }
        }
    }
    try
    {
        moduleManager.StopPlugins();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "DX12_SCENE_SMOKE_PLUGIN_SHUTDOWN_FAIL: "
                  << exception.what() << '\n';
        result = 1;
    }
    window.reset();
    return result;
}
