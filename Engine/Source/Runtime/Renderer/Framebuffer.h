#pragma once

#include "Runtime/Core/Base/Base.h"

namespace HEngine
{
	enum class FramebufferTextureFormat
	{
		None = 0, 

		// Color
		RGBA8,
		RED_INTEGER,

		// Depth/stencil
		DEPTH32F_TEX3D,
		DEPTH24STENCIL8,
	};

	struct FramebufferTextureSpecification
	{
		FramebufferTextureSpecification() = default;
		FramebufferTextureSpecification(FramebufferTextureFormat format)
			: TextureFormat(format) {}

		FramebufferTextureFormat TextureFormat = FramebufferTextureFormat::None;
		// TODO: filtering/wrap
	};

	struct FramebufferAttachmentSpecification
	{
		FramebufferAttachmentSpecification() = default;
		FramebufferAttachmentSpecification(std::initializer_list<FramebufferTextureSpecification> attachments)
			: Attachments(attachments) {}

		std::vector<FramebufferTextureSpecification> Attachments;
	};

    struct FramebufferSpecification
    {
        uint32_t Width, Height;
		FramebufferAttachmentSpecification Attachments;
        uint32_t Samples = 1;

        bool SwapChainTarget = false;
    };

    class Framebuffer
    {
    public:
        virtual ~Framebuffer() = default;

        virtual void Bind() = 0;
		virtual void BindReadFramebuffer() = 0;
		virtual void BindDrawFramebuffer() = 0;
        virtual void Unbind() = 0;

        virtual void Resize(uint32_t width, uint32_t height) = 0;
		virtual int ReadPixel(uint32_t attachmentIndex, int x, int y) = 0;

		virtual void ClearAttachment(uint32_t attachmentIndex, int value) = 0;

		[[nodiscard]] virtual uint32_t GetColorAttachmentRendererID(uint32_t index = 0) const = 0;
		[[nodiscard]] virtual uint32_t GetDepthAttachmentRendererID() const = 0;

		[[nodiscard]] virtual const FramebufferSpecification& GetSpecification() const = 0;

		virtual void FramebufferTexture2D(uint32_t cubemapIndex, uint32_t cubemapID, uint32_t slot = 0) = 0;

		[[nodiscard]] virtual Ref<class Texture3D> GetDepthTex3D() const = 0;
		virtual void BindDepthTex3D(uint32_t slot) = 0;
		virtual void UnBindDepthTex3D(uint32_t slot) = 0;

        static Ref<Framebuffer> Create(const FramebufferSpecification& spec);
    };
}
