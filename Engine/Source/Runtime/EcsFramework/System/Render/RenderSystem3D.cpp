#include "hepch.h"

#include "Runtime/EcsFramework/System/Render/RenderSystem3D.h"
#include "Runtime/EcsFramework/Component/ComponentGroup.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/Renderer/Renderer3D.h"
#include "Runtime/Renderer/RenderCommand.h"
#include "Runtime/Renderer/Framebuffer.h"
#include "Runtime/Renderer/UniformBuffer.h"
#include "Runtime/Library/ShaderLibrary.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"

namespace HEngine
{
	namespace Utils
	{
		static std::vector<glm::vec4> getFrustumCornersWorldSpace(const glm::mat4& projview)
		{
			const auto inv = glm::inverse(projview);

			std::vector<glm::vec4> frustumCorners;
			for (unsigned int x = 0; x < 2; ++x)
			{
				for (unsigned int y = 0; y < 2; ++y)
				{
					for (unsigned int z = 0; z < 2; ++z)
					{
						const glm::vec4 pt = inv * glm::vec4(2.0f * x - 1.0f, 2.0f * y - 1.0f, 2.0f * z - 1.0f, 1.0f);
						frustumCorners.push_back(pt / pt.w);
					}
				}
			}

			return frustumCorners;
		}

		static glm::mat4 getLightSpaceMatrix(const float nearPlane, const float farPlane, float cameraFOV, float aspect, const glm::vec3& lightDir, const glm::mat4& view)
		{
			const auto proj = glm::perspective(
				glm::radians(cameraFOV), aspect, nearPlane,
				farPlane);
			const auto corners = getFrustumCornersWorldSpace(proj * view);

			glm::vec3 center = glm::vec3(0, 0, 0);
			for (const auto& v : corners)
			{
				center += glm::vec3(v);
			}
			center /= corners.size();

			const auto lightView = glm::lookAt(center + lightDir, center, glm::vec3(0.0f, 1.0f, 0.0f));

			float minX = std::numeric_limits<float>::max();
			float maxX = std::numeric_limits<float>::min();
			float minY = std::numeric_limits<float>::max();
			float maxY = std::numeric_limits<float>::min();
			float minZ = std::numeric_limits<float>::max();
			float maxZ = std::numeric_limits<float>::min();
			for (const auto& v : corners)
			{
				const auto trf = lightView * v;
				minX = std::min(minX, trf.x);
				maxX = std::max(maxX, trf.x);
				minY = std::min(minY, trf.y);
				maxY = std::max(maxY, trf.y);
				minZ = std::min(minZ, trf.z);
				maxZ = std::max(maxZ, trf.z);
			}

			// Tune this parameter according to the scene
			constexpr float zMult = 10.0f;
			if (minZ < 0)
			{
				minZ *= zMult;
			}
			else
			{
				minZ /= zMult;
			}
			if (maxZ < 0)
			{
				maxZ /= zMult;
			}
			else
			{
				maxZ *= zMult;
			}

			const glm::mat4 lightProjection = glm::ortho(minX, maxX, minY, maxY, minZ, maxZ);

			return lightProjection * lightView;
		}

		static std::vector<glm::mat4> getLightSpaceMatrices(float cameraNearPlane, float cameraFarPlane, float cameraFOV, float aspect, const glm::vec3& lightDir, const glm::mat4& projview, const std::vector<float>& shadowCascadeLevels)
		{
			std::vector<glm::mat4> ret;
			for (size_t i = 0; i < shadowCascadeLevels.size() + 1; ++i)
			{
				if (i == 0)
				{
					ret.push_back(getLightSpaceMatrix(cameraNearPlane, shadowCascadeLevels[i], cameraFOV, aspect, lightDir, projview));
				}
				else if (i < shadowCascadeLevels.size())
				{
					ret.push_back(getLightSpaceMatrix(shadowCascadeLevels[i - 1], shadowCascadeLevels[i], cameraFOV, aspect, lightDir, projview));
				}
				else
				{
					ret.push_back(getLightSpaceMatrix(shadowCascadeLevels[i - 1], cameraFarPlane, cameraFOV, aspect, lightDir, projview));
				}
			}
			return ret;
		}
	}

    void RenderSystem3D::OnUpdateRuntime(Timestep ts)
    {
		Camera* mainCamera = nullptr;
		glm::mat4 cameraTransform;
		glm::vec3 cameraPos;
		{
			auto view = mLevel->mRegistry.view<TransformComponent, CameraComponent>();
			for (auto entity : view)
			{
				auto [transform, camera] = view.get<TransformComponent, CameraComponent>(entity);

				if (camera.Primary)
				{
					mainCamera = &camera.Camera;
					cameraTransform = transform.GetTransform();
					cameraPos = transform.GetTranslation();
					break;
				}
			}
		}

		if (mainCamera)
		{
			Renderer3D::BeginScene(*mainCamera, cameraTransform);

			// Get the Light 
			{

			}

			// Draw model
			{
				auto view = mLevel->mRegistry.view<TransformComponent, MeshComponent>();
				for (auto entity : view)
				{
					auto [transform, mesh] = view.get<TransformComponent, MeshComponent>(entity);

					Renderer3D::DrawModel(transform.GetTransform(), cameraPos, mesh, (int)entity);
				}
			}

			Renderer3D::EndScene();
		}
    }

	void RenderSystem3D::OnUpdateEditor(Timestep ts, EditorCamera& camera)
	{
		Renderer3D::BeginScene(camera);

		// Point Light 
		{
			auto view = mLevel->mRegistry.view<TransformComponent, PointLightComponent>();
			int i = 0;
			for (auto entity : view)
			{
				auto [transform, light] = view.get<TransformComponent, PointLightComponent>(entity);

				glm::vec3 lightPos = transform.GetTranslation();
				glm::vec3 lightColor = light.LightColor;

				Ref<Shader> iblPbrStatic = Library<Shader>::GetInstance().Get("IBL_pbr_static");
				Ref<Shader> iblPbrAnim   = Library<Shader>::GetInstance().Get("IBL_pbr_anim");
				Ref<Shader> basePbrStatic = Library<Shader>::GetInstance().GetDefaultShader();
				Ref<Shader> basePbrAnim = Library<Shader>::GetInstance().Get("BasePBR_anim");

				iblPbrStatic->Bind();
				iblPbrStatic->SetFloat3("lightPositions[" + std::to_string(i) + "]", lightPos);
				iblPbrStatic->SetFloat3("lightColors[" + std::to_string(i) + "]", lightColor);				
				
				iblPbrAnim->Bind();
				iblPbrAnim->SetFloat3("lightPositions[" + std::to_string(i) + "]", lightPos);
				iblPbrAnim->SetFloat3("lightColors[" + std::to_string(i) + "]", lightColor);
				
				basePbrStatic->Bind();
				basePbrStatic->SetFloat3("u_Uniform.lightPositions[" + std::to_string(i) + "]", lightPos);
				basePbrStatic->SetFloat3("u_Uniform.lightColors[" + std::to_string(i) + "]", lightColor);				
				
				basePbrAnim->Bind();
				basePbrAnim->SetFloat3("u_Uniform.lightPositions[" + std::to_string(i) + "]", lightPos);
				basePbrAnim->SetFloat3("u_Uniform.lightColors[" + std::to_string(i) + "]", lightColor);

				i++;
			}
			if (i == 0)
			{
				Ref<Shader> iblPbrStatic = Library<Shader>::GetInstance().Get("IBL_pbr_static");
				Ref<Shader> iblPbrAnim = Library<Shader>::GetInstance().Get("IBL_pbr_anim");
				Ref<Shader> basePbrStatic = Library<Shader>::GetInstance().GetDefaultShader();
				Ref<Shader> basePbrAnim = Library<Shader>::GetInstance().Get("BasePBR_anim");

				for (size_t i = 0; i < 4; i++)
				{
					iblPbrStatic->Bind();
					iblPbrStatic->SetFloat3("lightColors[" + std::to_string(i) + "]", glm::vec3{ -1.0f } );		
					
					iblPbrAnim->Bind();
					iblPbrAnim->SetFloat3("lightColors[" + std::to_string(i) + "]", glm::vec3{ -1.0f } );

					basePbrStatic->Bind();
					basePbrStatic->SetFloat3("u_Uniform.lightColors[" + std::to_string(i) + "]", glm::vec3{ -1.0f });		
					
					basePbrAnim->Bind();
					basePbrAnim->SetFloat3("u_Uniform.lightColors[" + std::to_string(i) + "]", glm::vec3{ -1.0f });
				}
			}
		}

		// Directional light depth pass
		{
			auto view = mLevel->mRegistry.view<TransformComponent, DirectionalLightComponent>();
			for (auto e : view)
			{
				Entity entity = { e, mLevel };
				auto& transform = entity.GetComponent<TransformComponent>();
				auto& directionalLight = entity.GetComponent<DirectionalLightComponent>();

				float cameraNearPlane = camera.GetNearPlane();
				float cameraFarPlane = camera.GetFarPlane();
				std::vector<float> shadowCascadeLevels{ cameraFarPlane / 50.0f, cameraFarPlane / 25.0f, cameraFarPlane / 10.0f, cameraFarPlane / 2.0f };

				const auto lightMatrices = Utils::getLightSpaceMatrices(cameraNearPlane, cameraFarPlane, camera.GetFOV(),
					camera.GetAspect(), glm::normalize(directionalLight.LightDir), camera.GetViewMatrix(), shadowCascadeLevels);
				Ref<UniformBuffer> lightMatricesUBO = Library<UniformBuffer>::GetInstance().Get("LightMatricesUniform");
				for (size_t i = 0; i < lightMatrices.size(); i++)
				{
					lightMatricesUBO->SetData(&lightMatrices[i], sizeof(glm::mat4x4), i * sizeof(glm::mat4x4));
				}

				break; // now we only support one directional light
			}
		}

		uint32_t mainFramebuffer = RenderCommand::GetDrawFrameBuffer();

		// Light Depth pass
		Renderer3D::lightFBO->Bind();
		RenderCommand::Clear();
		RenderCommand::CullFrontOrBack(true); // peter panning
		auto view = mLevel->mRegistry.view<TransformComponent, MeshComponent>();
		for (auto e : view)
		{
			Entity entity = { e, mLevel };
			auto& transform = entity.GetComponent<TransformComponent>();
			auto& mesh = entity.GetComponent<MeshComponent>();

			mesh.mMesh->Draw(transform.GetTransform(), camera.GetPosition(), Library<Shader>::GetInstance().Get("CSM_Depth"), (int)e);
		}
		RenderCommand::CullFrontOrBack(false);

		// Render pass
		RenderCommand::BindFrameBuffer(mainFramebuffer);
		for (auto e : view)
		{
			Entity entity = { e, mLevel };
			auto& transform = entity.GetComponent<TransformComponent>();
			auto& mesh = entity.GetComponent<MeshComponent>();
			
			if ((int)e == ConfigManager::selectedEntity)
			{
				RenderCommand::SetStencilFunc(StencilFunc::ALWAYS, 1, 0xFF);
				RenderCommand::StencilMask(0xFF);
				Renderer3D::DrawModel(transform.GetTransform(), camera.GetPosition(), mesh, (int)e);

				RenderCommand::SetStencilFunc(StencilFunc::NOTEQUAL, 1, 0xFF);
				RenderCommand::StencilMask(0x00);
				if (mesh.mMesh->bPlayAnim)
					mesh.mMesh->Draw(transform.GetTransform(), camera.GetPosition(), Library<Shader>::GetInstance().Get("NormalOutline_anim"), (int)e);
				else
					mesh.mMesh->Draw(transform.GetTransform(), camera.GetPosition(), Library<Shader>::GetInstance().Get("NormalOutline"), (int)e);
				RenderCommand::StencilMask(0xFF);
				RenderCommand::SetStencilFunc(StencilFunc::ALWAYS, 0, 0xFF);
				RenderCommand::ClearStencil();
			}
			else
				Renderer3D::DrawModel(transform.GetTransform(), camera.GetPosition(), mesh, (int)e);
		}

		Renderer3D::EndScene();
	}
}