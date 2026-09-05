#pragma once

#include "Runtime/EcsFramework/System/Render/RenderWorldExtractor.h"
#include "Runtime/EcsFramework/System/System.h"
#include "Runtime/EcsFramework/Level/Level.h"
#include "Runtime/Graphics/Scene/RenderWorldSubmission.h"

#include <cstdint>

namespace HEngine
{
	class RenderSystem3D : public System
	{
	public:
		explicit RenderSystem3D(Level* level);
		virtual ~RenderSystem3D() = default;
	public:
		void OnUpdateRuntime(Timestep ts) override;
		[[nodiscard]] bool ShouldUpdateWhenRuntimePaused() const override { return true; }
		void OnRuntiemStart() override;
		void OnRuntimeStop() override;
		[[nodiscard]] Ref<const RenderWorld> AcquireLatestRenderWorld() const
		{
			return mRenderWorldSubmission.AcquireLatest();
		}
		// Camera controllers and future temporal-AA integrations publish explicit
		// discontinuities through these APIs. Ordinary camera/object motion does
		// not call them and remains eligible for reprojection.
		void RequestCameraCut() noexcept { bPendingCameraCut = true; }
		void NotifyJitterSequenceReset() noexcept;
		[[nodiscard]] uint64_t GetSceneGeneration() const noexcept
		{
			return mSceneGeneration;
		}
#if WITH_EDITOR
		void OnUpdateEditor(Timestep ts, EditorCamera& camera) override;
		void OnUpdateEditorAtMaterialTime(
			Timestep ts,
			EditorCamera& camera,
			double materialTimeSeconds);
		[[nodiscard]] bool RequiresContinuousEditorUpdate() const override;
#endif
	private:
		void ExtractAndSubmit(RenderViewDescription view);
#if WITH_EDITOR
		void SubmitEditorView(
			Timestep ts,
			EditorCamera& camera,
			std::optional<double> materialTimeSeconds);
#endif

		RenderWorldExtractor mRenderWorldExtractor;
		RenderWorldSubmission mRenderWorldSubmission;
		uint64_t mRenderFrameNumber = 0;
		uint64_t mSceneGeneration = 1;
		uint64_t mJitterSequenceGeneration = 1;
		uint64_t mProjectionGeneration = 1;
		glm::mat4 mPreviousProjection{ 1.0f };
		bool bHasPreviousProjection = false;
		bool bPendingCameraCut = true;
	};
}
