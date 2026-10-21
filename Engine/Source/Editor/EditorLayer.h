#pragma once

#include "EditorHistory.h"
#include "Automation/EditorSceneMailbox.h"
#include "Preview/MaterialThumbnailService.h"
#include "Preview/SceneThumbnailService.h"

#include "HEngine.h"
#include "Panels/SceneHierarchyPanel.h"
#include "Panels/OutputLogPanel.h"
#include "Panels/BuildPublishPanel.h"

#include "Runtime/Camera/EditorCamera.h"
#include "Runtime/Graphics/Gui/NvrhiImGuiRenderer.h"
#include "Runtime/Graphics/Renderer/ViewportMode.h"
#include "Runtime/Events/Delegate.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "Runtime/EcsFramework/Serializer/SceneEditingService.h"
#include "Runtime/Scripting/ScriptPlayState.h"

#include <spdlog/sinks/base_sink.h>
#include <chrono>
#include <cstddef>
#include <future>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace HEngine
{
    class BasePanel;
    class Material;

    class EditorLayer : public Layer, public IEditorUndoClient
    {
    private:
        template<typename Mutex>
        class my_sink : public spdlog::sinks::base_sink <Mutex>
        {
        protected:
            void sink_it_(const spdlog::details::log_msg& msg) override
            {
                spdlog::memory_buf_t formatted;
                spdlog::sinks::base_sink<Mutex>::formatter_->format(msg, formatted);
                const std::string logText = fmt::to_string(formatted);
                const auto panelIt = mPanelMap.find(OutputLogPanel::GetTypeName());
                if (panelIt == mPanelMap.end() || !panelIt->second)
                    return;

                static_cast<OutputLogPanel*>(panelIt->second.get())->AddLog("%s", logText.c_str());
            }

            void flush_() override
            {
                std::cout << std::flush;
            }
        };
    public:
        EditorLayer();
        virtual ~EditorLayer() = default;

        static void EditorLogInit();

        virtual void OnAttach() override;
        virtual void OnDetach() override;

		void OnPreUpdate() override;
        void OnUpdate(Timestep ts) override;
        virtual void OnImGuiRender() override;
        void OnEvent(Event& e) override;
    public:
        [[nodiscard]] static const char* GetText(const char* label);

        void SaveBaseEditorConfig();
        void NotifyUndoClientActivity(IEditorUndoClient* client, bool bFocused, bool bHovered);
        void MarkSceneDirty(const char* label = "Edit Scene", bool bInteractive = false);
        void RequestOpenScene(const std::filesystem::path& path);
        std::string ValidateAssetMove() const;
        void RefreshAfterAssetMove(const std::filesystem::path& source, const std::filesystem::path& target);
        Json QueueSceneEditingRequest(const std::string& method, const Json& request);
        void QueueSceneOperations(Json operations);
        const std::string& GetLastSceneEditError() const { return mLastSceneEditError; }
        [[nodiscard]] IEditorUndoClient* GetSceneUndoClient() { return this; }
        [[nodiscard]] IEditorUndoClient* GetActiveUndoClient() const { return mActiveUndoClient; }
    public:
        // 对于材质编辑器，我们每次重新创立
		void CreateMaterialPanel(std::string matPath);
		[[nodiscard]] bool IsMaterialPanelOpen(std::string_view matPath) const;
		void PreviewMaterialAsset(const std::string& matPath, const Material& material);
		void CreateAssetPreviewPanel(std::string assetPath);
		[[nodiscard]] MaterialThumbnailService* GetMaterialThumbnailService() const noexcept
		{
			return mMaterialThumbnailService.get();
		}
		[[nodiscard]] SceneThumbnailService* GetSceneThumbnailService() const noexcept
		{
			return mSceneThumbnailService.get();
		}
		void CreateAnimControllerPanel(std::string controllerPath);
		void CreateUIPanel(std::string uiPath);
		// 对于音频编辑器，我们只创建一次
		void OpenAudioPanel(std::string audioPath);
    private:
        template<typename PanelType>
        void RegisterPanel(std::string panelName, bool* pPanelOpen);
        bool mStartupReadyReported = false;
		void FlushPendingMaterialPanels();
		void FlushPendingAssetPreviewPanels();
		void RefreshMaterialsForCompiledShader(
			const std::filesystem::path& shaderPath,
			bool bCompilationSucceeded = true,
			std::string_view compilationDiagnostics = {});
	private:
		bool OnKeyPressed(KeyPressedEvent& e);
		bool OnMouseScrolled(MouseScrolledEvent& e);
		bool OnMouseButtonPressed(MouseButtonPressedEvent& e);
		void OnViewportResize();
		void ApplyViewportMode(std::string_view modeId);
		[[nodiscard]] bool PrepareViewportModeForPlay();

        // imgui Layout load and save
        void UpdateSettings();

        void NewScene();
        void OpenScene();
        void OpenScene(const std::filesystem::path& path);
        void SaveScene();
        void SaveSceneAs();

		void SerializeScene(Ref<Level> scene, const std::filesystem::path& path);
		void OpenBuildPublish();

		void OnScenePlay(); 
		void OnSceneStop();
		void ProcessRuntimeSceneRequest();
		void TryChangeSceneMode(bool bPlay) noexcept;

		void OnDuplicateEntity();

		void UpdateRuntimeCursorPolicy();
		void SetRuntimeCursorCaptured(bool bCaptured);
		void CenterCursorToViewport();
		void CaptureSceneHistory(const char* label, bool bInteractive = false);
		void CommitPendingSceneHistory();

		// UI Panels
		void UI_Toolbar();
		void UI_ViewportMode();
		void UI_ViewportTransformTools();
		void UI_StatusBar();
		void UI_GraphicsApiSettings();
		void UI_GraphicsApiRestartPopup();
		void SetPreferredGraphicsApi(RenderBackend backend);
		void ResetSceneHistory(const char* label);
		void ResolveActiveUndoClient();
		[[nodiscard]] bool IsUndoClientAvailable(IEditorUndoClient* client) const;
		bool UndoActiveDocument();
		bool RedoActiveDocument();

		void LoadDefaultEditorConfig();
		void SaveDefaultEditorConfig();
    public:
        [[nodiscard]] bool CanUndo() const override;
        [[nodiscard]] bool CanRedo() const override;
        bool Undo() override;
        bool Redo() override;
        [[nodiscard]] const char* GetUndoContextName() const override { return EditorLayer::GetText("Scene"); }
        [[nodiscard]] bool IsUndoContextDirty() const override;
    public:
        template<typename PanelType>
        static PanelType* GetPanel()
        {
            HE_CORE_ASSERT(mPanelMap.count(PanelType::GetTypeName()), "Can not find this panel.")

            return (PanelType*)mPanelMap[PanelType::GetTypeName()].get();
        }
    public:
        Json mBaseEditorJson;
        EditorCamera mEditorCamera;
    private:
        Ref<Level> mActiveScene;
        Ref<Level> mEditorScene;
		ImGuiTextureHandle mMainViewportTexture;
		uint64_t mMainViewportOutputGeneration = 0;
		std::filesystem::path mEditorScenePath;
		std::filesystem::path mRuntimeScenePath;
        Entity mSquareEntity;
        Entity mCameraEntity;
        Entity mSecondCamera;

        bool mPrimaryCamera = true;

        bool bHandleEvent = false, bViewportHovered = false;

		glm::vec2 mViewportBounds[2];

        glm::vec4 mSquareColor = { 0.2f, 0.3f, 0.8f, 1.0f };

		int mGizmoType = -1;

        // Panels
        static std::unordered_map<std::string, Scope<BasePanel>> mPanelMap;
		std::vector<std::string> mPendingMaterialPaths;
		std::vector<std::string> mPendingAssetPreviewPaths;
		bool mIteratingPanelMap = false;
		Scope<MaterialThumbnailService> mMaterialThumbnailService;
		Scope<SceneThumbnailService> mSceneThumbnailService;
		DelegateHandle mShaderWatcherHandle;

        static Scope<class HIni> mTextIni;

		Scope<class BuildPublishPanel> mBuildPublishPanel;
		bool mRuntimeCursorCaptured = false;
		bool mRuntimeInputEngaged = false;
        ScriptPlaySession mScriptPlay;
        Json mPrePlayEditorState;
        std::filesystem::path mPendingScriptGeneration;
		bool mToolbarGridSnap = false;
		bool mToolbarUseLocalSpace = true;
		bool mToolbarPerspectiveView = true;
		bool mChangeDimensionPending = false;
		ViewportModeController mViewportModeController;
		std::string mViewportModeError;
		bool bViewGizmoHovered = false;
		bool bViewGizmoActive = false;
		Scope<SceneEditingService> mSceneEditing;
        EditorSceneMailbox mSceneMailbox;
		std::filesystem::path mPendingSceneOpenPath;
        bool mInvalidateClosedAssetPanels = false;
		struct PendingSceneEdit { std::string Ticket, Method; Json Request; bool LocalUi = false; };
		std::vector<PendingSceneEdit> mPendingSceneEdits;
		std::map<std::string, Json> mSceneEditResults;
        std::string mLastSceneEditError;
		bool mSceneHistoryInteractivePending = false;
		std::string mPendingSceneHistoryLabel;
		std::chrono::steady_clock::time_point mNextSceneObservation{};
		std::vector<EntityHandle> mSceneObservationHandles;
		std::unordered_set<std::string> mSceneObservationSeen;
		size_t mSceneObservationIndex = 0;
		bool mSceneDirty = false;
		uint64_t mEditorRenderRevision = 1;
		uint64_t mLastRenderedEditorRevision = 0;
		uint32_t mEditorWarmupFramesRemaining = 30;
		glm::mat4 mLastRenderedEditorView{ 0.0f };
		glm::mat4 mLastRenderedEditorProjection{ 0.0f };
		int mLastRenderedSelectedEntity = -2;
		int mLastRenderedDimensionMode = -1;
		bool mLastRenderedShowColliders = false;
		PhysicsDebugDrawModeFlag mLastRenderedPhysicsDebugMode = PhysicsDebugDrawModeFlag::None;
		bool mResetEditorDockLayout = false;
		RenderBackend mGraphicsApiTargetBackend = RenderBackend::DX12;
		RenderBackend mGraphicsApiActiveBackend = RenderBackend::DX12;
		std::string mGraphicsApiStatusMessage;
		bool mGraphicsApiChangeSucceeded = false;
		bool mOpenGraphicsApiStatusPopup = false;
		IEditorUndoClient* mActiveUndoClient = nullptr;
		IEditorUndoClient* mFocusedUndoClient = nullptr;
		IEditorUndoClient* mHoveredUndoClient = nullptr;
		IEditorUndoClient* mLastActiveUndoClient = nullptr;
    };
}

