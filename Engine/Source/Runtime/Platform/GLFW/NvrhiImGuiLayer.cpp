#include "Hepch.h"

#include "Runtime/ImGui/NvrhiImGuiLayer.h"

#include "Runtime/Core/Log/Log.h"
#include "Runtime/Platform/GLFW/CommonWindow.h"

#include <backends/imgui_impl_glfw.h>
#include <imgui.h>

#ifdef WITH_EDITOR
#include <ImGuizmo.h>
#endif

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace HEngine
{
    struct NvrhiImGuiLayer::Impl
    {
        CommonWindow* Window = nullptr;
        GLFWwindow* GlfwWindow = nullptr;
        ImGuiContext* Context = nullptr;
        std::unique_ptr<NvrhiImGuiRenderer> Renderer;
        bool bPlatformInitialized = false;
        bool bCpuFrameActive = false;
        bool bInitialized = false;
        bool bViewportWarningIssued = false;
    };

    namespace
    {
        [[nodiscard]] float GetContentScale(GLFWwindow* window)
        {
            float xScale = 1.0f;
            float yScale = 1.0f;
            glfwGetWindowContentScale(window, &xScale, &yScale);
            return std::clamp((xScale + yScale) * 0.5f, 0.75f, 3.0f);
        }
    }

    NvrhiImGuiLayer::NvrhiImGuiLayer()
        : ImGuiLayer("NvrhiImGuiLayer")
    {
    }

    NvrhiImGuiLayer::~NvrhiImGuiLayer()
    {
        try
        {
            Shutdown();
        }
        catch (const std::exception& exception)
        {
            Log::WriteFallbackError("NVRHI ImGui layer destruction failed.", exception.what());
        }
        catch (...)
        {
            Log::WriteFallbackError("NVRHI ImGui layer destruction failed.", "unknown exception");
        }
    }

    void NvrhiImGuiLayer::Initialize(const NvrhiImGuiLayerDesc& desc)
    {
        if (mImpl)
            throw std::logic_error("NVRHI ImGui layer is already initialized");
        if (!desc.MainWindow)
            throw std::invalid_argument("NVRHI ImGui layer requires a main Window");
        if (ImGui::GetCurrentContext() != nullptr)
            throw std::logic_error("NVRHI ImGui layer requires exclusive ownership of the ImGui context");

        auto impl = std::make_unique<Impl>();
        impl->Window = dynamic_cast<CommonWindow*>(desc.MainWindow);
        if (!impl->Window)
            throw std::invalid_argument("NVRHI ImGui layer requires the GLFW Window implementation");
        impl->GlfwWindow = impl->Window->GetGlfwWindow();
        if (!impl->GlfwWindow)
            throw std::invalid_argument("NVRHI ImGui layer received a Window without a GLFW handle");

        mImpl = std::move(impl);
        try
        {
            ImguiInit(
                GetContentScale(mImpl->GlfwWindow),
                desc.bEnableDocking,
                false);
            mImpl->Context = ImGui::GetCurrentContext();

            if (!ImGui_ImplGlfw_InitForOther(
                    mImpl->GlfwWindow,
                    desc.bInstallGlfwCallbacks))
            {
                throw std::runtime_error("Failed to initialize the ImGui GLFW platform backend");
            }
            mImpl->bPlatformInitialized = true;

            ImGuiIO& io = ImGui::GetIO();
            io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
            mImpl->Renderer = std::make_unique<NvrhiImGuiRenderer>(desc.Renderer);
            mImpl->bInitialized = true;
        }
        catch (...)
        {
            const std::exception_ptr initializationFailure = std::current_exception();
            if (mImpl && !mImpl->Context)
                mImpl->Context = ImGui::GetCurrentContext();
            try
            {
                Shutdown();
            }
            catch (...)
            {
                Log::WriteFallbackError(
                    "NVRHI ImGui initialization rollback failed.",
                    "cleanup raised an exception");
            }
            std::rethrow_exception(initializationFailure);
        }
    }

    void NvrhiImGuiLayer::Shutdown()
    {
        if (!mImpl)
            return;

        if (mImpl->Context)
            ImGui::SetCurrentContext(mImpl->Context);
        CancelCpuFrame();

        if (mImpl->Renderer)
        {
            mImpl->Renderer->Shutdown();
            mImpl->Renderer.reset();
        }
        if (mImpl->bPlatformInitialized)
        {
            ImGui_ImplGlfw_Shutdown();
            mImpl->bPlatformInitialized = false;
        }
        if (mImpl->Context)
        {
            ImGui::DestroyContext(mImpl->Context);
            mImpl->Context = nullptr;
        }
        mImpl.reset();
    }

    void NvrhiImGuiLayer::OnAttach()
    {
        if (!IsInitialized())
            throw std::logic_error("NVRHI ImGui layer must be initialized before it is attached");
    }

    void NvrhiImGuiLayer::OnDetach()
    {
        Shutdown();
    }

    void NvrhiImGuiLayer::Begin()
    {
        BeginCpuFrame();
    }

    void NvrhiImGuiLayer::End()
    {
        throw std::logic_error(
            "NVRHI ImGui End cannot record without an explicit command list; "
            "call RecordGpuFrame instead");
    }

    void NvrhiImGuiLayer::BeginCpuFrame()
    {
        if (!IsInitialized())
            throw std::logic_error("NVRHI ImGui BeginCpuFrame called before Initialize");
        if (mImpl->bCpuFrameActive)
            throw std::logic_error("NVRHI ImGui BeginCpuFrame called twice");

        ImGui::SetCurrentContext(mImpl->Context);
        ImGuiIO& io = ImGui::GetIO();
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
            if (!mImpl->bViewportWarningIssued)
            {
                HE_CORE_WARN(
                    "NVRHI ImGui secondary platform windows are disabled until "
                    "they have NVRHI-owned swap chains.");
                mImpl->bViewportWarningIssued = true;
            }
        }

        ApplyUIScale(GetContentScale(mImpl->GlfwWindow));
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        mImpl->bCpuFrameActive = true;

#ifdef WITH_EDITOR
        ImGuizmo::BeginFrame();
#endif
    }

    void NvrhiImGuiLayer::RecordGpuFrame(
        nvrhi::ICommandList* commandList,
        nvrhi::IFramebuffer* framebuffer,
        uint32_t frameSlot)
    {
        if (!IsInitialized())
            throw std::logic_error("NVRHI ImGui RecordGpuFrame called before Initialize");
        if (!mImpl->bCpuFrameActive)
            throw std::logic_error("NVRHI ImGui RecordGpuFrame requires BeginCpuFrame");
        if (!commandList || !framebuffer)
            throw std::invalid_argument("NVRHI ImGui RecordGpuFrame requires explicit GPU targets");

        ImGui::SetCurrentContext(mImpl->Context);
        try
        {
            mImpl->Renderer->NewFrame(commandList, frameSlot);
            ImGui::Render();
            mImpl->bCpuFrameActive = false;
            mImpl->Renderer->RenderDrawData(
                ImGui::GetDrawData(),
                commandList,
                framebuffer,
                frameSlot);
        }
        catch (...)
        {
            CancelCpuFrame();
            throw;
        }
    }

    void NvrhiImGuiLayer::CancelCpuFrame() noexcept
    {
        if (!mImpl || !mImpl->bCpuFrameActive || !mImpl->Context)
            return;

        ImGui::SetCurrentContext(mImpl->Context);
        ImGui::EndFrame();
        mImpl->bCpuFrameActive = false;
    }

    ImGuiTextureHandle NvrhiImGuiLayer::RegisterTexture(
        const nvrhi::TextureHandle& texture,
        const nvrhi::SamplerHandle& sampler,
        nvrhi::Format viewFormat,
        nvrhi::TextureSubresourceSet subresources)
    {
        if (!IsInitialized())
            throw std::logic_error("NVRHI ImGui texture registration requires Initialize");
        return mImpl->Renderer->RegisterTexture(texture, sampler, viewFormat, subresources);
    }

    void NvrhiImGuiLayer::UnregisterTexture(
        ImGuiTextureHandle handle,
        uint32_t retireFrameSlot)
    {
        if (!IsInitialized())
            throw std::logic_error("NVRHI ImGui texture retirement requires Initialize");
        mImpl->Renderer->UnregisterTexture(handle, retireFrameSlot);
    }

    bool NvrhiImGuiLayer::IsTextureValid(ImGuiTextureHandle handle) const
    {
        return IsInitialized() && mImpl->Renderer->IsTextureValid(handle);
    }

    ImTextureID NvrhiImGuiLayer::ToImTextureID(ImGuiTextureHandle handle) noexcept
    {
        return NvrhiImGuiRenderer::ToImTextureID(handle);
    }

    ImGuiTextureHandle NvrhiImGuiLayer::FromImTextureID(ImTextureID textureId) noexcept
    {
        return NvrhiImGuiRenderer::FromImTextureID(textureId);
    }

    ImGuiTextureHandle NvrhiImGuiLayer::GetFontTexture() const noexcept
    {
        if (!IsInitialized())
            return {};
        return mImpl->Renderer->GetFontTexture();
    }

    uint32_t NvrhiImGuiLayer::GetRegisteredTextureCount() const noexcept
    {
        if (!IsInitialized())
            return 0;
        return mImpl->Renderer->GetRegisteredTextureCount();
    }

    const NvrhiImGuiRendererStats& NvrhiImGuiLayer::GetRendererStats() const
    {
        if (!IsInitialized())
            throw std::logic_error("NVRHI ImGui renderer statistics require Initialize");
        return mImpl->Renderer->GetStats();
    }

    void NvrhiImGuiLayer::ResetRendererStats() noexcept
    {
        if (IsInitialized())
            mImpl->Renderer->ResetStats();
    }

    bool NvrhiImGuiLayer::IsInitialized() const noexcept
    {
        return mImpl && mImpl->bInitialized && mImpl->Renderer;
    }

    bool NvrhiImGuiLayer::IsCpuFrameActive() const noexcept
    {
        return mImpl && mImpl->bCpuFrameActive;
    }

    bool NvrhiImGuiLayer::AreSecondaryViewportsEnabled() const noexcept
    {
        if (!IsInitialized() || !mImpl->Context)
            return false;
        ImGui::SetCurrentContext(mImpl->Context);
        return (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0;
    }
}
