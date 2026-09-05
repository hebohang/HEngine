#pragma once

#include "Editor/Preview/EditorOffscreenViewport.h"

#include "Runtime/Camera/EditorCamera.h"
#include "Runtime/Core/Base/Base.h"
#include "Runtime/Core/Timestep.h"
#include "Runtime/Graphics/Renderer2D.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace HEngine
{
	class Level;

	class SceneThumbnailPreview final
	{
	public:
		explicit SceneThumbnailPreview(std::string debugName);
		~SceneThumbnailPreview();

		SceneThumbnailPreview(const SceneThumbnailPreview&) = delete;
		SceneThumbnailPreview& operator=(const SceneThumbnailPreview&) = delete;

		void SetScene(Ref<Level> scene);
		void ClearScene() noexcept;
		[[nodiscard]] bool Render(Timestep ts, uint32_t width, uint32_t height);
		[[nodiscard]] TextureReadbackHandle RequestCapture(std::filesystem::path outputPath);
		void Release() noexcept;

		[[nodiscard]] const std::string& GetError() const noexcept { return mError; }

	private:
		void ResetCamera();

		Ref<Level> mScene;
		EditorOffscreenViewport mViewport;
		RenderPrimitiveCollector mPrimitiveCollector;
		EditorCamera mCamera;
		glm::vec3 mPreviewCenter{ 0.0f };
		float mPreviewRadius = 5.0f;
		bool mIs3D = true;
		std::string mError;
	};
}
