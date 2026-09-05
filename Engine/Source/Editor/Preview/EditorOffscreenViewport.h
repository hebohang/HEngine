#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Graphics/Gui/NvrhiImGuiRenderer.h"
#include "Runtime/Graphics/Renderer/RuntimeRenderer.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace HEngine
{
	struct RenderWorld;

	class EditorOffscreenViewport final
	{
	public:
		explicit EditorOffscreenViewport(std::string debugName);
		~EditorOffscreenViewport();

		EditorOffscreenViewport(const EditorOffscreenViewport&) = delete;
		EditorOffscreenViewport& operator=(const EditorOffscreenViewport&) = delete;

		// width and height are final viewport pixels; do not apply UI scaling here.
		void Activate(uint32_t width, uint32_t height);
		void Submit(Ref<const RenderWorld> world);
		[[nodiscard]] TextureReadbackHandle RequestCapture(std::filesystem::path outputPath);
		[[nodiscard]] ImGuiTextureHandle GetImGuiTexture();
		[[nodiscard]] bool IsActive() const noexcept { return static_cast<bool>(mOffscreenView); }

		void Release() noexcept;

	private:
		void UnregisterImGuiTexture();
		void ResetResizeCandidate() noexcept;

		static constexpr uint32_t ResizeStabilityFrames = 3;

		std::string mDebugName;
		RuntimeOffscreenViewHandle mOffscreenView;
		ImGuiTextureHandle mImGuiTexture;
		uint64_t mOutputGeneration = 0;
		uint32_t mWidth = 0;
		uint32_t mHeight = 0;
		uint32_t mCandidateWidth = 0;
		uint32_t mCandidateHeight = 0;
		uint32_t mCandidateStableFrames = 0;
		uint64_t mLastResizeObservationFrame = 0;
	};
}
