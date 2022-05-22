#include "hepch.h"

#include "Runtime/EcsFramework/System/Render/EnvironmentSystem.h"
#include "Runtime/Resource/ModeManager/ModeManager.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
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
	static uint32_t id = 0;
	static uint32_t oldId = 0;
	EnvironmentHdrSettings EnvironmentSystem::environmentSettings;

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
		Library<Mesh>::GetInstance().Get("Box")->Draw();

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

		oldId = id;
		Ref<Texture2D> hdrTex = Library<Texture2D>::GetInstance().Get("DefaultHdr");
		id = hdrTex->GetRendererID();

		Ref<CubeMapTexture> envCubemap = Library<CubeMapTexture>::GetInstance().Get("EnvironmentHdr");

		if (id != oldId)
		{
			Ref<Shader> equirectangularToCubemapShader = Library<Shader>::GetInstance().Get("IBL_equirectangularToCubemap");
			equirectangularToCubemapShader->Bind();
			equirectangularToCubemapShader->SetInt("equirectangularMap", 0);
			equirectangularToCubemapShader->SetMat4("projection", captureProjection);

			int framebufferOld = 0;
			framebufferOld = RenderCommand::GetDrawFrameBuffer();

			// Temp
			unsigned int captureFBO;
			unsigned int captureRBO;
			glGenFramebuffers(1, &captureFBO);
			glGenRenderbuffers(1, &captureRBO);

			glBindFramebuffer(GL_FRAMEBUFFER, captureFBO);
			glBindRenderbuffer(GL_RENDERBUFFER, captureRBO);
			glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, 512, 512);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, captureRBO);

			hdrTex->Bind();
			glBindFramebuffer(GL_FRAMEBUFFER, captureFBO);
			RenderCommand::SetViewport(0, 0, envCubemap->GetWidth(), envCubemap->GetHeight());
			for (unsigned int i = 0; i < 6; ++i)
			{
				equirectangularToCubemapShader->SetMat4("view", captureViews[i]);
				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, envCubemap->GetRendererID(), 0);
				RenderCommand::Clear();

				Library<Mesh>::GetInstance().Get("Box")->Draw();
			}
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			// End temp

			//FramebufferSpecification fbSpec;
			//fbSpec.Attachments = { FramebufferTextureFormat::DEPTH24STENCIL8 };
			//fbSpec.Width = 512;
			//fbSpec.Height = 512;
			//static Ref<Framebuffer> captureFBO = Framebuffer::Create(fbSpec);

			//hdrTex->Bind();
			//captureFBO->Bind();
			//captureFBO->BindDrawFramebuffer();
			//RenderCommand::SetViewport(0, 0, envCubemap->GetWidth(), envCubemap->GetHeight());
			//for (unsigned int i = 0; i < 6; ++i)
			//{
			//	equirectangularToCubemapShader->SetMat4("view", captureViews[i]);
			//	captureFBO->FramebufferTexture2D(i, envCubemap->GetRendererID());
			//	RenderCommand::Clear();

			//	Library<Mesh>::GetInstance().Get("Box")->Draw();
			//}
			//captureFBO->Unbind();


			envCubemap->Bind(0);
			envCubemap->GenerateMipmap();

			// irradiance map
			Ref<CubeMapTexture> irradianceMap = Library<CubeMapTexture>::GetInstance().Get("EnvironmentIrradiance");
			Ref<Shader> irradianceShader = Library<Shader>::GetInstance().Get("IBL_irradiance");



			//Temp
			// pbr: create an irradiance cubemap, and re-scale capture FBO to irradiance scale.
			// --------------------------------------------------------------------------------
			glBindTexture(GL_TEXTURE_CUBE_MAP, irradianceMap->GetRendererID());
			for (unsigned int i = 0; i < 6; ++i)
			{
				glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_RGB16F, 32, 32, 0, GL_RGB, GL_FLOAT, nullptr);
			}
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

			glBindFramebuffer(GL_FRAMEBUFFER, captureFBO);
			glBindRenderbuffer(GL_RENDERBUFFER, captureRBO);
			glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, 32, 32);

			// pbr: solve diffuse integral by convolution to create an irradiance (cube)map.
			// -----------------------------------------------------------------------------
			irradianceShader->Bind();
			irradianceShader->SetInt("environmentMap", 0);
			irradianceShader->SetMat4("projection", captureProjection);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_CUBE_MAP, envCubemap->GetRendererID());

			glViewport(0, 0, 32, 32); // don't forget to configure the viewport to the capture dimensions.
			glBindFramebuffer(GL_FRAMEBUFFER, captureFBO);
			for (unsigned int i = 0; i < 6; ++i)
			{
				irradianceShader->SetMat4("view", captureViews[i]);
				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, irradianceMap->GetRendererID(), 0);
				glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

				Library<Mesh>::GetInstance().Get("Box")->Draw();
			}
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			//end temp


			//captureFBO->Bind();
			//glBindRenderbuffer(GL_RENDERBUFFER, captureFBO->GetDepthAttachmentRendererID());
			//glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 32, 32);

			//irradianceShader->Bind();
			//irradianceShader->SetInt("environmentMap", 0);
			//irradianceShader->SetMat4("projection", captureProjection);
			//envCubemap->Bind(0);

			//RenderCommand::SetViewport(0, 0, 32, 32); // don't forget to configure the viewport to the capture dimensions.
			//captureFBO->Bind();
			//for (unsigned int i = 0; i < 6; ++i)
			//{
			//	irradianceShader->SetMat4("view", captureViews[i]);
			//	captureFBO->FramebufferTexture2D(i, irradianceMap->GetRendererID());
			//	RenderCommand::Clear();

			//	//glDrawBuffer(GL_COLOR_ATTACHMENT0);
			//	Library<Mesh>::GetInstance().Get("Box")->Draw();
			//}
			//captureFBO->Unbind();
			Library<CubeMapTexture>::GetInstance().Set("EnvironmentIrradiance", irradianceMap);

			// prefilter map
			Ref<CubeMapTexture> prefilterMap = Library<CubeMapTexture>::GetInstance().Get("EnvironmentPrefilter");
			Ref<Shader> prefilterShader = Library<Shader>::GetInstance().Get("IBL_prefilter");
			prefilterMap->Bind();
			prefilterMap->GenerateMipmap();
			prefilterShader->Bind();
			prefilterShader->SetInt("environmentMap", 0);
			prefilterShader->SetMat4("projection", captureProjection);
			envCubemap->Bind(0);

			// Temp
			glBindFramebuffer(GL_FRAMEBUFFER, captureFBO);
			unsigned int maxMipLevels = 5;
			for (unsigned int mip = 0; mip < maxMipLevels; ++mip)
			{
				// reisze framebuffer according to mip-level size.
				unsigned int mipWidth = static_cast<unsigned int>(128 * std::pow(0.5, mip));
				unsigned int mipHeight = static_cast<unsigned int>(128 * std::pow(0.5, mip));
				glBindRenderbuffer(GL_RENDERBUFFER, captureRBO);
				glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, mipWidth, mipHeight);
				glViewport(0, 0, mipWidth, mipHeight);

				float roughness = (float)mip / (float)(maxMipLevels - 1);
				prefilterShader->SetFloat("roughness", roughness);
				for (unsigned int i = 0; i < 6; ++i)
				{
					prefilterShader->SetMat4("view", captureViews[i]);
					glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, prefilterMap->GetRendererID(), mip);

					RenderCommand::Clear();
					Library<Mesh>::GetInstance().Get("Box")->Draw();
				}
			}
			glBindFramebuffer(GL_FRAMEBUFFER, 0);

			// End temp


			//prefilterShader->Bind();
			//prefilterShader->SetInt("environmentMap", 0);
			//prefilterShader->SetMat4("projection", captureProjection);
			//envCubemap->Bind(0);
			//captureFBO->Bind();
			//unsigned int maxMipLevels = 5;
			//for (unsigned int mip = 0; mip < maxMipLevels; ++mip)
			//{
			//	// reisze framebuffer according to mip-level size.
			//	unsigned int mipWidth = static_cast<unsigned int>(128 * std::pow(0.5, mip));
			//	unsigned int mipHeight = static_cast<unsigned int>(128 * std::pow(0.5, mip));
			//	
			//	glBindRenderbuffer(GL_RENDERBUFFER, captureFBO->GetDepthAttachmentRendererID());
			//	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, mipWidth, mipHeight);
			//	RenderCommand::SetViewport(0, 0, mipWidth, mipHeight);

			//	float roughness = (float)mip / (float)(maxMipLevels - 1);
			//	prefilterShader->SetFloat("roughness", roughness);
			//	for (unsigned int i = 0; i < 6; ++i)
			//	{
			//		prefilterShader->SetMat4("view", captureViews[i]);
			//		captureFBO->FramebufferTexture2D(i, prefilterMap->GetRendererID());

			//		RenderCommand::Clear();
			//		Library<Mesh>::GetInstance().Get("Box")->Draw();
			//	}
			//}
			//captureFBO->Unbind();
			Library<CubeMapTexture>::GetInstance().Set("EnvironmentPrefilter", prefilterMap);


			RenderCommand::BindFrameBuffer(framebufferOld);
		}

		envCubemap->Bind(0);

		RenderCommand::SetViewport(0, 0, ConfigManager::mViewportSize.x, ConfigManager::mViewportSize.y);

		RenderCommand::DepthFunc(DepthComp::LEQUAL);

		Library<Shader>::GetInstance().Get("IBL_pbr_static")->Bind();
		Library<Shader>::GetInstance().Get("IBL_pbr_static")->SetFloat("exposure", environmentSettings.exposure);
		
		Library<Shader>::GetInstance().GetDefaultShader()->Bind();
		Library<Shader>::GetInstance().GetDefaultShader()->SetFloat("exposure", environmentSettings.exposure);

		Library<Shader>::GetInstance().Get("IBL_pbr_anim")->Bind();
		Library<Shader>::GetInstance().Get("IBL_pbr_anim")->SetFloat("exposure", environmentSettings.exposure);

		Ref<Shader> backgroundShader = Library<Shader>::GetInstance().Get("IBL_background");
		backgroundShader->Bind();
		backgroundShader->SetInt("environmentMap", 0);
		backgroundShader->SetFloat("SkyBoxLod", environmentSettings.SkyBoxLod);
		backgroundShader->SetFloat("exposure", environmentSettings.exposure);

		Library<Mesh>::GetInstance().Get("Box")->Draw();
		RenderCommand::DepthFunc(DepthComp::LESS);
	}
}