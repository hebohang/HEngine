#include "hepch.h"

#include "Runtime/Resource/ModeManager/ModeManager.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/EcsFramework/System/Render/EnvironmentSystem.h"
#include "Runtime/EcsFramework/Component/ComponentGroup.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/Renderer/RenderCommand.h"
#include "Runtime/Library/UniformBufferLibrary.h"
#include "Runtime/Library/ShaderLibrary.h"
#include "Runtime/Renderer/Renderer3D.h"
#include "Runtime/Renderer/Framebuffer.h"

#include "Editor/Panels/SceneSettingsPanel.h"

#include <glm/glm.hpp>

#include <glad/glad.h>

namespace HEngine
{
	void EnvironmentSystem::OnUpdateRuntime(Timestep ts)
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

		switch (ModeManager::mSceneMode)
		{
		case SceneMode::None:
			break;
		case SceneMode::EnvironmentHdr:
			if (mainCamera)
				DrawEnvironmentHdr(glm::inverse(cameraTransform), mainCamera->GetProjection());
			break;
		case SceneMode::SkyBox:
			if (mainCamera)
				DrawSkyBox(glm::inverse(cameraTransform), mainCamera->GetProjection());
			break;
		default:
			break;
		}
	}

	void EnvironmentSystem::OnUpdateEditor(Timestep ts, EditorCamera& camera)
	{
		switch (ModeManager::mSceneMode)
		{
		case SceneMode::None:
			break;
		case SceneMode::EnvironmentHdr:
			DrawEnvironmentHdr(camera.GetViewMatrix(), camera.GetProjection());
			break;
		case SceneMode::SkyBox:
			DrawSkyBox(camera.GetViewMatrix(), camera.GetProjection());
			break;
		default:
			break;
		}
	}

	void EnvironmentSystem::DrawSkyBox(const glm::mat4& ViewMatrix, const glm::mat4& ProjectMatrix)
	{
		Ref<UniformBuffer> cameraUniform = Library<UniformBuffer>::GetInstance().GetCameraUniformBuffer();
		glm::mat4 ViewProjection = ProjectMatrix * glm::mat4(glm::mat3(ViewMatrix));
		cameraUniform->SetData(&ViewProjection, sizeof(ViewProjection));

		RenderCommand::Cull(0);

		RenderCommand::DepthFunc(DepthComp::LEQUAL);
		Library<Shader>::GetInstance().GetSkyBoxShader()->Bind();

		Library<CubeMapTexture>::GetInstance().Get("SkyBoxTexture")->Bind(0);
		Library<Shader>::GetInstance().GetSkyBoxShader()->SetInt("SkyBox", 0);
		Library<Model>::GetInstance().Get("Box")->Draw();

		RenderCommand::DepthFunc(DepthComp::LESS);
	}

	void EnvironmentSystem::DrawEnvironmentHdr(const glm::mat4& ViewMatrix, const glm::mat4& ProjectMatrix)
	{
		if (!ModeManager::bHdrUse)
		{
			return;
		}
		Ref<UniformBuffer> cameraUniform = Library<UniformBuffer>::GetInstance().GetCameraUniformBuffer();
		glm::mat4 ViewProjection = ProjectMatrix * glm::mat4(glm::mat3(ViewMatrix));
		cameraUniform->SetData(&ViewProjection, sizeof(ViewProjection));

		glm::mat4 captureProjection = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);
		glm::mat4 captureViews[] =
		{
			glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f,  0.0f,  0.0f), glm::vec3(0.0f, -1.0f,  0.0f)),
			glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(-1.0f,  0.0f,  0.0f), glm::vec3(0.0f, -1.0f,  0.0f)),
			glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f,  1.0f,  0.0f), glm::vec3(0.0f,  0.0f,  1.0f)),
			glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f,  0.0f), glm::vec3(0.0f,  0.0f, -1.0f)),
			glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f,  0.0f,  1.0f), glm::vec3(0.0f, -1.0f,  0.0f)),
			glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f,  0.0f, -1.0f), glm::vec3(0.0f, -1.0f,  0.0f))
		};

		Ref<Texture2D> hdrTex = Library<Texture2D>::GetInstance().Get("DefaultHdr");
		Ref<Shader> equirectangularToCubemapShader = Library<Shader>::GetInstance().Get("IBL_equirectangularToCubemap");
		equirectangularToCubemapShader->Bind();
		equirectangularToCubemapShader->SetInt("equirectangularMap", 0);
		equirectangularToCubemapShader->SetMat4("projection", captureProjection);

		Ref<CubeMapTexture> envCubemap = Library<CubeMapTexture>::GetInstance().Get("EnvironmentHdr");

		unsigned int captureFBO;
		unsigned int captureRBO;
		glGenFramebuffers(1, &captureFBO);
		glGenRenderbuffers(1, &captureRBO);

		glBindFramebuffer(GL_FRAMEBUFFER, captureFBO);
		glBindRenderbuffer(GL_RENDERBUFFER, captureRBO);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, 512, 512);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, captureRBO);

		//FramebufferSpecification fbSpec;
		//fbSpec.Attachments = { FramebufferTextureFormat::Depth };
		//fbSpec.Width = 512;
		//fbSpec.Height = 512;
		//static Ref<Framebuffer> captureFBO = Framebuffer::Create(fbSpec);
		//glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, captureRBO);

		hdrTex->Bind();
		//captureFBO->Bind();
		glBindFramebuffer(GL_FRAMEBUFFER, captureFBO);
		RenderCommand::SetViewport(0, 0, envCubemap->GetWidth(), envCubemap->GetHeight());
		//captureFBO->Bind();
		//captureFBO->BindDrawFramebuffer();
		for (unsigned int i = 0; i < 6; ++i)
		{
			equirectangularToCubemapShader->SetMat4("view", captureViews[i]);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, envCubemap->GetRendererID(), 0);
			RenderCommand::Clear();

			Library<Model>::GetInstance().Get("Box")->Draw();
		}
		//captureFBO->Unbind();
		glBindFramebuffer(GL_FRAMEBUFFER, 0);

		RenderCommand::SetViewport(0, 0, ConfigManager::mViewportSize.x, ConfigManager::mViewportSize.y);

		envCubemap->Bind(0);
		glGenerateMipmap(GL_TEXTURE_CUBE_MAP);

		glBindFramebuffer(GL_FRAMEBUFFER, 1);
		RenderCommand::DepthFunc(DepthComp::LEQUAL);
		Library<Shader>::GetInstance().GetSkyBoxShader()->Bind();
		Library<Shader>::GetInstance().GetSkyBoxShader()->SetInt("SkyBox", 0);
		Library<Model>::GetInstance().Get("Box")->Draw();
		RenderCommand::DepthFunc(DepthComp::LESS);
	}
}