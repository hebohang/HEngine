#include "Hepch.h"

#include "Runtime/UnitTest/CornellSceneSerializationTests.h"

#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#include "Runtime/EcsFramework/Component/Light/EnvironmentLightComponent.h"
#include "Runtime/EcsFramework/Component/Mesh/MeshComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Level/Level.h"
#include "Runtime/EcsFramework/Serializer/SceneSerializer.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"

#include <HEngine/ProjectBuildConfig.h>

#include <cmath>
#include <filesystem>
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

        [[nodiscard]] bool Near(float left, float right, float epsilon = 1e-6f)
        {
            return std::abs(left - right) <= epsilon;
        }
    }

    void CornellSceneSerializationContract()
    {
        struct ConfigCleanup final
        {
            ~ConfigCleanup()
            {
                ConfigManager::GetInstance().Clear();
            }
        } cleanup;
        ConfigManager::GetInstance().Clear();
        ConfigManager::GetInstance().Init(
            HE_DEFAULT_PROJECT_DIR,
            HE_ENGINE_CONTENT_DIR,
            {},
            HE_ENGINE_GENERATED_CONTENT_DIR,
            HE_PROJECT_GAME_CONFIG);
        const std::filesystem::path scenePath =
            std::filesystem::path(HE_DEFAULT_PROJECT_DIR) /
            "Assets/CornellBox/CornellBox.he";
        Require(std::filesystem::is_regular_file(scenePath),
            "Generated Cornell HEngine scene is missing.");

        Ref<Level> level = CreateRef<Level>();
        SceneSerializer serializer(level);
        Require(serializer.Deserialize(scenePath.string()),
            "Reflected SceneSerializer rejected the generated Cornell scene.");

        size_t entityCount = 0;
        level->mRegistry.each([&](auto) { ++entityCount; });
        Require(entityCount == 3,
            "Cornell scene did not materialize exactly mesh, camera, and exposure entities.");

        const auto meshView = level->mRegistry.view<MeshComponent, TransformComponent>();
        Require(meshView.size_hint() == 1,
            "Cornell scene did not contain exactly one mesh entity.");
        for (auto entity : meshView)
        {
            MeshComponent& mesh = meshView.get<MeshComponent>(entity);
            const TransformComponent& transform = meshView.get<TransformComponent>(entity);
            Require(mesh.Path == "Assets/CornellBox/CornellBox.hmesh" &&
                    mesh.meshResPath == "Assets/CornellBox/CornellBox.hmesh",
                "Cornell scene mesh path does not reference the deterministic imported asset.");
            Require(transform.Translation == glm::vec3(0.0f) &&
                    transform.Rotation == glm::vec3(0.0f) &&
                    transform.Scale == glm::vec3(1.0f),
                "Cornell scene mesh transform is not identity.");
			Require(mesh.mMesh && !mesh.mMesh->mMaterial.empty(),
				"Cornell mesh has no authored material slot for override validation.");
			const Ref<Material> authoredMaterial = mesh.mMesh->mMaterial.front();
            mesh.MaterialOverrides = {
                "Assets/Models/MaterialPreview/shaderball.hmat" };
			mesh.ApplyMaterialOverrides();
			Require(mesh.mMesh->mMaterial.front() &&
				mesh.mMesh->mMaterial.front() != authoredMaterial &&
				mesh.mMesh->mMaterial.front()->mMatResPath ==
					"Assets/Models/MaterialPreview/shaderball.hmat",
				"MeshComponent did not apply its reflected material slot override.");
			mesh.MaterialOverrides = { "" };
			mesh.ApplyMaterialOverrides();
			Require(mesh.mMesh->mMaterial.front() == authoredMaterial,
				"Clearing a material override did not restore the authored mesh slot.");
			mesh.MaterialOverrides = {
				"Assets/Models/MaterialPreview/shaderball.hmat" };
        }

        const auto cameraView = level->mRegistry.view<CameraComponent, TransformComponent>();
        Require(cameraView.size_hint() == 1,
            "Cornell scene did not contain exactly one camera.");
        for (auto entity : cameraView)
        {
            const CameraComponent& camera = cameraView.get<CameraComponent>(entity);
            const TransformComponent& transform = cameraView.get<TransformComponent>(entity);
            if (!(camera.Primary && camera.FixedAspectRatio &&
                    camera.Camera.GetProjectionType() ==
                        SceneCamera::ProjectionType::Perspective &&
                    Near(camera.Camera.GetPerspectiveVerticalFOV(), 0.686047881f) &&
                    Near(camera.Camera.mAspectRatio, 1.0f)))
            {
                throw std::runtime_error(fmt::format(
                    "Cornell scene camera mismatch: primary={} fixed={} type={} fov={} aspect={}",
                    camera.Primary,
                    camera.FixedAspectRatio,
                    static_cast<int>(camera.Camera.GetProjectionType()),
                    camera.Camera.GetPerspectiveVerticalFOV(),
                    camera.Camera.mAspectRatio));
            }
            Require(Near(transform.Translation.x, 0.278f) &&
                    Near(transform.Translation.y, 0.273f) &&
                    Near(transform.Translation.z, -0.8f) &&
                    Near(transform.Rotation.y, 3.14159265f),
                "Cornell scene camera transform does not match the measured camera.");
        }

        const auto environmentView = level->mRegistry.view<EnvironmentLightComponent>();
        size_t environmentCount = 0;
        for ([[maybe_unused]] auto entity : environmentView)
            ++environmentCount;
        Require(environmentCount == 1,
            "Cornell scene did not contain exactly one exposure contract.");
        for (auto entity : environmentView)
        {
            const EnvironmentLightComponent& environment =
                environmentView.get<EnvironmentLightComponent>(entity);
            Require(environment.HdrTexturePath.empty() &&
                    Near(environment.Exposure, 1.0f) &&
                    Near(environment.IBLIntensity, 0.0f),
                "Cornell scene enabled IBL, auto exposure, or a non-unit exposure.");
        }

        const Json roundTrip = serializer.SerializeToJson();
        Require(roundTrip.size() == 3 &&
                roundTrip.contains("2db5c629-8172-54f8-8b8c-955c6ef07c7a") &&
                roundTrip.contains("576c2ac0-bf6f-59c5-aa98-e11313004508"),
            "Reflected Cornell scene round-trip lost stable entities.");
        bool bSerializedMaterialOverride = false;
        for (const auto& [uuid, document] : roundTrip.items())
        {
            (void)uuid;
            const auto meshComponent = document.find("MeshComponent");
            if (meshComponent == document.end())
                continue;
            const auto overrides = meshComponent->find("MaterialOverrides");
            bSerializedMaterialOverride = overrides != meshComponent->end() &&
                overrides->is_array() && overrides->size() == 1 &&
                (*overrides)[0] == "Assets/Models/MaterialPreview/shaderball.hmat";
        }
        Require(bSerializedMaterialOverride,
            "Reflected SceneSerializer dropped MeshComponent material overrides.");
    }
}
