#include "Editor/Preview/EditorOffscreenViewport.h"

#include "Runtime/Core/AppFramework/Application.h"
#include "Runtime/Core/Log/Log.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace HEngine
{
	EditorOffscreenViewport::EditorOffscreenViewport(std::string debugName)
		: mDebugName(std::move(debugName))
	{
		if (mDebugName.empty())
			mDebugName = "Editor Offscreen Viewport";
	}

	EditorOffscreenViewport::~EditorOffscreenViewport()
	{
		Release();
	}

	void EditorOffscreenViewport::Activate(uint32_t width, uint32_t height)
	{
		width = std::max(width, 1u);
		height = std::max(height, 1u);

		Application& application = Application::GetInstance();
		if (!mOffscreenView)
		{
			mOffscreenView = application.CreateOffscreenView(width, height, mDebugName);
			mWidth = width;
			mHeight = height;
			ResetResizeCandidate();
			return;
		}

		if (width == mWidth && height == mHeight)
		{
			ResetResizeCandidate();
			return;
		}

		if (width != mCandidateWidth || height != mCandidateHeight)
		{
			mCandidateWidth = width;
			mCandidateHeight = height;
			mCandidateStableFrames = 1;
			mLastResizeObservationFrame = application.GetFrameNumber();
			return;
		}

		const uint64_t frameNumber = application.GetFrameNumber();
		if (frameNumber == mLastResizeObservationFrame)
			return;
		mLastResizeObservationFrame = frameNumber;
		++mCandidateStableFrames;
		if (mCandidateStableFrames < ResizeStabilityFrames)
			return;

		UnregisterImGuiTexture();
		application.ResizeOffscreenView(mOffscreenView, width, height);
		mWidth = width;
		mHeight = height;
		ResetResizeCandidate();
	}

	void EditorOffscreenViewport::Submit(Ref<const RenderWorld> world)
	{
		if (!mOffscreenView)
			throw std::logic_error("Editor offscreen viewport must be active before submission");

		Application::GetInstance().SubmitOffscreenView(mOffscreenView, std::move(world));
	}

	TextureReadbackHandle EditorOffscreenViewport::RequestCapture(
		std::filesystem::path outputPath)
	{
		if (!mOffscreenView)
			throw std::logic_error("Editor offscreen viewport must be active before capture");
		return Application::GetInstance().RequestOffscreenCapture(
			mOffscreenView,
			std::move(outputPath));
	}

	ImGuiTextureHandle EditorOffscreenViewport::GetImGuiTexture()
	{
		if (!mOffscreenView)
			return {};

		Application& application = Application::GetInstance();
		const RuntimeOffscreenOutput output = application.GetOffscreenOutput(mOffscreenView);
		if (!output)
			return {};

		if (!mImGuiTexture || mOutputGeneration != output.Generation)
		{
			UnregisterImGuiTexture();
			mImGuiTexture = application.RegisterImGuiTexture(output.Color, output.ColorFormat);
			mOutputGeneration = output.Generation;
		}

		return mImGuiTexture;
	}

	void EditorOffscreenViewport::Release() noexcept
	{
		const ImGuiTextureHandle imguiTexture = std::exchange(mImGuiTexture, {});
		const RuntimeOffscreenViewHandle offscreenView = std::exchange(mOffscreenView, {});
		mOutputGeneration = 0;
		mWidth = 0;
		mHeight = 0;
		ResetResizeCandidate();
		if (!imguiTexture && !offscreenView)
			return;

		Application& application = Application::GetInstance();
		if (imguiTexture)
		{
			try
			{
				application.UnregisterImGuiTexture(imguiTexture);
			}
			catch (const std::exception& exception)
			{
				Log::WriteFallbackError("Editor offscreen ImGui texture release failed.", exception.what());
			}
			catch (...)
			{
				Log::WriteFallbackError("Editor offscreen ImGui texture release failed.", "unknown exception");
			}
		}

		if (offscreenView)
		{
			try
			{
				application.DestroyOffscreenView(offscreenView);
			}
			catch (const std::exception& exception)
			{
				Log::WriteFallbackError("Editor offscreen view release failed.", exception.what());
			}
			catch (...)
			{
				Log::WriteFallbackError("Editor offscreen view release failed.", "unknown exception");
			}
		}
	}

	void EditorOffscreenViewport::UnregisterImGuiTexture()
	{
		const ImGuiTextureHandle texture = std::exchange(mImGuiTexture, {});
		mOutputGeneration = 0;
		if (texture)
			Application::GetInstance().UnregisterImGuiTexture(texture);
	}

	void EditorOffscreenViewport::ResetResizeCandidate() noexcept
	{
		mCandidateWidth = 0;
		mCandidateHeight = 0;
		mCandidateStableFrames = 0;
		mLastResizeObservationFrame = 0;
	}
}
