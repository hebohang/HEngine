#pragma once

#include "Runtime/Renderer/Framebuffer.h"
#include "Runtime/Renderer/PostProcessing.h"

namespace HEngine
{
	struct RenderPassSpecification
	{
		Ref<Framebuffer> TargetFramebuffer = nullptr;
		std::string DebugName = "";
	};

	class RenderPass
	{
	public:
		RenderPass(RenderPassSpecification Spec) : mSpecification(Spec) {};
		virtual ~RenderPass() = default;

	public:
		RenderPassSpecification& GetSpecification() { return mSpecification; };
		const RenderPassSpecification& GetSpecification() const { return mSpecification; };

		void AddPostProcessing(PostProcessingType type);

		uint32_t ExcuteAndReturnFinalTex();
	public:
		std::vector<Scope<PostProcessing>> mPostProcessings;
	private:
		RenderPassSpecification mSpecification;
	};
}