#pragma once

#include "BasePanel.h"
#include "Editor/Preview/EditorOffscreenViewport.h"

#include "Runtime/Camera/EditorCamera.h"
#include "Runtime/Core/Timestep.h"
#include "Runtime/Core/Base/Base.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/Graphics/Renderer2D.h"
#include "Runtime/Resource/ResType/Animation.h"
#include "Runtime/Resource/ResType/Skeleton.h"

#include <imgui/imgui.h>

#include <filesystem>
#include <string>
#include <vector>

namespace HEngine
{
	class Event;
	class AnimationSystem;
	class RenderSystem3D;

	class AssetPreviewPanel : public BasePanel
	{
	public:
		enum class AssetKind
		{
			Mesh,
			Animation,
			Skeleton,
			Unknown
		};

		AssetPreviewPanel(EditorLayer* parent, std::string assetPath);
		~AssetPreviewPanel() override;

		void OnImGuiRender() override;
		void OnImGuiRenderImpl() override;
		void OnEvent(Event& e) override;

		static std::string BuildPanelKey(const std::string& assetPath);

	private:
		struct PreviewSkeletonSegment
		{
			int boneVectorIndex = -1;
			int parentBoneVectorIndex = -1;
			glm::vec3 position = glm::vec3(0.0f);
			glm::vec3 parentPosition = glm::vec3(0.0f);
			bool bHasParent = false;
		};

		void LoadAsset();
		void BuildPreviewScene();
		void BuildMeshPreview();
		void BuildAnimationPreview();
		void BuildSkeletonPreview();
		void RenderPreview(Timestep ts, const ImVec2& viewportSize);
		void DrawToolbar();
		void DrawViewport();
		void DrawDetails();
		void DrawAssetDetailsSection();
		void DrawPreviewDetailsSection();
		void DrawAnimationDetailsSection();
		void DrawSkeletonDetailsSection();
		void DrawSkeletonOverlay(const ImVec2& viewportMin, const ImVec2& viewportSize);
		void DrawSkeletonTreeNode(int boneVectorIndex, const std::vector<std::vector<int>>& childBoneIndices, const std::vector<bool>& visibleBoneMask);
		bool BuildPreviewSkeletonSegments(std::vector<PreviewSkeletonSegment>& outSegments, std::vector<int>* outParentVectorIndices = nullptr);
		void DrawFloorGrid();
		void FrameAsset();
		void UpdateAnimationPreviewPose(Timestep ts);
		void UpdatePreviewEntityPlacement();
		void UpdateSkeletonPreviewOffset();
		void ResetAnimationPreviewState();

		[[nodiscard]] AssetKind ResolveAssetKind() const;
		[[nodiscard]] bool IsRenderableAsset() const;
		[[nodiscard]] bool HasPreviewMesh();
		[[nodiscard]] std::string GetAssetKindName() const;
		[[nodiscard]] float GetSelectedClipDurationSeconds() const;
		[[nodiscard]] float GetSelectedClipCurrentSeconds() const;
		[[nodiscard]] glm::vec3 ComputePreviewCenter();
		[[nodiscard]] float ComputePreviewRadius();

	private:
		std::string mAssetPath;
		std::filesystem::path mAssetFullPath;
		AssetKind mAssetKind = AssetKind::Unknown;

		bool mOpen = true;
		bool mLoaded = false;
		bool mSceneDirty = true;
		bool mFirstOpen = true;
		bool mViewportHovered = false;
		bool mFrameRequested = true;

		EditorOffscreenViewport mOffscreenViewport;
		Ref<Level> mPreviewScene;
		Scope<AnimationSystem> mPreviewAnimationSystem;
		Scope<RenderSystem3D> mPreviewRenderSystem;
		RenderPrimitiveCollector mPrimitiveCollector;
		std::vector<RenderPrimitive2D> mPreviewPrimitives;
		Entity mPreviewEntity;
		EditorCamera mCamera;

		AnimRes mAnimRes;
		SkeletonRes mSkeletonRes;
		std::string mLoadError;
		std::string mPreviewNote;

		int mSelectedClipIndex = 0;
		int mPreviewClipIndex = -1;
		bool mPlayAnimation = false;
		bool mLoopAnimation = true;
		float mAnimationSpeed = 1.0f;
		float mAnimationTimeTicks = 0.0f;
		float mAnimationProgress = 0.0f;
		bool mShowGrid = true;
		bool mShowMesh = true;
		bool mShowSkeleton = false;
		float mPreviewRadius = 1.0f;
		glm::vec3 mPreviewCenter = glm::vec3(0.0f);
		glm::vec3 mPreviewContentOffset = glm::vec3(0.0f);
		int mSelectedBoneIndex = -1;
		char mSkeletonFilter[128] = {};
	};
}
