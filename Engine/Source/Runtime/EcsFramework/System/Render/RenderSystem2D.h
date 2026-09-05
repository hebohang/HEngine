#pragma once

#include "Runtime/EcsFramework/System/System.h"
#include "Runtime/EcsFramework/Level/Level.h"
#include "Runtime/Graphics/Scene/RenderWorldSubmission.h"

#include <cstdint>

namespace HEngine
{
	struct RenderPrimitive2DFrame;

	class RenderSystem2D : public System
	{
	public:
		RenderSystem2D(Level* level) : System(level) {}
		virtual ~RenderSystem2D() = default;
	public:
		void OnUpdateRuntime(Timestep ts) override;
		void OnRuntiemStart() override;
		void OnRuntimeStop() override;
		[[nodiscard]] bool ShouldUpdateWhenRuntimePaused() const override { return true; }
		[[nodiscard]] Ref<const RenderWorld> AcquireLatestRenderWorld() const
		{
			return mRenderWorldSubmission.AcquireLatest();
		}
#if WITH_EDITOR
		void OnUpdateEditor(Timestep ts, EditorCamera& camera) override;
#endif
	private:
		void SubmitPrimitiveFrame(RenderPrimitive2DFrame frame, bool bEditorView);

		RenderWorldSubmission mRenderWorldSubmission;
		uint64_t mRenderFrameNumber = 0;
	};
}
