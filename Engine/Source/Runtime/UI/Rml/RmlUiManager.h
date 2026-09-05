#pragma once

#include "Runtime/Core/Templates/PublicSingleton.h"
#include "Runtime/Core/UUID.h"
#include "Runtime/Graphics/Renderer/RuntimeUiRenderGraphPass.h"
#include "Runtime/UI/Rml/RmlRenderBackend.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace HEngine
{
    class Event;
    struct UIRes;
    class Window;

    struct RmlUiNativeEvent
    {
        std::string Type;
        std::string SourceElementId;
        std::string TargetElementId;
    };

    class RmlUiManager final : public PublicSingleton<RmlUiManager>
    {
        friend class PublicSingleton<RmlUiManager>;

    public:
        ~RmlUiManager();

        // Legacy entry point retained while Application is migrated. It reports
        // the missing device injection and never selects a fallback backend.
        void Init();
        void InitNvrhi(const NvrhiRmlRenderBackendDesc& desc);
        void InitNvrhi(
            const NvrhiRmlRenderBackendDesc& desc,
            Window& cursorWindow);
        void Shutdown();

        // CPU-side viewport/layout frame. This does not open a GPU command list.
        void BeginFrame(
            float viewportWidth,
            float viewportHeight,
            float uiScale,
            float renderWidth = 0.0f,
            float renderHeight = 0.0f);
        void Update();

        // GPU calls must occur after the command list is opened and before it is
        // submitted. Render closes this GPU frame automatically on an exception.
        void BeginGpuFrame(
            nvrhi::ICommandList* commandList,
            nvrhi::IFramebuffer* framebuffer,
            uint32_t frameSlot,
            uint64_t completedSubmission,
            nvrhi::ITexture* backdropTexture = nullptr);
        void Render();
        void EndGpuFrame();
        void NotifyGpuFrameSubmitted(uint32_t frameSlot, uint64_t submission);
        void AbortGpuFrame(uint32_t frameSlot) noexcept;
        bool OnEvent(Event& e);
        void SetViewportRect(float x, float y, float w, float h, float uiScale);

        bool LoadDocument(const std::string& uiAssetPath, const UUID& ownerScript, std::string* outDocumentId = nullptr);
        bool ShowDocument(const std::string& documentId);
        bool HideDocument(const std::string& documentId);
        bool ReloadDocument(const std::string& documentId);
        bool UnloadDocument(const std::string& documentId);
        bool SetActiveDocument(const std::string& documentId);
        void ClearOwnerDocuments(const UUID& ownerScript);

        bool SetElementText(const std::string& documentId, const std::string& elementId, const std::string& text);
        bool SetElementMarkup(const std::string& documentId, const std::string& elementId, const std::string& markup);
        bool SetElementAttribute(const std::string& documentId, const std::string& elementId, const std::string& name, const std::string& value);
        bool GetElementValue(const std::string& documentId, const std::string& elementId, std::string& outValue) const;
        bool SetDocumentEventHandler(
            const std::string& documentId,
            std::function<void(const RmlUiNativeEvent&)> handler);
        bool SetElementClass(const std::string& documentId, const std::string& elementId, const std::string& className, bool bEnabled);
        bool FocusElement(const std::string& documentId, const std::string& elementId);
        bool ClickElement(const std::string& documentId, const std::string& elementId);
        bool GetElementClass(const std::string& documentId, const std::string& elementId, const std::string& className, bool& outEnabled) const;
        bool SetElementStyle(const std::string& documentId, const std::string& elementId, const std::string& name, const std::string& value);
        bool GetElementStyle(const std::string& documentId, const std::string& elementId, const std::string& name, std::string& outValue) const;
        bool SetCustomMouseCursor(const std::string& documentId, const std::string& assetPath, uint32_t hotspotX, uint32_t hotspotY);
        bool SetMouseCursorVisible(const std::string& documentId, bool bVisible);
        void SetUserScale(float scale);

        [[nodiscard]] bool IsInitialized() const { return mInitialized; }
        [[nodiscard]] bool IsGpuFrameActive() const { return mGpuFrameActive; }
        [[nodiscard]] const NvrhiRmlRenderBackendStats& GetRenderStats() const;
        [[nodiscard]] RmlRenderCapabilities GetRenderCapabilities() const noexcept;
        void SetRuntimeUiTransition(RuntimeUiTransitionParameters transition) noexcept;
        void ClearRuntimeUiTransition() noexcept;
        [[nodiscard]] RuntimeUiTransitionParameters GetRuntimeUiTransition() const noexcept;
        [[nodiscard]] bool HasActiveDocument() const { return !mActiveDocumentId.empty(); }
        [[nodiscard]] bool IsMouseCursorVisibleRequested() const;
        [[nodiscard]] float GetUserScale() const { return mUserScale; }

    private:
        RmlUiManager() = default;

        void InitNvrhiInternal(
            const NvrhiRmlRenderBackendDesc& desc,
            Window* cursorWindow);

        struct Impl;
        struct DocumentState;
		enum class DeferredMutationType : std::uint8_t
		{
			Reload,
			Unload,
			ClearOwner
		};
		struct DeferredMutation
		{
			DeferredMutationType Type = DeferredMutationType::Unload;
			std::string Value;
		};

        DocumentState* FindDocument(const std::string& documentId);
        const DocumentState* FindDocument(const std::string& documentId) const;
        std::string BuildDocumentId(const std::string& canonicalCachePath, const UUID& ownerScript) const;
        Ref<DocumentState> CreateDocumentState(
            const std::string& uiAssetPath,
            const UUID& ownerScript,
            std::string& outDocumentId,
            bool bAllowExistingDocument);
        void DestroyDocumentDom(DocumentState& state);
		bool ReloadDocumentNow(const std::string& documentId);
		bool UnloadDocumentNow(const std::string& documentId);
		void ClearOwnerDocumentsNow(const UUID& ownerScript);
		[[nodiscard]] bool ShouldDeferDocumentMutation() const noexcept;
		void QueueDeferredMutation(DeferredMutationType type, std::string value);
		void BeginDocumentDispatch() noexcept;
		void EndDocumentDispatch();
		void DrainDeferredMutations();
        void LoadDefaultFonts();
        bool EnsureFontLoaded(const std::string& relativeFontPath, bool bFallbackFace = false);
        void LoadFontsForUiDefinition(const UIRes& uiRes);
        void RefreshMouseCursor();

    private:
        bool mInitialized = false;
        bool mRmlInitialized = false;
        bool mGpuFrameActive = false;
        bool mCustomCursorApplied = false;
        Window* mCursorWindow = nullptr;
        float mViewportX = 0.0f;
        float mViewportY = 0.0f;
        float mViewportW = 0.0f;
        float mViewportH = 0.0f;
        float mUIScale = 1.0f;
        float mUserScale = 1.0f;
        std::vector<std::string> mDocumentOrder;
        std::unordered_map<std::string, Ref<DocumentState>> mDocuments;
        std::unordered_map<UUID, std::vector<std::string>> mOwnerToDocuments;
        std::unordered_set<std::string> mLoadedFontPaths;
        std::string mActiveDocumentId;
		RuntimeUiTransitionParameters mRuntimeUiTransition;
		std::deque<DeferredMutation> mDeferredMutations;
		std::size_t mDispatchDepth = 0;
		bool mDrainingDeferredMutations = false;
        Ref<Impl> mImpl;
    };
}
