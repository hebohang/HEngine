#include "Hepch.h"

#include "Runtime/EcsFramework/System/Camera/CameraDebugDrawSystem.h"

#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#include "Runtime/EcsFramework/Component/Camera/SpringArmComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/ModeManager/ModeManager.h"
#include "Runtime/Graphics/Renderer2D.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

namespace HEngine
{
#if WITH_EDITOR
    namespace
    {
        void DrawFrustumFromCorners(const glm::vec3 corners[8], const glm::vec4& color)
        {
            constexpr int edges[12][2] =
            {
                {0, 1}, {1, 2}, {2, 3}, {3, 0},
                {4, 5}, {5, 6}, {6, 7}, {7, 4},
                {0, 4}, {1, 5}, {2, 6}, {3, 7}
            };

            for (const auto& edge : edges)
                Renderer2D::DrawLine(corners[edge[0]], corners[edge[1]], color);
        }

		void DrawSelectedCameraFrustum(Level* level, Entity cameraEntity, const CameraComponent& cameraComp, const glm::vec4& color)
		{
			const glm::mat4 cameraWorld = level->GetWorldTransform(cameraEntity);
			const glm::mat4 projection = cameraComp.Camera.GetProjection();
			const glm::mat4 invViewProjection = glm::inverse(projection * glm::inverse(cameraWorld));

            const glm::vec3 ndc[8] =
            {
                {-1.0f, -1.0f, -1.0f},
                { 1.0f, -1.0f, -1.0f},
                { 1.0f,  1.0f, -1.0f},
                {-1.0f,  1.0f, -1.0f},
                {-1.0f, -1.0f,  1.0f},
                { 1.0f, -1.0f,  1.0f},
                { 1.0f,  1.0f,  1.0f},
                {-1.0f,  1.0f,  1.0f}
            };

            glm::vec3 worldCorners[8];
            for (int i = 0; i < 8; ++i)
            {
                const glm::vec4 corner = invViewProjection * glm::vec4(ndc[i], 1.0f);
                if (std::abs(corner.w) > 0.00001f)
                    worldCorners[i] = glm::vec3(corner) / corner.w;
                else
                    worldCorners[i] = glm::vec3(corner);
            }

            DrawFrustumFromCorners(worldCorners, color);
        }

		void DrawSelectedCameraSpringArm(Level* level, Entity cameraEntity, const SpringArmComponent& springArm, const glm::vec4& color)
		{
			const glm::vec3 cameraWorldTranslation = level->GetWorldTranslation(cameraEntity);
			const glm::vec3 cameraWorldRotation = level->GetWorldRotation(cameraEntity);
			const glm::mat3 rotation = glm::mat3(glm::toMat4(glm::quat(cameraWorldRotation)));
			const glm::vec3 forward = glm::normalize(rotation * glm::vec3(0.0f, 0.0f, -1.0f));

            const float currentDistance = springArm.RuntimeCurrentDistance > 0.0f
                ? springArm.RuntimeCurrentDistance
                : std::max(0.1f, springArm.ArmLength);

			const glm::vec3 pivot = cameraWorldTranslation + forward * currentDistance - rotation * springArm.SocketOffset;
			const glm::vec3 socket = pivot + rotation * springArm.SocketOffset;

			Renderer2D::DrawLine(pivot, socket, color);
			Renderer2D::DrawLine(socket, cameraWorldTranslation, color);

            const float axisLength = 0.2f;
            Renderer2D::DrawLine(pivot - glm::vec3(axisLength, 0.0f, 0.0f), pivot + glm::vec3(axisLength, 0.0f, 0.0f), color);
            Renderer2D::DrawLine(pivot - glm::vec3(0.0f, axisLength, 0.0f), pivot + glm::vec3(0.0f, axisLength, 0.0f), color);
            Renderer2D::DrawLine(pivot - glm::vec3(0.0f, 0.0f, axisLength), pivot + glm::vec3(0.0f, 0.0f, axisLength), color);
        }
    }

    void CameraDebugDrawSystem::OnUpdateEditor(Timestep ts, EditorCamera& camera)
    {
        if (!ModeManager::b3DMode)
            return;

        if (ConfigManager::selectedEntity < 0)
            return;

        Entity selectedEntity = { static_cast<EntityHandle>(ConfigManager::selectedEntity), mLevel };
        if (!selectedEntity || !selectedEntity.HasComponent<TransformComponent>() || !selectedEntity.HasComponent<CameraComponent>())
            return;

		auto& cameraComp = selectedEntity.GetComponent<CameraComponent>();

		Renderer2D::BeginScene(camera);
		DrawSelectedCameraFrustum(mLevel, selectedEntity, cameraComp, glm::vec4(0.95f, 0.80f, 0.25f, 1.0f));

		if (selectedEntity.HasComponent<SpringArmComponent>())
			DrawSelectedCameraSpringArm(mLevel, selectedEntity, selectedEntity.GetComponent<SpringArmComponent>(), glm::vec4(0.35f, 0.85f, 1.0f, 1.0f));

        Renderer2D::EndScene();
    }
#endif
}
