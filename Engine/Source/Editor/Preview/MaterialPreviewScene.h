#pragma once

#include "Editor/Preview/EditorOffscreenViewport.h"

#include "Runtime/Camera/EditorCamera.h"
#include "Runtime/Core/Base/Base.h"
#include "Runtime/Core/Timestep.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/Graphics/Renderer2D.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace HEngine
{
	class Event;
	class Level;
	class Material;
	class RenderSystem3D;

	class MaterialPreviewScene final
	{
	public:
		MaterialPreviewScene(std::string debugName, Ref<Material> material);
		~MaterialPreviewScene();

		MaterialPreviewScene(const MaterialPreviewScene&) = delete;
		MaterialPreviewScene& operator=(const MaterialPreviewScene&) = delete;

		void SetMaterial(Ref<Material> material);
		[[nodiscard]] bool Render(
			Timestep ts,
			uint32_t width,
			uint32_t height,
			std::optional<double> materialTimeSeconds = std::nullopt);
		[[nodiscard]] TextureReadbackHandle RequestCapture(std::filesystem::path outputPath);
		[[nodiscard]] ImGuiTextureHandle GetImGuiTexture();
		void SetViewportHovered(bool bHovered) noexcept { mViewportHovered = bHovered; }
		void OnEvent(Event& event);
		void ResetCamera();
		void Release() noexcept;

		[[nodiscard]] const std::string& GetError() const noexcept { return mError; }

	private:
		[[nodiscard]] bool EnsureScene();
		void BuildScene();
		void BindDraftMaterial();

		static constexpr const char* PreviewMeshPath =
			"Assets/Models/MaterialPreview/MaterialBall.hmesh";

		Ref<Material> mMaterial;
		EditorOffscreenViewport mViewport;
		Ref<Level> mScene;
		Scope<RenderSystem3D> mRenderSystem;
		RenderPrimitiveCollector mPrimitiveCollector;
		Entity mPreviewEntity;
		EditorCamera mCamera;
		glm::vec3 mPreviewCenter{ 0.0f };
		float mPreviewRadius = 1.0f;
		std::string mError;
		bool mViewportHovered = false;
		bool mBuildFailed = false;
	};
}
