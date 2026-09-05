#include "AssetPreviewPanel.h"

#include "Editor/EditorTheme.h"
#include "Editor/EditorLayer.h"
#include "Editor/ImGuiWrapper/ImGuiWrapper.h"
#include "Runtime/EcsFramework/Component/Animation/AnimatorComponent.h"
#include "Runtime/EcsFramework/Component/Light/DirectionalLightComponent.h"
#include "Runtime/EcsFramework/Component/Light/EnvironmentLightComponent.h"
#include "Runtime/EcsFramework/Component/Light/PointLightComponent.h"
#include "Runtime/EcsFramework/Component/Mesh/MeshComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/System/Animation/AnimationSystem.h"
#include "Runtime/EcsFramework/System/Render/RenderSystem3D.h"
#include "Runtime/Events/Event.h"
#include "Runtime/Events/MouseEvent.h"
#include "Runtime/Graphics/Renderer2D.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/ModeManager/ModeManager.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"
#include "_generated/serializer/Animation.gen.h"

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <imgui/imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cmath>
#include <functional>
#include <limits>
#include <unordered_map>

namespace HEngine
{
	namespace
	{
		constexpr float kPreviewGridMinorStep = 10.0f;
		constexpr int kPreviewGridMajorEvery = 10;
		constexpr float kPreviewGridMinHalfExtent = 100.0f;
		constexpr float kPreviewGridMaxHalfExtent = 5000.0f;

		class ScopedPreviewConfigState final
		{
		public:
			ScopedPreviewConfigState(const glm::vec2& viewportSize, int selectedEntity)
				: mPreviousViewportSize(ConfigManager::mViewportSize),
				  mPreviousSelectedEntity(ConfigManager::selectedEntity)
			{
				ConfigManager::mViewportSize = viewportSize;
				ConfigManager::selectedEntity = selectedEntity;
			}

			~ScopedPreviewConfigState() noexcept
			{
				ConfigManager::mViewportSize = mPreviousViewportSize;
				ConfigManager::selectedEntity = mPreviousSelectedEntity;
			}

			ScopedPreviewConfigState(const ScopedPreviewConfigState&) = delete;
			ScopedPreviewConfigState& operator=(const ScopedPreviewConfigState&) = delete;

		private:
			glm::vec2 mPreviousViewportSize;
			int mPreviousSelectedEntity = -1;
		};

		static std::string LocalizedLabel(const char* key, const char* id)
		{
			return std::string(EditorLayer::GetText(key)) + "###" + id;
		}

		static std::string ToLower(std::string text)
		{
			std::transform(text.begin(), text.end(), text.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return text;
		}

		static float SnapUpToStep(float value, float step)
		{
			if (step <= 0.0f)
				return value;
			return std::ceil(value / step) * step;
		}

		static glm::mat4 BuildCameraFacingQuadTransform(const EditorCamera& camera, const glm::vec3& center, float halfWidth, float halfHeight)
		{
			glm::vec3 right = camera.GetRightDirection();
			glm::vec3 up = camera.GetUpDirection();
			glm::vec3 forward = -camera.GetForwardDirection();

			if (glm::dot(right, right) < 1e-6f)
				right = glm::vec3(1.0f, 0.0f, 0.0f);
			if (glm::dot(up, up) < 1e-6f)
				up = glm::vec3(0.0f, 1.0f, 0.0f);
			if (glm::dot(forward, forward) < 1e-6f)
				forward = glm::vec3(0.0f, 0.0f, 1.0f);

			right = glm::normalize(right);
			up = glm::normalize(up);
			forward = glm::normalize(forward);

			glm::mat4 basis(1.0f);
			basis[0] = glm::vec4(right, 0.0f);
			basis[1] = glm::vec4(up, 0.0f);
			basis[2] = glm::vec4(forward, 0.0f);

			return glm::translate(glm::mat4(1.0f), center)
				* basis
				* glm::scale(glm::mat4(1.0f), glm::vec3(halfWidth * 2.0f, halfHeight * 2.0f, 1.0f));
		}

		static glm::mat4 BuildBoneSegmentQuadTransform(const EditorCamera& camera, const glm::vec3& start, const glm::vec3& end, float halfWidth)
		{
			const glm::vec3 segment = end - start;
			const float length = glm::length(segment);
			const glm::vec3 center = (start + end) * 0.5f;
			if (length <= 1e-4f)
				return BuildCameraFacingQuadTransform(camera, center, halfWidth, halfWidth);

			const glm::vec3 direction = segment / length;
			glm::vec3 toCamera = camera.GetPosition() - center;
			if (glm::dot(toCamera, toCamera) < 1e-6f)
				toCamera = -camera.GetForwardDirection();

			glm::vec3 widthAxis = glm::cross(direction, glm::normalize(toCamera));
			if (glm::dot(widthAxis, widthAxis) < 1e-6f)
				widthAxis = glm::cross(direction, camera.GetUpDirection());
			if (glm::dot(widthAxis, widthAxis) < 1e-6f)
				widthAxis = glm::cross(direction, camera.GetRightDirection());
			if (glm::dot(widthAxis, widthAxis) < 1e-6f)
				widthAxis = glm::vec3(1.0f, 0.0f, 0.0f);
			widthAxis = glm::normalize(widthAxis);

			glm::vec3 normalAxis = glm::cross(widthAxis, direction);
			if (glm::dot(normalAxis, normalAxis) < 1e-6f)
				normalAxis = -camera.GetForwardDirection();
			if (glm::dot(normalAxis, normalAxis) < 1e-6f)
				normalAxis = glm::vec3(0.0f, 0.0f, 1.0f);
			normalAxis = glm::normalize(normalAxis);

			glm::mat4 basis(1.0f);
			basis[0] = glm::vec4(widthAxis, 0.0f);
			basis[1] = glm::vec4(direction, 0.0f);
			basis[2] = glm::vec4(normalAxis, 0.0f);

			return glm::translate(glm::mat4(1.0f), center)
				* basis
				* glm::scale(glm::mat4(1.0f), glm::vec3(halfWidth * 2.0f, length, 1.0f));
		}

		static ImVec2 AddImVec2(const ImVec2& a, const ImVec2& b)
		{
			return ImVec2(a.x + b.x, a.y + b.y);
		}

		static ImVec2 SubImVec2(const ImVec2& a, const ImVec2& b)
		{
			return ImVec2(a.x - b.x, a.y - b.y);
		}

		static ImVec2 MulImVec2(const ImVec2& value, float scalar)
		{
			return ImVec2(value.x * scalar, value.y * scalar);
		}

		static ImU32 ToImU32Color(const glm::vec4& color)
		{
			return IM_COL32(
				static_cast<int>(std::clamp(color.r, 0.0f, 1.0f) * 255.0f),
				static_cast<int>(std::clamp(color.g, 0.0f, 1.0f) * 255.0f),
				static_cast<int>(std::clamp(color.b, 0.0f, 1.0f) * 255.0f),
				static_cast<int>(std::clamp(color.a, 0.0f, 1.0f) * 255.0f));
		}

		static bool ProjectWorldToViewport(const EditorCamera& camera, const glm::vec3& worldPosition, const ImVec2& viewportMin, const ImVec2& viewportSize, ImVec2& outScreenPosition)
		{
			const glm::vec4 clipPosition = camera.GetViewProjection() * glm::vec4(worldPosition, 1.0f);
			if (clipPosition.w <= 1e-4f)
				return false;

			const glm::vec3 ndcPosition = glm::vec3(clipPosition) / clipPosition.w;
			if (ndcPosition.z < -1.2f || ndcPosition.z > 1.2f)
				return false;

			outScreenPosition.x = viewportMin.x + (ndcPosition.x * 0.5f + 0.5f) * viewportSize.x;
			outScreenPosition.y = viewportMin.y + (-ndcPosition.y * 0.5f + 0.5f) * viewportSize.y;
			return true;
		}

		static void DrawBoneConeOverlay(ImDrawList* drawList, const ImVec2& start, const ImVec2& end, float baseHalfWidth, float tipHalfWidth, ImU32 fillColor, ImU32 outlineColor, float outlineThickness)
		{
			const ImVec2 delta = SubImVec2(end, start);
			const float length = std::sqrt(delta.x * delta.x + delta.y * delta.y);
			if (length <= 1.0f)
				return;

			const ImVec2 direction(delta.x / length, delta.y / length);
			const ImVec2 normal(-direction.y, direction.x);
			const float tipLength = std::clamp(length * 0.32f, 8.0f, 22.0f);
			const ImVec2 neckCenter = SubImVec2(end, MulImVec2(direction, std::min(tipLength, std::max(length - 1.0f, 0.0f))));

			const ImVec2 points[5] = {
				SubImVec2(start, MulImVec2(normal, baseHalfWidth)),
				SubImVec2(neckCenter, MulImVec2(normal, tipHalfWidth)),
				end,
				AddImVec2(neckCenter, MulImVec2(normal, tipHalfWidth)),
				AddImVec2(start, MulImVec2(normal, baseHalfWidth))
			};

			drawList->AddConvexPolyFilled(points, 5, fillColor);
			drawList->AddPolyline(points, 5, outlineColor, ImDrawFlags_Closed, outlineThickness);
		}

		static void DrawJointOverlay(ImDrawList* drawList, const ImVec2& center, float radius, ImU32 fillColor, ImU32 outlineColor, float outlineThickness)
		{
			drawList->AddCircleFilled(center, radius, fillColor, 18);
			drawList->AddCircle(center, radius, outlineColor, 18, outlineThickness);
		}

		static glm::mat4 MakeBoneLocalTransform(const SkeletonBoneRes& bone)
		{
			return glm::translate(glm::mat4(1.0f), bone.localBindT)
				* glm::toMat4(bone.localBindR)
				* glm::scale(glm::mat4(1.0f), bone.localBindS);
		}

		static std::unordered_map<std::string, int> BuildBoneNameToVectorIndexMap(const SkeletonRes& skeleton)
		{
			std::unordered_map<std::string, int> boneNameToVectorIndex;
			boneNameToVectorIndex.reserve(skeleton.bones.size());
			for (size_t i = 0; i < skeleton.bones.size(); ++i)
				boneNameToVectorIndex.emplace(skeleton.bones[i].name, static_cast<int>(i));
			return boneNameToVectorIndex;
		}

		static std::vector<int> BuildSkeletonParentVectorIndices(const SkeletonRes& skeleton, const AnimNodeRes* hierarchyRoot = nullptr)
		{
			std::vector<int> parentVectorIndices(skeleton.bones.size(), -1);
			if (skeleton.bones.empty())
				return parentVectorIndices;

			std::unordered_map<int, int> boneIdToVectorIndex;
			boneIdToVectorIndex.reserve(skeleton.bones.size());
			for (size_t i = 0; i < skeleton.bones.size(); ++i)
				boneIdToVectorIndex.emplace(skeleton.bones[i].index, static_cast<int>(i));

			const auto boneNameToVectorIndex = BuildBoneNameToVectorIndexMap(skeleton);

			for (size_t i = 0; i < skeleton.bones.size(); ++i)
			{
				const int parentIndex = skeleton.bones[i].parentIndex;
				if (parentIndex < 0)
					continue;

				auto parentByBoneId = boneIdToVectorIndex.find(parentIndex);
				if (parentByBoneId != boneIdToVectorIndex.end())
					parentVectorIndices[i] = parentByBoneId->second;
				else if (parentIndex < static_cast<int>(skeleton.bones.size()))
					parentVectorIndices[i] = parentIndex;
			}

			if (!hierarchyRoot)
				return parentVectorIndices;

			std::function<void(const AnimNodeRes&, int)> visitNode = [&](const AnimNodeRes& node, int parentBoneVectorIndex)
			{
				int currentParentBoneIndex = parentBoneVectorIndex;

				auto currentBoneIt = boneNameToVectorIndex.find(node.name);
				if (currentBoneIt != boneNameToVectorIndex.end())
				{
					const int currentBoneIndex = currentBoneIt->second;
					parentVectorIndices[static_cast<size_t>(currentBoneIndex)] = parentBoneVectorIndex;
					currentParentBoneIndex = currentBoneIndex;
				}

				for (const AnimNodeRes& child : node.children)
					visitNode(child, currentParentBoneIndex);
			};

			visitNode(*hierarchyRoot, -1);

			return parentVectorIndices;
		}

		static std::vector<glm::mat4> BuildSkeletonWorldTransforms(const SkeletonRes& skeleton)
		{
			const size_t boneCount = skeleton.bones.size();
			const std::vector<int> parentVectorIndices = BuildSkeletonParentVectorIndices(skeleton);
			std::vector<glm::mat4> worldTransforms(boneCount, glm::mat4(1.0f));
			std::vector<bool> computed(boneCount, false);

			std::function<glm::mat4(size_t)> resolveBone = [&](size_t boneIndex) -> glm::mat4
			{
				if (computed[boneIndex])
					return worldTransforms[boneIndex];

				const SkeletonBoneRes& bone = skeleton.bones[boneIndex];
				glm::mat4 localTransform = MakeBoneLocalTransform(bone);
				const int parentBoneIndex = parentVectorIndices[boneIndex];
				if (parentBoneIndex >= 0 && parentBoneIndex < static_cast<int>(boneCount))
				{
					worldTransforms[boneIndex] = resolveBone(static_cast<size_t>(parentBoneIndex)) * localTransform;
				}
				else
				{
					worldTransforms[boneIndex] = localTransform;
				}

				computed[boneIndex] = true;
				return worldTransforms[boneIndex];
			};

			for (size_t i = 0; i < boneCount; ++i)
				resolveBone(i);

			return worldTransforms;
		}

		static bool TryBuildWorldTransformsFromAnimHierarchy(const AnimNodeRes& rootNode, const SkeletonRes& skeleton, std::vector<glm::mat4>& outWorldTransforms)
		{
			if (skeleton.bones.empty())
				return false;

			const auto boneNameToVectorIndex = BuildBoneNameToVectorIndexMap(skeleton);
			outWorldTransforms.assign(skeleton.bones.size(), glm::mat4(1.0f));
			bool bHasAnyBoneTransform = false;

			std::function<void(const AnimNodeRes&, const glm::mat4&)> visitNode = [&](const AnimNodeRes& node, const glm::mat4& parentWorld)
			{
				glm::mat4 localTransform = glm::mat4(1.0f);
				if (node.localTransform.size() == 16)
					localTransform = glm::make_mat4(node.localTransform.data());

				const glm::mat4 worldTransform = parentWorld * localTransform;

				auto currentBoneIt = boneNameToVectorIndex.find(node.name);
				if (currentBoneIt != boneNameToVectorIndex.end())
				{
					outWorldTransforms[static_cast<size_t>(currentBoneIt->second)] = worldTransform;
					bHasAnyBoneTransform = true;
				}

				for (const AnimNodeRes& child : node.children)
					visitNode(child, worldTransform);
			};

			visitNode(rootNode, glm::mat4(1.0f));
			return bHasAnyBoneTransform;
		}

		static bool TryLoadSkeletonAsset(const std::string& skeletonPath, SkeletonRes& outSkeleton, std::string& outError)
		{
			const std::string normalizedPath = Utils::NormalizeAssetPath(skeletonPath);
			if (normalizedPath.empty() || normalizedPath == "None")
				return false;

			const std::filesystem::path fullPath = ConfigManager::GetFullPath(normalizedPath);
			if (!std::filesystem::exists(fullPath))
			{
				outError = std::string(EditorLayer::GetText("Skeleton file does not exist:")) + " " + normalizedPath;
				return false;
			}

			std::string loadError;
			if (!LoadSkeletonRes(fullPath, outSkeleton, &loadError))
			{
				outError = loadError;
				return false;
			}

			return true;
		}

		static bool TryLoadAnimationAsset(
			const std::filesystem::path& searchRoot,
			const std::function<bool(const AnimRes&)>& matcher,
			AnimRes& outAnimRes)
		{
			if (searchRoot.empty() || !std::filesystem::exists(searchRoot))
				return false;

			for (const auto& entry : std::filesystem::recursive_directory_iterator(searchRoot))
			{
				if (!entry.is_regular_file() || entry.path().extension() != ".hanim")
					continue;

				try
				{
					AnimRes candidate;
					Serializer::Deserialize(Utils::LoadJsonFile(entry.path()), candidate);
					if (matcher(candidate))
					{
						outAnimRes = std::move(candidate);
						return true;
					}
				}
				catch (const std::exception&)
				{
				}
			}

			return false;
		}

		static bool TryLoadAnimationHierarchyForSkeleton(const std::filesystem::path& skeletonFullPath, const std::string& skeletonAssetPath, AnimRes& outAnimRes)
		{
			const std::string normalizedSkeletonPath = Utils::NormalizeAssetPath(skeletonAssetPath);
			if (normalizedSkeletonPath.empty() || normalizedSkeletonPath == "None")
				return false;

			const auto matchSkeleton = [&normalizedSkeletonPath](const AnimRes& candidate)
				{
					return Utils::IsSameAssetPath(Utils::NormalizeAssetPath(candidate.targetSkeletonPath), normalizedSkeletonPath);
				};

			const std::filesystem::path localSearchRoot = skeletonFullPath.parent_path();
			if (TryLoadAnimationAsset(localSearchRoot, matchSkeleton, outAnimRes))
				return true;

			const std::filesystem::path assetSearchRoot = ConfigManager::GetFullPath("Assets");
			if (assetSearchRoot != localSearchRoot && TryLoadAnimationAsset(assetSearchRoot, matchSkeleton, outAnimRes))
				return true;

			return false;
		}

		static bool TryLoadAnimationHierarchyForMesh(
			const std::filesystem::path& meshFullPath,
			const std::string& meshAssetPath,
			const std::string& skeletonAssetPath,
			AnimRes& outAnimRes)
		{
			const std::string normalizedMeshPath = Utils::NormalizeAssetPath(meshAssetPath);
			const std::string normalizedSkeletonPath = Utils::NormalizeAssetPath(skeletonAssetPath);

			if ((normalizedMeshPath.empty() || normalizedMeshPath == "None") &&
				(normalizedSkeletonPath.empty() || normalizedSkeletonPath == "None"))
			{
				return false;
			}

			const auto matchMeshOrSkeleton = [&normalizedMeshPath, &normalizedSkeletonPath](const AnimRes& candidate)
				{
					const std::string candidateSkeletonPath = Utils::NormalizeAssetPath(candidate.targetSkeletonPath);
					if (!normalizedSkeletonPath.empty() && normalizedSkeletonPath != "None" &&
						Utils::IsSameAssetPath(candidateSkeletonPath, normalizedSkeletonPath))
					{
						return true;
					}

					const std::string candidateMeshPath = Utils::NormalizeAssetPath(candidate.meshResPath);
					if (!normalizedMeshPath.empty() && normalizedMeshPath != "None" &&
						Utils::IsSameAssetPath(candidateMeshPath, normalizedMeshPath))
					{
						return true;
					}

					return false;
				};

			const std::filesystem::path localSearchRoot = meshFullPath.parent_path();
			if (TryLoadAnimationAsset(localSearchRoot, matchMeshOrSkeleton, outAnimRes))
				return true;

			const std::filesystem::path assetSearchRoot = ConfigManager::GetFullPath("Assets");
			if (assetSearchRoot != localSearchRoot && TryLoadAnimationAsset(assetSearchRoot, matchMeshOrSkeleton, outAnimRes))
				return true;

			return false;
		}

		static void ExpandBounds(glm::vec3& minBounds, glm::vec3& maxBounds, const glm::vec3& point)
		{
			minBounds = glm::min(minBounds, point);
			maxBounds = glm::max(maxBounds, point);
		}

		static float SafeRadiusFromBounds(const glm::vec3& minBounds, const glm::vec3& maxBounds)
		{
			const glm::vec3 extent = maxBounds - minBounds;
			const float radius = glm::length(extent) * 0.5f;
			return std::max(radius, 0.5f);
		}

		static const std::vector<glm::mat4>* GetActiveSkinningMatrices(const Mesh& mesh)
		{
			if (!mesh.bAnimated || !mesh.bPlayAnim)
				return nullptr;

			if (mesh.bUseExternalBoneMatrices && !mesh.mExternalBoneMatrices.empty())
				return &mesh.mExternalBoneMatrices;

			const std::vector<glm::mat4>& animatorMatrices = mesh.mAnimator.GetFinalBoneMatrices();
			return animatorMatrices.empty() ? nullptr : &animatorMatrices;
		}

		static glm::vec3 CalculateSkinnedPosition(const SkinnedVertex& vertex, const std::vector<glm::mat4>& boneMatrices)
		{
			glm::vec4 skinnedPosition(0.0f);
			bool bHasWeightedBone = false;

			for (int i = 0; i < MAX_BONE_INFLUENCE; ++i)
			{
				const int boneId = vertex.mBoneIDs[i];
				const float weight = vertex.mWeights[i];
				if (weight <= 0.0f || boneId < 0 || boneId >= static_cast<int>(boneMatrices.size()))
					continue;

				skinnedPosition += boneMatrices[static_cast<size_t>(boneId)] * glm::vec4(vertex.Pos, 1.0f) * weight;
				bHasWeightedBone = true;
			}

			if (!bHasWeightedBone)
				return vertex.Pos;

			if (std::abs(skinnedPosition.w) > 1e-6f)
				return glm::vec3(skinnedPosition) / skinnedPosition.w;

			return glm::vec3(skinnedPosition);
		}

		static bool TryBuildMeshBounds(const MeshComponent& meshComponent, const glm::mat4& worldTransform, glm::vec3& outMinBounds, glm::vec3& outMaxBounds)
		{
			if (!meshComponent.mMesh)
				return false;

			const std::vector<glm::mat4>* skinningMatrices = GetActiveSkinningMatrices(*meshComponent.mMesh);
			outMinBounds = glm::vec3(std::numeric_limits<float>::max());
			outMaxBounds = glm::vec3(std::numeric_limits<float>::lowest());
			bool bHasPoint = false;

			for (const auto& submesh : meshComponent.mMesh->mSubMeshes)
			{
				for (const StaticVertex& vertex : submesh->mStaticVertices)
				{
					ExpandBounds(outMinBounds, outMaxBounds, glm::vec3(worldTransform * glm::vec4(vertex.Pos, 1.0f)));
					bHasPoint = true;
				}
				for (const SkinnedVertex& vertex : submesh->mSkinnedVertices)
				{
					const glm::vec3 localPosition = skinningMatrices ? CalculateSkinnedPosition(vertex, *skinningMatrices) : vertex.Pos;
					ExpandBounds(outMinBounds, outMaxBounds, glm::vec3(worldTransform * glm::vec4(localPosition, 1.0f)));
					bHasPoint = true;
				}
			}

			return bHasPoint;
		}

		static bool TryBuildBoundsFromTransforms(const std::vector<glm::mat4>& transforms, glm::vec3& outMinBounds, glm::vec3& outMaxBounds)
		{
			if (transforms.empty())
				return false;

			outMinBounds = glm::vec3(std::numeric_limits<float>::max());
			outMaxBounds = glm::vec3(std::numeric_limits<float>::lowest());
			for (const glm::mat4& transform : transforms)
				ExpandBounds(outMinBounds, outMaxBounds, glm::vec3(transform[3]));
			return true;
		}

		static glm::vec3 BuildGroundedCenteringOffset(const glm::vec3& minBounds, const glm::vec3& maxBounds)
		{
			const glm::vec3 center = (minBounds + maxBounds) * 0.5f;
			return glm::vec3(-center.x, -minBounds.y, -center.z);
		}

		static int FindPreferredAnchorBoneIndex(const SkeletonRes& skeleton)
		{
			int rootBoneIndex = -1;
			for (size_t i = 0; i < skeleton.bones.size(); ++i)
			{
				const SkeletonBoneRes& bone = skeleton.bones[i];
				const std::string lowerName = ToLower(bone.name);
				if (lowerName.find("hips") != std::string::npos ||
					lowerName.find("pelvis") != std::string::npos ||
					lowerName.find("root") != std::string::npos)
				{
					return static_cast<int>(i);
				}

				if (rootBoneIndex < 0 && bone.parentIndex < 0)
					rootBoneIndex = static_cast<int>(i);
			}

			if (rootBoneIndex >= 0)
				return rootBoneIndex;
			return skeleton.bones.empty() ? -1 : 0;
		}

		static bool TryBuildAnimatedBoneWorldTransforms(Mesh& mesh, const SkeletonRes& skeleton, std::vector<glm::mat4>& outWorldTransforms, const AnimNodeRes* hierarchyRoot = nullptr)
		{
			const std::vector<glm::mat4>* skinningMatrices = GetActiveSkinningMatrices(mesh);
			if (!skinningMatrices || skinningMatrices->empty() || skeleton.bones.empty())
				return false;

			outWorldTransforms.assign(skeleton.bones.size(), glm::mat4(1.0f));
			std::vector<bool> hasAnimatedWorld(skeleton.bones.size(), false);
			bool bHasAnyTransform = false;

			const auto& boneInfoMap = mesh.GetBoneInfoMap();

			for (size_t i = 0; i < skeleton.bones.size(); ++i)
			{
				const SkeletonBoneRes& bone = skeleton.bones[i];
				auto boneInfoIt = boneInfoMap.find(bone.name);
				if (boneInfoIt == boneInfoMap.end())
					continue;

				const BoneInfo& boneInfo = boneInfoIt->second;
				const int boneMatrixIndex = boneInfo.id;
				if (boneMatrixIndex < 0 || boneMatrixIndex >= static_cast<int>(skinningMatrices->size()))
					continue;

				outWorldTransforms[i] = (*skinningMatrices)[boneMatrixIndex] * glm::inverse(boneInfo.offset);
				hasAnimatedWorld[i] = true;
				bHasAnyTransform = true;
			}

			if (!bHasAnyTransform)
				return false;

			const std::vector<int> parentVectorIndices = BuildSkeletonParentVectorIndices(skeleton, hierarchyRoot);
			std::vector<bool> resolved(skeleton.bones.size(), false);
			std::function<glm::mat4(size_t)> resolveBoneWorld = [&](size_t boneIndex) -> glm::mat4
			{
				if (resolved[boneIndex])
					return outWorldTransforms[boneIndex];

				if (hasAnimatedWorld[boneIndex])
				{
					resolved[boneIndex] = true;
					return outWorldTransforms[boneIndex];
				}

				const SkeletonBoneRes& bone = skeleton.bones[boneIndex];
				const glm::mat4 localBind = MakeBoneLocalTransform(bone);
				const int parentBoneVectorIndex = parentVectorIndices[boneIndex];
				if (parentBoneVectorIndex >= 0 && parentBoneVectorIndex < static_cast<int>(skeleton.bones.size()))
					outWorldTransforms[boneIndex] = resolveBoneWorld(static_cast<size_t>(parentBoneVectorIndex)) * localBind;
				else
					outWorldTransforms[boneIndex] = localBind;

				resolved[boneIndex] = true;
				return outWorldTransforms[boneIndex];
			};

			for (size_t i = 0; i < skeleton.bones.size(); ++i)
				resolveBoneWorld(i);

			return bHasAnyTransform;
		}
	}

	std::string AssetPreviewPanel::BuildPanelKey(const std::string& assetPath)
	{
		return std::string("AssetPreview:") + Utils::NormalizeAssetPath(assetPath);
	}

	AssetPreviewPanel::AssetPreviewPanel(EditorLayer* parent, std::string assetPath)
		: BasePanel(parent, BuildPanelKey(assetPath), &mOpen),
		  mAssetPath(Utils::NormalizeAssetPath(assetPath)),
		  mAssetFullPath(ConfigManager::GetFullPath(mAssetPath)),
		  mOffscreenViewport("Editor Asset Preview " + mAssetPath),
		  mCamera(45.0f, 16.0f / 9.0f, 0.05f, 1000.0f)
	{
		LoadAsset();
		BuildPreviewScene();
	}

	AssetPreviewPanel::~AssetPreviewPanel() = default;

	void AssetPreviewPanel::OnImGuiRender()
	{
		if (!(*pOpen))
		{
			mViewportHovered = false;
			mOffscreenViewport.Release();
			return;
		}

		const std::string title = GetAssetKindName() + " " + EditorLayer::GetText("Preview") + " - " + mAssetFullPath.filename().string() + "###" + BuildPanelKey(mAssetPath);
		ImGuiWrapper::SetNextWindowSizeConstraints(ImVec2(760.0f, 480.0f), ImVec2(FLT_MAX, FLT_MAX));
		if (mFirstOpen)
		{
			ImGuiWrapper::SetNextWindowSize(ImVec2(1080.0f, 720.0f), ImGuiCond_Once);
			mFirstOpen = false;
		}

		if (!ImGui::Begin(title.c_str(), pOpen))
		{
			ImGui::End();
			mViewportHovered = false;
			if (!(*pOpen))
				mOffscreenViewport.Release();
			return;
		}

		OnImGuiRenderImpl();
		ImGui::End();
		if (!(*pOpen))
		{
			mViewportHovered = false;
			mOffscreenViewport.Release();
		}
	}

	void AssetPreviewPanel::OnImGuiRenderImpl()
	{
		if (mSceneDirty)
			BuildPreviewScene();

		DrawToolbar();
		ImGui::Separator();

		if (ImGuiWrapper::BeginResizableSplit("AssetPreviewMainSplit", 2.6f, 1.0f, ImGuiTableFlags_BordersInnerV))
		{
			DrawViewport();

			ImGui::TableSetColumnIndex(1);
			DrawDetails();

			ImGuiWrapper::EndResizableSplit();
		}
	}

	void AssetPreviewPanel::OnEvent(Event& e)
	{
		if (!mViewportHovered)
			return;

		EventDispatcher dispatcher(e);
		dispatcher.Dispatch<MouseScrolledEvent>([this](MouseScrolledEvent& event)
			{
				mCamera.OnEvent(event);
				return true;
			});
		dispatcher.Dispatch<MouseButtonPressedEvent>([](MouseButtonPressedEvent& event)
			{
				(void)event;
				return true;
			});
		dispatcher.Dispatch<MouseButtonReleasedEvent>([](MouseButtonReleasedEvent& event)
			{
				(void)event;
				return true;
			});
	}

	void AssetPreviewPanel::LoadAsset()
	{
		mAssetKind = ResolveAssetKind();
		mLoaded = false;
		mLoadError.clear();
		mPreviewNote.clear();
		mAnimRes = {};
		mSkeletonRes = {};
		mPreviewClipIndex = -1;
		mAnimationTimeTicks = 0.0f;
		mAnimationProgress = 0.0f;
		mPlayAnimation = false;
		mSelectedBoneIndex = -1;

		if (mAssetKind == AssetKind::Unknown)
		{
			mLoadError = std::string(EditorLayer::GetText("Unsupported preview asset:")) + " " + mAssetPath;
			return;
		}

		if (!std::filesystem::exists(mAssetFullPath))
		{
			mLoadError = std::string(EditorLayer::GetText("Asset file does not exist:")) + " " + mAssetFullPath.string();
			return;
		}

		try
		{
			if (mAssetKind == AssetKind::Animation)
			{
				Serializer::Deserialize(Utils::LoadJsonFile(mAssetFullPath), mAnimRes);
				if (mAnimRes.clips.empty())
					mPreviewNote = EditorLayer::GetText("Animation has no clips.");
			}
			else if (mAssetKind == AssetKind::Skeleton)
			{
				std::string error;
				if (!LoadSkeletonRes(mAssetFullPath, mSkeletonRes, &error))
				{
					mLoadError = error;
					return;
				}

				AnimRes hierarchySource;
				if (TryLoadAnimationHierarchyForSkeleton(mAssetFullPath, mAssetPath, hierarchySource))
				{
					mAnimRes = std::move(hierarchySource);
					mPreviewNote = EditorLayer::GetText("Skeleton preview using hierarchy from a matching .hanim asset.");
				}
			}
		}
		catch (const std::exception& ex)
		{
			mLoadError = ex.what();
			return;
		}

		mLoaded = true;
	}

	void AssetPreviewPanel::BuildPreviewScene()
	{
		const bool bPrevious3DMode = ModeManager::b3DMode;
		ModeManager::b3DMode = true;
		mPreviewScene = CreateRef<Level>();
		ModeManager::b3DMode = bPrevious3DMode;
		mPreviewAnimationSystem = CreateScope<AnimationSystem>(mPreviewScene.get());
		mPreviewRenderSystem = CreateScope<RenderSystem3D>(mPreviewScene.get());

		mPreviewEntity = {};
		mPreviewContentOffset = glm::vec3(0.0f);

		Entity environmentLight = mPreviewScene->CreateEntity("Preview Studio Environment");
		auto& environment = environmentLight.AddComponent<EnvironmentLightComponent>();
		environment.HdrTexturePath = "Assets/Textures/Hdr/Default.hdr";
		environment.Exposure = 0.9f;
		environment.IBLIntensity = 1.15f;

		Entity directionalLight = mPreviewScene->CreateEntity("Preview Key Light");
		directionalLight.GetComponent<TransformComponent>().Rotation = glm::radians(glm::vec3(48.0f, -35.0f, -20.0f));
		auto& dirLight = directionalLight.AddComponent<DirectionalLightComponent>(2.6f, glm::vec3(1.0f, 0.96f, 0.88f));
		dirLight.bCastShadows = true;
		dirLight.ShadowMapSize = 1024;

		if (mLoaded)
		{
			if (mAssetKind == AssetKind::Mesh)
				BuildMeshPreview();
			else if (mAssetKind == AssetKind::Animation)
				BuildAnimationPreview();
			else if (mAssetKind == AssetKind::Skeleton)
				BuildSkeletonPreview();
		}

		if ((mAssetKind == AssetKind::Skeleton || !mPreviewEntity) && !mSkeletonRes.bones.empty())
			UpdateSkeletonPreviewOffset();
		else
			UpdatePreviewEntityPlacement();

		mPreviewCenter = ComputePreviewCenter();
		mPreviewRadius = ComputePreviewRadius();

		const float studioScale = std::max(mPreviewRadius, 1.0f);
		Entity fillLight = mPreviewScene->CreateEntity("Preview Fill Light");
		fillLight.GetComponent<TransformComponent>().Translation =
			mPreviewCenter + glm::vec3(-2.0f, 1.5f, 2.0f) * studioScale;
		fillLight.AddComponent<PointLightComponent>(
			3.0f * studioScale * studioScale,
			glm::vec3(0.55f, 0.68f, 1.0f),
			6.0f * studioScale,
			2.0f);

		mFrameRequested = true;
		mSceneDirty = false;
	}

	void AssetPreviewPanel::BuildMeshPreview()
	{
		mPreviewEntity = mPreviewScene->CreateEntity(mAssetFullPath.stem().string());
		auto& meshComponent = mPreviewEntity.AddComponent<MeshComponent>(mAssetPath);
		meshComponent.meshResPath = mAssetPath;

		std::string skeletonError;
		const std::string skeletonAssetPath = meshComponent.mMesh ? Utils::NormalizeAssetPath(meshComponent.mMesh->mSkeletonPath) : std::string();
		if (meshComponent.mMesh)
		{
			if (!TryLoadSkeletonAsset(skeletonAssetPath, mSkeletonRes, skeletonError) && !skeletonError.empty())
				mPreviewNote = skeletonError;
		}

		AnimRes hierarchySource;
		if (TryLoadAnimationHierarchyForMesh(mAssetFullPath, mAssetPath, skeletonAssetPath, hierarchySource))
		{
			mAnimRes = std::move(hierarchySource);
			if (mPreviewNote.empty())
				mPreviewNote = EditorLayer::GetText("Mesh preview using hierarchy from a matching .hanim asset.");
		}
	}

	void AssetPreviewPanel::BuildAnimationPreview()
	{
		mShowSkeleton = true;
		mSelectedClipIndex = std::clamp(mSelectedClipIndex, 0, std::max(0, static_cast<int>(mAnimRes.clips.size()) - 1));

		std::string skeletonError;
		if (!TryLoadSkeletonAsset(mAnimRes.targetSkeletonPath, mSkeletonRes, skeletonError) && !skeletonError.empty())
			mPreviewNote = skeletonError;

		const std::string meshPath = Utils::NormalizeAssetPath(mAnimRes.meshResPath);
		if (meshPath.empty() || meshPath == "None")
		{
			if (mPreviewNote.empty())
				mPreviewNote = EditorLayer::GetText("Animation has no meshResPath; showing skeleton fallback.");
			mShowSkeleton = true;
			return;
		}

		const std::filesystem::path meshFullPath = ConfigManager::GetFullPath(meshPath);
		if (!std::filesystem::exists(meshFullPath))
		{
			mPreviewNote = std::string(EditorLayer::GetText("Animation mesh does not exist:")) + " " + meshPath;
			mShowSkeleton = true;
			return;
		}

		mPreviewEntity = mPreviewScene->CreateEntity(mAssetFullPath.stem().string());
		auto& meshComponent = mPreviewEntity.AddComponent<MeshComponent>(meshPath);
		meshComponent.meshResPath = meshPath;

		auto& animator = mPreviewEntity.AddComponent<AnimatorComponent>(mAssetPath);
		animator.controllerPath = "None";
		animator.currentClipIndex = mSelectedClipIndex;
		// Keep the preview animator on the regular clip-loading path. The preview
		// timeline samples the pose manually, so a zero play speed prevents the
		// animation system from advancing it independently.
		animator.bPlay = true;
		animator.bLoop = mLoopAnimation;
		animator.playSpeed = 0.0f;
	}

	void AssetPreviewPanel::BuildSkeletonPreview()
	{
		mShowSkeleton = true;
	}

	void AssetPreviewPanel::RenderPreview(Timestep ts, const ImVec2& viewportSize)
	{
		const uint32_t width = static_cast<uint32_t>(std::max(1.0f, viewportSize.x));
		const uint32_t height = static_cast<uint32_t>(std::max(1.0f, viewportSize.y));
		mOffscreenViewport.Activate(width, height);

		if (mPreviewEntity && mPreviewEntity.HasComponent<AnimatorComponent>())
		{
			auto& animator = mPreviewEntity.GetComponent<AnimatorComponent>();
			const int clipCount = static_cast<int>(mAnimRes.clips.size());
			animator.currentClipIndex = clipCount > 0 ? std::clamp(mSelectedClipIndex, 0, clipCount - 1) : 0;
			animator.bPlay = true;
			animator.bLoop = mLoopAnimation;
			animator.playSpeed = 0.0f;
		}

		const ScopedPreviewConfigState previewConfig(
			glm::vec2(static_cast<float>(width), static_cast<float>(height)),
			-1);

		mCamera.SetViewportSize(static_cast<float>(width), static_cast<float>(height));
		if (mViewportHovered)
			mCamera.OnUpdate(ts);

		if (mAssetKind == AssetKind::Animation && mPreviewEntity && mPreviewEntity.HasComponent<AnimatorComponent>())
		{
			auto& animator = mPreviewEntity.GetComponent<AnimatorComponent>();
			if (!animator.bLoaded && mPreviewAnimationSystem)
				mPreviewAnimationSystem->OnUpdateEditor(Timestep(0.0f), mCamera);
			UpdateAnimationPreviewPose(ts);
		}
		else if (mPreviewAnimationSystem)
		{
			mPreviewAnimationSystem->OnUpdateEditor(ts, mCamera);
		}


		mPrimitiveCollector.BeginFrame();
		try
		{
			Renderer2D::CollectorScope collectorScope(mPrimitiveCollector);
			if (mShowGrid)
			{
				Renderer2D::BeginScene(
					mCamera,
					RenderPrimitive2DCategory::Editor,
					RenderPrimitiveDepthMode::DepthTest);
				DrawFloorGrid();
				Renderer2D::EndScene();
			}

			if (mShowMesh && mPreviewRenderSystem && IsRenderableAsset())
			{
				// RenderSystem3D moves all preview primitives into its immutable
				// RenderWorld together with the preview mesh extraction.
				mPreviewRenderSystem->OnUpdateEditor(ts, mCamera);
				Ref<const RenderWorld> previewWorld =
					mPreviewRenderSystem->AcquireLatestRenderWorld();
				mOffscreenViewport.Submit(std::move(previewWorld));
				mPreviewPrimitives.clear();
			}
			else
			{
				RenderPrimitive2DFrame primitiveFrame = Renderer2D::TakeFrameSnapshot();
				mPreviewPrimitives = primitiveFrame.Primitives;
				Ref<RenderWorld> previewWorld = CreateRef<RenderWorld>();
				previewWorld->Camera = primitiveFrame.Camera;
				previewWorld->Primitives2D = std::move(primitiveFrame.Primitives);
				previewWorld->Environment.HdrTexturePath = "Assets/Textures/Hdr/Default.hdr";
				previewWorld->Environment.Exposure = 0.9f;
				previewWorld->PostProcess.Exposure = 0.9f;
				previewWorld->Environment.IblIntensity = 1.15f;
				previewWorld->bEditorView = true;
				mOffscreenViewport.Submit(std::move(previewWorld));
			}

			mPrimitiveCollector.EndFrame();
		}
		catch (...)
		{
			mPrimitiveCollector.CancelFrame();
			throw;
		}
	}

	void AssetPreviewPanel::DrawToolbar()
	{
		if (ImGui::Button(LocalizedLabel("Frame", "AssetPreviewFrame").c_str(), ImGuiWrapper::S(78.0f, 0.0f)))
		{
			FrameAsset();
			mFrameRequested = false;
		}

		ImGuiWrapper::SameLine(0.0f, 8.0f);
		ImGui::Checkbox(LocalizedLabel("Grid", "AssetPreviewGrid").c_str(), &mShowGrid);

		const bool bHasPreviewMesh = HasPreviewMesh();
		const bool bHasPreviewSkeleton = !mSkeletonRes.bones.empty();

		if (bHasPreviewMesh)
		{
			ImGuiWrapper::SameLine(0.0f, 8.0f);
			ImGui::Checkbox(LocalizedLabel("Mesh", "AssetPreviewMesh").c_str(), &mShowMesh);
		}

		if (bHasPreviewSkeleton)
		{
			ImGuiWrapper::SameLine(0.0f, 8.0f);
			ImGui::Checkbox(LocalizedLabel("Skeleton", "AssetPreviewSkeleton").c_str(), &mShowSkeleton);
		}

		if (bHasPreviewMesh && bHasPreviewSkeleton)
		{
			ImGuiWrapper::SameLine(0.0f, 12.0f);
			if (ImGui::Button(LocalizedLabel("Mesh + Skeleton", "AssetPreviewMeshSkeleton").c_str(), ImGuiWrapper::S(132.0f, 0.0f)))
			{
				mShowMesh = true;
				mShowSkeleton = true;
			}

			ImGuiWrapper::SameLine(0.0f, 6.0f);
			if (ImGui::Button(LocalizedLabel("Skeleton Only", "AssetPreviewSkeletonOnly").c_str(), ImGuiWrapper::S(118.0f, 0.0f)))
			{
				mShowMesh = false;
				mShowSkeleton = true;
			}
		}

		if (mAssetKind == AssetKind::Animation)
		{
			const bool bCanPreviewClip = bHasPreviewMesh;
			ImGui::BeginDisabled(!bCanPreviewClip);

			ImGuiWrapper::SameLine(0.0f, 16.0f);
			if (ImGui::Button(LocalizedLabel(mPlayAnimation ? "Pause" : "Play", "AssetPreviewPlayPause").c_str(), ImGuiWrapper::S(78.0f, 0.0f)))
				mPlayAnimation = !mPlayAnimation;

			ImGuiWrapper::SameLine(0.0f, 8.0f);
			if (ImGui::Button(LocalizedLabel("Reset", "AssetPreviewReset").c_str(), ImGuiWrapper::S(78.0f, 0.0f)))
				ResetAnimationPreviewState();

			ImGuiWrapper::SameLine(0.0f, 8.0f);
			ImGui::Checkbox(LocalizedLabel("Loop", "AssetPreviewLoop").c_str(), &mLoopAnimation);

			ImGuiWrapper::SameLine(0.0f, 12.0f);
			ImGui::SetNextItemWidth(ImGuiWrapper::S(150.0f));
			ImGui::SliderFloat(LocalizedLabel("Speed", "AssetPreviewSpeed").c_str(), &mAnimationSpeed, 0.05f, 4.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);

			const std::array<float, 5> speedPresets = { 0.25f, 0.50f, 1.00f, 2.00f, 4.00f };
			for (float preset : speedPresets)
			{
				ImGuiWrapper::SameLine(0.0f, 6.0f);
				char label[16] = {};
				snprintf(label, sizeof(label), "%.2fx", preset);
				if (ImGui::SmallButton(label))
					mAnimationSpeed = preset;
			}

			if (!mAnimRes.clips.empty())
			{
				mSelectedClipIndex = std::clamp(mSelectedClipIndex, 0, static_cast<int>(mAnimRes.clips.size()) - 1);
				ImGuiWrapper::SameLine(0.0f, 12.0f);
				ImGui::SetNextItemWidth(ImGuiWrapper::S(220.0f));
				const std::string currentClipName = mAnimRes.clips[mSelectedClipIndex].name.empty()
					? std::string(EditorLayer::GetText("Clip")) + " " + std::to_string(mSelectedClipIndex)
					: mAnimRes.clips[mSelectedClipIndex].name;
				if (ImGui::BeginCombo(LocalizedLabel("Clip", "AssetPreviewClip").c_str(), currentClipName.c_str()))
				{
					for (int i = 0; i < static_cast<int>(mAnimRes.clips.size()); ++i)
					{
						const std::string clipName = mAnimRes.clips[i].name.empty()
							? std::string(EditorLayer::GetText("Clip")) + " " + std::to_string(i)
							: mAnimRes.clips[i].name;
						const bool bSelected = i == mSelectedClipIndex;
						if (ImGui::Selectable(clipName.c_str(), bSelected))
						{
							mSelectedClipIndex = i;
							ResetAnimationPreviewState();
						}
						if (bSelected)
							ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
			}

			ImGui::EndDisabled();
		}
	}

	void AssetPreviewPanel::DrawViewport()
	{
		ImVec2 viewportSize = ImGui::GetContentRegionAvail();
		viewportSize.x = std::max(viewportSize.x, ImGuiWrapper::S(64.0f));
		viewportSize.y = std::max(viewportSize.y, ImGuiWrapper::S(64.0f));

		const Timestep frameTs(ImGui::GetIO().DeltaTime);
		if (mFrameRequested)
		{
			RenderPreview(Timestep(0.0f), viewportSize);
			FrameAsset();
			mFrameRequested = false;
		}

		RenderPreview(frameTs, viewportSize);

		const ImGuiTextureHandle offscreenTexture = mOffscreenViewport.GetImGuiTexture();
		if (offscreenTexture)
		{
			ImGui::Image(NvrhiImGuiRenderer::ToImTextureID(offscreenTexture), viewportSize);
			if (mShowSkeleton && !mSkeletonRes.bones.empty())
				DrawSkeletonOverlay(ImGui::GetItemRectMin(), viewportSize);
			mViewportHovered = ImGui::IsItemHovered();
		}
		else
		{
			ImGui::Dummy(viewportSize);
			mViewportHovered = ImGui::IsItemHovered();
		}
	}

	void AssetPreviewPanel::DrawDetails()
	{
		if (ImGui::BeginChild("##AssetPreviewInspector", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_AlwaysVerticalScrollbar))
		{
			if (!mLoadError.empty())
			{
				DrawAssetDetailsSection();
				ImGui::Spacing();
				ImGui::TextColored(ImVec4(0.95f, 0.30f, 0.25f, 1.0f), "%s", mLoadError.c_str());
				ImGui::EndChild();
				return;
			}

			if (!mSkeletonRes.bones.empty())
			{
				DrawSkeletonDetailsSection();
				ImGui::Spacing();
			}

			DrawAssetDetailsSection();

			if (!mPreviewNote.empty())
			{
				ImGui::Spacing();
				ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.35f, 1.0f), "%s", mPreviewNote.c_str());
			}

			ImGui::Spacing();
			DrawPreviewDetailsSection();

			if (mAssetKind == AssetKind::Animation)
			{
				ImGui::Spacing();
				DrawAnimationDetailsSection();
			}
		}
		ImGui::EndChild();
	}

	void AssetPreviewPanel::DrawAssetDetailsSection()
	{
		EditorTheme::DrawSectionHeader(EditorLayer::GetText("Asset"), mAssetFullPath.filename().string().c_str());
		ImGui::Dummy(ImGuiWrapper::S(0.0f, 6.0f));

		EditorTheme::DrawPill(GetAssetKindName().c_str(), ImVec4(0.24f, 0.40f, 0.67f, 0.90f));
		if (mAssetKind == AssetKind::Animation)
		{
			ImGuiWrapper::SameLine(0.0f, 8.0f);
			EditorTheme::DrawPill(EditorLayer::GetText(mPlayAnimation ? "Playing" : "Paused"),
				mPlayAnimation ? ImVec4(0.20f, 0.56f, 0.33f, 0.90f) : ImVec4(0.38f, 0.40f, 0.44f, 0.90f));
		}

		ImGui::Spacing();
		ImGuiWrapper::PropertyRow(EditorLayer::GetText("Path"), [this]() { ImGui::TextWrapped("%s", mAssetPath.c_str()); });
		ImGuiWrapper::PropertyRow(EditorLayer::GetText("Renderable"), [this]() { ImGui::TextUnformatted(EditorLayer::GetText(IsRenderableAsset() ? "Yes" : "No")); });
	}

	void AssetPreviewPanel::DrawPreviewDetailsSection()
	{
		EditorTheme::DrawSectionHeader(EditorLayer::GetText("Preview"));
		ImGui::Dummy(ImGuiWrapper::S(0.0f, 6.0f));

		ImGuiWrapper::PropertyRow(EditorLayer::GetText("Radius"), [this]() { ImGui::Text("%.2f", mPreviewRadius); });
		ImGuiWrapper::PropertyRow(EditorLayer::GetText("Center"), [this]() { ImGui::Text("%.2f, %.2f, %.2f", mPreviewCenter.x, mPreviewCenter.y, mPreviewCenter.z); });
		ImGuiWrapper::PropertyRow(EditorLayer::GetText("Grid"), [this]() { ImGui::TextUnformatted(EditorLayer::GetText(mShowGrid ? "Visible" : "Hidden")); });
		ImGuiWrapper::PropertyRow(EditorLayer::GetText("Mesh"), [this]() { ImGui::TextUnformatted(EditorLayer::GetText(HasPreviewMesh() ? (mShowMesh ? "Visible" : "Hidden") : "Unavailable")); });
		ImGuiWrapper::PropertyRow(EditorLayer::GetText("Grid Units"), []() { ImGui::TextUnformatted(EditorLayer::GetText("10u minor / 100u major")); });
		ImGuiWrapper::PropertyRow(EditorLayer::GetText("Skeleton"), [this]() { ImGui::TextUnformatted(EditorLayer::GetText(mShowSkeleton ? "Visible" : "Hidden")); });

		if (mPreviewEntity && mPreviewEntity.HasComponent<MeshComponent>())
		{
			auto& meshComponent = mPreviewEntity.GetComponent<MeshComponent>();
			if (meshComponent.mMesh)
			{
				ImGuiWrapper::PropertyRow(EditorLayer::GetText("Submeshes"), [&meshComponent]() { ImGui::Text("%zu", meshComponent.mMesh->mSubMeshes.size()); });
				ImGuiWrapper::PropertyRow(EditorLayer::GetText("Materials"), [&meshComponent]() { ImGui::Text("%zu", meshComponent.mMesh->mMaterial.size()); });
				ImGuiWrapper::PropertyRow(EditorLayer::GetText("Bones"), [&meshComponent]() { ImGui::Text("%d", meshComponent.mMesh->GetBoneCount()); });
			}
		}
	}

	void AssetPreviewPanel::DrawAnimationDetailsSection()
	{
		EditorTheme::DrawSectionHeader(EditorLayer::GetText("Animation"));
		ImGui::Dummy(ImGuiWrapper::S(0.0f, 6.0f));

		ImGuiWrapper::PropertyRow(EditorLayer::GetText("Clips"), [this]() { ImGui::Text("%zu", mAnimRes.clips.size()); });
		ImGuiWrapper::PropertyRow(EditorLayer::GetText("Mesh"), [this]() { ImGui::TextWrapped("%s", mAnimRes.meshResPath.empty() ? EditorLayer::GetText("None") : mAnimRes.meshResPath.c_str()); });
		ImGuiWrapper::PropertyRow(EditorLayer::GetText("Skeleton"), [this]() { ImGui::TextWrapped("%s", mAnimRes.targetSkeletonPath.empty() ? EditorLayer::GetText("None") : mAnimRes.targetSkeletonPath.c_str()); });
		ImGuiWrapper::PropertyRow(EditorLayer::GetText("Speed"), [this]() { ImGui::Text("%.2fx", mAnimationSpeed); });

		if (!HasPreviewMesh())
		{
			ImGui::TextDisabled("%s", EditorLayer::GetText("Current .hanim preview has no renderable mesh target."));
			return;
		}

		if (mAnimRes.clips.empty())
			return;

		mSelectedClipIndex = std::clamp(mSelectedClipIndex, 0, static_cast<int>(mAnimRes.clips.size()) - 1);
		const AnimClipRes& clip = mAnimRes.clips[static_cast<size_t>(mSelectedClipIndex)];
		ImGuiWrapper::PropertyRow(EditorLayer::GetText("Clip"), [&clip, this]()
			{
				const std::string clipName = clip.name.empty() ? std::string(EditorLayer::GetText("Clip")) + " " + std::to_string(mSelectedClipIndex) : clip.name;
				ImGui::TextWrapped("%s", clipName.c_str());
			});
		ImGuiWrapper::PropertyRow(EditorLayer::GetText("Duration"), [this]() { ImGui::Text("%.2fs", GetSelectedClipDurationSeconds()); });
		ImGuiWrapper::PropertyRow(EditorLayer::GetText("Time"), [this]() { ImGui::Text("%.2fs", GetSelectedClipCurrentSeconds()); });
		ImGuiWrapper::PropertyRow(EditorLayer::GetText("Ticks/s"), [&clip]() { ImGui::Text("%.2f", clip.ticksPerSecond); });
		ImGuiWrapper::PropertyRow(EditorLayer::GetText("Channels"), [&clip]() { ImGui::Text("%zu", clip.channels.size()); });

		float normalizedTime = mAnimationProgress;
		if (ImGui::SliderFloat(LocalizedLabel("Timeline", "AssetPreviewTimeline").c_str(), &normalizedTime, 0.0f, 1.0f, "%.3f"))
		{
			mPlayAnimation = false;
			if (clip.duration > 0.0f)
			{
				mAnimationProgress = std::clamp(normalizedTime, 0.0f, 1.0f);
				mAnimationTimeTicks = clip.duration * mAnimationProgress;
			}
			else
			{
				mAnimationProgress = 0.0f;
				mAnimationTimeTicks = 0.0f;
			}
		}
	}

	void AssetPreviewPanel::DrawSkeletonDetailsSection()
	{
		const char* selectedBoneName = nullptr;
		if (mSelectedBoneIndex >= 0 && mSelectedBoneIndex < static_cast<int>(mSkeletonRes.bones.size()))
			selectedBoneName = mSkeletonRes.bones[static_cast<size_t>(mSelectedBoneIndex)].name.c_str();

		EditorTheme::DrawSectionHeader(EditorLayer::GetText("Skeleton"), selectedBoneName);
		ImGui::Dummy(ImGuiWrapper::S(0.0f, 6.0f));

		ImGuiWrapper::PropertyRow(EditorLayer::GetText("Bones"), [this]() { ImGui::Text("%zu", mSkeletonRes.bones.size()); });
		ImGuiWrapper::PropertyRow(EditorLayer::GetText("Display"), [this]()
			{
				if (mShowMesh && mShowSkeleton)
					ImGui::TextUnformatted(EditorLayer::GetText("Mesh + Skeleton"));
				else if (mShowMesh)
					ImGui::TextUnformatted(EditorLayer::GetText("Mesh Only"));
				else if (mShowSkeleton)
					ImGui::TextUnformatted(EditorLayer::GetText("Skeleton Only"));
				else
					ImGui::TextUnformatted(EditorLayer::GetText("Hidden"));
			});

		ImGui::InputTextWithHint("##SkeletonFilter", EditorLayer::GetText("Filter bones"), mSkeletonFilter, IM_ARRAYSIZE(mSkeletonFilter));

		std::vector<int> parentVectorIndices = BuildSkeletonParentVectorIndices(mSkeletonRes, &mAnimRes.rootNode);
		std::vector<std::vector<int>> childBoneIndices(mSkeletonRes.bones.size());
		std::vector<int> rootBoneIndices;
		rootBoneIndices.reserve(mSkeletonRes.bones.size());
		for (size_t i = 0; i < mSkeletonRes.bones.size(); ++i)
		{
			const int parentBoneVectorIndex = parentVectorIndices[i];
			if (parentBoneVectorIndex >= 0 && parentBoneVectorIndex < static_cast<int>(childBoneIndices.size()))
				childBoneIndices[static_cast<size_t>(parentBoneVectorIndex)].push_back(static_cast<int>(i));
			else
				rootBoneIndices.push_back(static_cast<int>(i));
		}

		const std::string filterText = ToLower(std::string(mSkeletonFilter));
		std::vector<bool> visibleBoneMask(mSkeletonRes.bones.size(), filterText.empty());
		if (!filterText.empty())
		{
			std::function<bool(int)> markVisible = [&](int boneVectorIndex) -> bool
			{
				bool bVisible = ToLower(mSkeletonRes.bones[static_cast<size_t>(boneVectorIndex)].name).find(filterText) != std::string::npos;
				for (int childBoneVectorIndex : childBoneIndices[static_cast<size_t>(boneVectorIndex)])
					bVisible = markVisible(childBoneVectorIndex) || bVisible;

				visibleBoneMask[static_cast<size_t>(boneVectorIndex)] = bVisible;
				return bVisible;
			};

			for (int rootBoneVectorIndex : rootBoneIndices)
				markVisible(rootBoneVectorIndex);
		}

		const float treeHeight = std::clamp(ImGui::GetContentRegionAvail().y * 0.48f, ImGuiWrapper::S(280.0f), ImGuiWrapper::S(460.0f));
		if (ImGui::BeginChild("##SkeletonTree", ImVec2(0.0f, treeHeight), true))
		{
			for (int rootBoneVectorIndex : rootBoneIndices)
				DrawSkeletonTreeNode(rootBoneVectorIndex, childBoneIndices, visibleBoneMask);
		}
		ImGui::EndChild();

		if (mSelectedBoneIndex >= 0 && mSelectedBoneIndex < static_cast<int>(mSkeletonRes.bones.size()))
		{
			const SkeletonBoneRes& selectedBone = mSkeletonRes.bones[static_cast<size_t>(mSelectedBoneIndex)];
			const int parentBoneVectorIndex = parentVectorIndices[static_cast<size_t>(mSelectedBoneIndex)];
			const size_t childCount = childBoneIndices[static_cast<size_t>(mSelectedBoneIndex)].size();

			ImGui::Dummy(ImGuiWrapper::S(0.0f, 6.0f));
			ImGuiWrapper::PropertyRow(EditorLayer::GetText("Selected"), [&selectedBone]() { ImGui::TextWrapped("%s", selectedBone.name.c_str()); });
			ImGuiWrapper::PropertyRow(EditorLayer::GetText("Bone Id"), [&selectedBone]() { ImGui::Text("%d", selectedBone.index); });
			ImGuiWrapper::PropertyRow(EditorLayer::GetText("Parent"), [this, parentBoneVectorIndex]()
				{
					if (parentBoneVectorIndex >= 0 && parentBoneVectorIndex < static_cast<int>(mSkeletonRes.bones.size()))
						ImGui::TextWrapped("%s", mSkeletonRes.bones[static_cast<size_t>(parentBoneVectorIndex)].name.c_str());
					else
						ImGui::TextUnformatted(EditorLayer::GetText("None"));
				});
			ImGuiWrapper::PropertyRow(EditorLayer::GetText("Children"), [childCount]() { ImGui::Text("%zu", childCount); });
		}
		else
		{
			ImGui::Dummy(ImGuiWrapper::S(0.0f, 6.0f));
			ImGui::TextDisabled("%s", EditorLayer::GetText("Select a bone in the tree to highlight it in the viewport."));
		}
	}

	void AssetPreviewPanel::DrawSkeletonTreeNode(int boneVectorIndex, const std::vector<std::vector<int>>& childBoneIndices, const std::vector<bool>& visibleBoneMask)
	{
		if (boneVectorIndex < 0 || boneVectorIndex >= static_cast<int>(mSkeletonRes.bones.size()))
			return;
		if (!visibleBoneMask.empty() && !visibleBoneMask[static_cast<size_t>(boneVectorIndex)])
			return;

		const std::vector<int>& children = childBoneIndices[static_cast<size_t>(boneVectorIndex)];
		bool bHasVisibleChild = false;
		for (int childBoneVectorIndex : children)
		{
			if (visibleBoneMask.empty() || visibleBoneMask[static_cast<size_t>(childBoneVectorIndex)])
			{
				bHasVisibleChild = true;
				break;
			}
		}

		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
			ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
		if (mSelectedBoneIndex == boneVectorIndex)
			flags |= ImGuiTreeNodeFlags_Selected;
		if (!bHasVisibleChild)
			flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

		const SkeletonBoneRes& bone = mSkeletonRes.bones[static_cast<size_t>(boneVectorIndex)];
		const bool bOpen = ImGui::TreeNodeEx((void*)(intptr_t)(boneVectorIndex + 1), flags, "%s", bone.name.c_str());
		if (ImGui::IsItemClicked())
		{
			mSelectedBoneIndex = boneVectorIndex;
			mShowSkeleton = true;
		}

		if (bOpen && bHasVisibleChild)
		{
			for (int childBoneVectorIndex : children)
				DrawSkeletonTreeNode(childBoneVectorIndex, childBoneIndices, visibleBoneMask);
			ImGui::TreePop();
		}
	}

	void AssetPreviewPanel::UpdateAnimationPreviewPose(Timestep ts)
	{
		if (mAssetKind != AssetKind::Animation || !mPreviewEntity || !mPreviewEntity.HasComponent<AnimatorComponent>() || !mPreviewEntity.HasComponent<MeshComponent>())
			return;

		auto& animatorComponent = mPreviewEntity.GetComponent<AnimatorComponent>();
		auto& meshComponent = mPreviewEntity.GetComponent<MeshComponent>();
		if (!meshComponent.mMesh || !animatorComponent.bLoaded || animatorComponent.mClips.empty())
			return;

		const int clipCount = static_cast<int>(animatorComponent.mClips.size());
		mSelectedClipIndex = clipCount > 0 ? std::clamp(mSelectedClipIndex, 0, clipCount - 1) : 0;

		const bool bClipChanged = mPreviewClipIndex != mSelectedClipIndex;
		if (bClipChanged)
		{
			mPreviewClipIndex = mSelectedClipIndex;
			mAnimationTimeTicks = 0.0f;
			mAnimationProgress = 0.0f;
		}

		if (bClipChanged || animatorComponent.mLastClipIndex != mSelectedClipIndex)
		{
			meshComponent.mMesh->mAnimation = animatorComponent.mClips[mSelectedClipIndex];
			meshComponent.mMesh->mAnimator = Animator(&meshComponent.mMesh->mAnimation);
			meshComponent.mMesh->mAnimator.SetRootMotionOptions(false, true);
			animatorComponent.mLastClipIndex = mSelectedClipIndex;
		}

		const float duration = meshComponent.mMesh->mAnimation.GetDuration();
		const float ticksPerSecond = meshComponent.mMesh->mAnimation.GetTicksPerSecond();
		if (mPlayAnimation && duration > 0.0f)
		{
			mAnimationTimeTicks += std::max(ticksPerSecond, 0.0f) * static_cast<float>(ts) * mAnimationSpeed;
			if (mLoopAnimation)
			{
				mAnimationTimeTicks = std::fmod(mAnimationTimeTicks, duration);
				if (mAnimationTimeTicks < 0.0f)
					mAnimationTimeTicks += duration;
			}
			else if (mAnimationTimeTicks >= duration)
			{
				mAnimationTimeTicks = duration;
				mPlayAnimation = false;
			}
		}

		if (!mPlayAnimation && duration > 0.0f)
			mAnimationTimeTicks = std::clamp(mAnimationTimeTicks, 0.0f, duration);
		if (duration <= 0.0f)
			mAnimationTimeTicks = 0.0f;

		float poseTimeTicks = mAnimationTimeTicks;
		if (duration > 0.0f && poseTimeTicks >= duration)
			poseTimeTicks = std::max(0.0f, duration - 1e-4f);

		meshComponent.mMesh->bUseExternalBoneMatrices = true;
		meshComponent.mMesh->bPlayAnim = true;
		meshComponent.mMesh->mAnimPlaySpeed = 0.0f;
		meshComponent.mMesh->mAnimator.UpdateAnimationAbsolute(poseTimeTicks);
		meshComponent.mMesh->mExternalBoneMatrices = meshComponent.mMesh->mAnimator.GetFinalBoneMatrices();

		mAnimationProgress = duration > 0.0f ? std::clamp(mAnimationTimeTicks / duration, 0.0f, 1.0f) : 0.0f;
		animatorComponent.mProgress = mAnimationProgress;
		UpdatePreviewEntityPlacement();
	}

	void AssetPreviewPanel::UpdatePreviewEntityPlacement()
	{
		if (!mPreviewEntity || !mPreviewEntity.HasComponent<MeshComponent>() || !mPreviewEntity.HasComponent<TransformComponent>())
			return;

		auto& meshComponent = mPreviewEntity.GetComponent<MeshComponent>();
		if (!meshComponent.mMesh)
			return;

		glm::vec3 minBounds;
		glm::vec3 maxBounds;
		if (!TryBuildMeshBounds(meshComponent, glm::mat4(1.0f), minBounds, maxBounds))
			return;

		glm::vec3 translation = BuildGroundedCenteringOffset(minBounds, maxBounds);
		if (mAssetKind == AssetKind::Animation && !mSkeletonRes.bones.empty())
		{
			std::vector<glm::mat4> animatedTransforms;
			if (TryBuildAnimatedBoneWorldTransforms(*meshComponent.mMesh, mSkeletonRes, animatedTransforms, &mAnimRes.rootNode))
			{
				const int anchorBoneIndex = FindPreferredAnchorBoneIndex(mSkeletonRes);
				if (anchorBoneIndex >= 0 && anchorBoneIndex < static_cast<int>(animatedTransforms.size()))
				{
					const glm::vec3 anchor = glm::vec3(animatedTransforms[static_cast<size_t>(anchorBoneIndex)][3]);
					translation.x = -anchor.x;
					translation.z = -anchor.z;
				}
			}
		}

		mPreviewEntity.GetComponent<TransformComponent>().Translation = translation;
	}

	void AssetPreviewPanel::UpdateSkeletonPreviewOffset()
	{
		mPreviewContentOffset = glm::vec3(0.0f);
		if (mSkeletonRes.bones.empty())
			return;

		std::vector<glm::mat4> worldTransforms;
		if (!TryBuildWorldTransformsFromAnimHierarchy(mAnimRes.rootNode, mSkeletonRes, worldTransforms))
			worldTransforms = BuildSkeletonWorldTransforms(mSkeletonRes);
		glm::vec3 minBounds;
		glm::vec3 maxBounds;
		if (!TryBuildBoundsFromTransforms(worldTransforms, minBounds, maxBounds))
			return;

		mPreviewContentOffset = BuildGroundedCenteringOffset(minBounds, maxBounds);
	}

	void AssetPreviewPanel::ResetAnimationPreviewState()
	{
		mAnimationTimeTicks = 0.0f;
		mAnimationProgress = 0.0f;
		mPreviewClipIndex = -1;
	}

	bool AssetPreviewPanel::BuildPreviewSkeletonSegments(std::vector<PreviewSkeletonSegment>& outSegments, std::vector<int>* outParentVectorIndices)
	{
		if (mSkeletonRes.bones.empty())
			return false;

		const std::vector<int> parentVectorIndices = BuildSkeletonParentVectorIndices(mSkeletonRes, &mAnimRes.rootNode);
		if (outParentVectorIndices)
			*outParentVectorIndices = parentVectorIndices;

		std::vector<glm::mat4> worldTransforms;
		bool bUseAnimatedPose = false;
		glm::mat4 previewEntityWorldTransform = glm::mat4(1.0f);
		if (mPreviewEntity && mPreviewEntity.HasComponent<MeshComponent>())
		{
			auto& meshComponent = mPreviewEntity.GetComponent<MeshComponent>();
			if (meshComponent.mMesh)
			{
				bUseAnimatedPose = TryBuildAnimatedBoneWorldTransforms(*meshComponent.mMesh, mSkeletonRes, worldTransforms, &mAnimRes.rootNode);
				if (bUseAnimatedPose && mPreviewScene)
					previewEntityWorldTransform = mPreviewScene->GetWorldTransform(mPreviewEntity);
			}
		}

		if (!bUseAnimatedPose)
		{
			if (!TryBuildWorldTransformsFromAnimHierarchy(mAnimRes.rootNode, mSkeletonRes, worldTransforms))
				worldTransforms = BuildSkeletonWorldTransforms(mSkeletonRes);
		}

		outSegments.assign(mSkeletonRes.bones.size(), PreviewSkeletonSegment{});
		for (size_t i = 0; i < mSkeletonRes.bones.size(); ++i)
		{
			outSegments[i].boneVectorIndex = static_cast<int>(i);
			outSegments[i].position = bUseAnimatedPose
				? glm::vec3(previewEntityWorldTransform * glm::vec4(glm::vec3(worldTransforms[i][3]), 1.0f))
				: glm::vec3(worldTransforms[i][3]) + mPreviewContentOffset;

			const int parentBoneVectorIndex = parentVectorIndices[i];
			if (parentBoneVectorIndex >= 0 && parentBoneVectorIndex < static_cast<int>(worldTransforms.size()))
			{
				outSegments[i].parentBoneVectorIndex = parentBoneVectorIndex;
				outSegments[i].parentPosition = bUseAnimatedPose
					? glm::vec3(previewEntityWorldTransform * glm::vec4(glm::vec3(worldTransforms[static_cast<size_t>(parentBoneVectorIndex)][3]), 1.0f))
					: glm::vec3(worldTransforms[static_cast<size_t>(parentBoneVectorIndex)][3]) + mPreviewContentOffset;
				outSegments[i].bHasParent = true;
			}
		}

		return true;
	}

	void AssetPreviewPanel::DrawSkeletonOverlay(const ImVec2& viewportMin, const ImVec2& viewportSize)
	{
		std::vector<PreviewSkeletonSegment> segments;
		if (!BuildPreviewSkeletonSegments(segments, nullptr))
			return;

		const bool bSkeletonOnly = mShowSkeleton && !mShowMesh;
		const glm::vec4 defaultBoneFill = bSkeletonOnly
			? glm::vec4(0.93f, 0.79f, 0.18f, 0.96f)
			: glm::vec4(0.86f, 0.72f, 0.22f, 0.90f);
		const glm::vec4 defaultBoneOutline = bSkeletonOnly
			? glm::vec4(0.44f, 0.31f, 0.02f, 1.0f)
			: glm::vec4(0.34f, 0.23f, 0.02f, 0.98f);
		const glm::vec4 selectedBoneFill(1.0f, 0.93f, 0.36f, 1.0f);
		const glm::vec4 selectedBoneOutline(0.56f, 0.40f, 0.04f, 1.0f);
		const float jointRadius = bSkeletonOnly ? ImGuiWrapper::S(6.0f) : ImGuiWrapper::S(4.6f);
		const float outlineThickness = std::max(ImGuiWrapper::S(bSkeletonOnly ? 1.4f : 1.25f), 1.0f);
		const float baseWidthScale = bSkeletonOnly ? 0.18f : 0.14f;
		const float selectedWidthScale = bSkeletonOnly ? 0.20f : 0.16f;
		const float minBaseHalfWidth = ImGuiWrapper::S(bSkeletonOnly ? 4.2f : 3.0f);
		const float maxBaseHalfWidth = ImGuiWrapper::S(bSkeletonOnly ? 12.5f : 9.0f);
		const float minSelectedBaseHalfWidth = ImGuiWrapper::S(bSkeletonOnly ? 5.0f : 4.0f);
		const float maxSelectedBaseHalfWidth = ImGuiWrapper::S(bSkeletonOnly ? 14.0f : 11.0f);
		const float minTipHalfWidth = ImGuiWrapper::S(bSkeletonOnly ? 1.8f : 1.2f);
		const float minSelectedTipHalfWidth = ImGuiWrapper::S(bSkeletonOnly ? 2.0f : 1.4f);

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->PushClipRect(viewportMin, ImVec2(viewportMin.x + viewportSize.x, viewportMin.y + viewportSize.y), true);

		struct ProjectedJoint
		{
			ImVec2 position = ImVec2(0.0f, 0.0f);
			bool bVisible = false;
		};

		std::vector<ProjectedJoint> projectedJoints(segments.size());
		for (size_t i = 0; i < segments.size(); ++i)
			projectedJoints[i].bVisible = ProjectWorldToViewport(mCamera, segments[i].position, viewportMin, viewportSize, projectedJoints[i].position);

		for (const PreviewSkeletonSegment& segment : segments)
		{
			if (segment.bHasParent &&
				segment.boneVectorIndex != mSelectedBoneIndex &&
				projectedJoints[static_cast<size_t>(segment.boneVectorIndex)].bVisible &&
				projectedJoints[static_cast<size_t>(segment.parentBoneVectorIndex)].bVisible)
			{
				const ImVec2 start = projectedJoints[static_cast<size_t>(segment.parentBoneVectorIndex)].position;
				const ImVec2 end = projectedJoints[static_cast<size_t>(segment.boneVectorIndex)].position;
				const ImVec2 delta = SubImVec2(end, start);
				const float screenLength = std::sqrt(delta.x * delta.x + delta.y * delta.y);
				const float baseHalfWidth = std::clamp(screenLength * baseWidthScale, minBaseHalfWidth, maxBaseHalfWidth);
				const float tipHalfWidth = std::max(baseHalfWidth * 0.38f, minTipHalfWidth);
				DrawBoneConeOverlay(drawList, start, end, baseHalfWidth, tipHalfWidth, ToImU32Color(defaultBoneFill), ToImU32Color(defaultBoneOutline), outlineThickness);
			}
		}

		for (size_t i = 0; i < projectedJoints.size(); ++i)
		{
			if (!projectedJoints[i].bVisible || static_cast<int>(i) == mSelectedBoneIndex)
				continue;

			DrawJointOverlay(drawList, projectedJoints[i].position, jointRadius, ToImU32Color(defaultBoneFill), ToImU32Color(defaultBoneOutline), outlineThickness);
		}

		if (mSelectedBoneIndex >= 0 && mSelectedBoneIndex < static_cast<int>(segments.size()))
		{
			const PreviewSkeletonSegment& selectedSegment = segments[static_cast<size_t>(mSelectedBoneIndex)];
			const ProjectedJoint& selectedJoint = projectedJoints[static_cast<size_t>(mSelectedBoneIndex)];
			if (selectedSegment.bHasParent &&
				selectedJoint.bVisible &&
				projectedJoints[static_cast<size_t>(selectedSegment.parentBoneVectorIndex)].bVisible)
			{
				const ImVec2 start = projectedJoints[static_cast<size_t>(selectedSegment.parentBoneVectorIndex)].position;
				const ImVec2 end = selectedJoint.position;
				const ImVec2 delta = SubImVec2(end, start);
				const float screenLength = std::sqrt(delta.x * delta.x + delta.y * delta.y);
				const float baseHalfWidth = std::clamp(screenLength * selectedWidthScale, minSelectedBaseHalfWidth, maxSelectedBaseHalfWidth);
				const float tipHalfWidth = std::max(baseHalfWidth * 0.42f, minSelectedTipHalfWidth);
				DrawBoneConeOverlay(drawList, start, end, baseHalfWidth, tipHalfWidth, ToImU32Color(selectedBoneFill), ToImU32Color(selectedBoneOutline), outlineThickness + ImGuiWrapper::S(0.5f));
			}

			if (selectedJoint.bVisible)
				DrawJointOverlay(drawList, selectedJoint.position, jointRadius + ImGuiWrapper::S(bSkeletonOnly ? 1.4f : 1.0f), ToImU32Color(selectedBoneFill), ToImU32Color(selectedBoneOutline), outlineThickness + ImGuiWrapper::S(0.5f));
		}

		drawList->PopClipRect();
	}

	void AssetPreviewPanel::DrawFloorGrid()
	{
		const float gridHalfExtent = std::clamp(
			SnapUpToStep(std::max(mPreviewRadius * 2.0f, kPreviewGridMinHalfExtent), kPreviewGridMinorStep),
			kPreviewGridMinHalfExtent,
			kPreviewGridMaxHalfExtent);
		const int lineCount = static_cast<int>(gridHalfExtent / kPreviewGridMinorStep);
		const glm::vec4 minorColor(0.26f, 0.27f, 0.29f, 0.65f);
		const glm::vec4 majorColor(0.38f, 0.40f, 0.43f, 0.85f);
		const glm::vec4 axisX(0.85f, 0.24f, 0.24f, 0.95f);
		const glm::vec4 axisZ(0.24f, 0.52f, 0.92f, 0.95f);

		for (int i = -lineCount; i <= lineCount; ++i)
		{
			const float p = static_cast<float>(i) * kPreviewGridMinorStep;
			const glm::vec4 color = (i % kPreviewGridMajorEvery == 0) ? majorColor : minorColor;
			Renderer2D::DrawLine(glm::vec3(-gridHalfExtent, 0.0f, p), glm::vec3(gridHalfExtent, 0.0f, p), i == 0 ? axisX : color);
			Renderer2D::DrawLine(glm::vec3(p, 0.0f, -gridHalfExtent), glm::vec3(p, 0.0f, gridHalfExtent), i == 0 ? axisZ : color);
		}
	}

	void AssetPreviewPanel::FrameAsset()
	{
		mPreviewCenter = ComputePreviewCenter();
		mPreviewRadius = ComputePreviewRadius();

		const float distance = std::max(mPreviewRadius * 2.8f, 2.5f);
		mCamera.SetFarPlane(std::max(1000.0f, distance + mPreviewRadius * 4.0f));
		mCamera.UpdateProjection();
		mCamera.SetCenter(mPreviewCenter);
		mCamera.SetDistance(distance);
		mCamera.SetOrientation(glm::quat(glm::vec3(glm::radians(-22.0f), glm::radians(-35.0f), 0.0f)));
		mCamera.mCameraSpeed = std::clamp(distance * 0.18f, 0.2f, 20.0f);
	}

	AssetPreviewPanel::AssetKind AssetPreviewPanel::ResolveAssetKind() const
	{
		const std::string ext = ToLower(mAssetFullPath.extension().string());
		if (ext == ".hmesh")
			return AssetKind::Mesh;
		if (ext == ".hanim")
			return AssetKind::Animation;
		if (ext == ".hskel")
			return AssetKind::Skeleton;
		return AssetKind::Unknown;
	}

	bool AssetPreviewPanel::IsRenderableAsset() const
	{
		return mAssetKind == AssetKind::Mesh || (mAssetKind == AssetKind::Animation && mPreviewEntity);
	}

	bool AssetPreviewPanel::HasPreviewMesh()
	{
		return mPreviewEntity && mPreviewEntity.HasComponent<MeshComponent>() && mPreviewEntity.GetComponent<MeshComponent>().mMesh;
	}

	std::string AssetPreviewPanel::GetAssetKindName() const
	{
		switch (mAssetKind)
		{
		case AssetKind::Mesh:
			return EditorLayer::GetText("Mesh");
		case AssetKind::Animation:
			return EditorLayer::GetText("Animation");
		case AssetKind::Skeleton:
			return EditorLayer::GetText("Skeleton");
		default:
		return EditorLayer::GetText("Asset");
		}
	}

	float AssetPreviewPanel::GetSelectedClipDurationSeconds() const
	{
		if (mSelectedClipIndex < 0 || mSelectedClipIndex >= static_cast<int>(mAnimRes.clips.size()))
			return 0.0f;

		const AnimClipRes& clip = mAnimRes.clips[static_cast<size_t>(mSelectedClipIndex)];
		const float ticksPerSecond = std::max(clip.ticksPerSecond, 0.0f);
		if (ticksPerSecond <= 0.0f)
			return 0.0f;
		return clip.duration / ticksPerSecond;
	}

	float AssetPreviewPanel::GetSelectedClipCurrentSeconds() const
	{
		if (mSelectedClipIndex < 0 || mSelectedClipIndex >= static_cast<int>(mAnimRes.clips.size()))
			return 0.0f;

		const AnimClipRes& clip = mAnimRes.clips[static_cast<size_t>(mSelectedClipIndex)];
		const float ticksPerSecond = std::max(clip.ticksPerSecond, 0.0f);
		if (ticksPerSecond <= 0.0f)
			return 0.0f;
		return mAnimationTimeTicks / ticksPerSecond;
	}

	glm::vec3 AssetPreviewPanel::ComputePreviewCenter()
	{
		if (mPreviewEntity && mPreviewEntity.HasComponent<MeshComponent>())
		{
			const auto& meshComponent = mPreviewEntity.GetComponent<MeshComponent>();
			glm::vec3 minBounds;
			glm::vec3 maxBounds;
			const glm::mat4 worldTransform = mPreviewScene ? mPreviewScene->GetWorldTransform(mPreviewEntity) : glm::mat4(1.0f);
			if (TryBuildMeshBounds(meshComponent, worldTransform, minBounds, maxBounds))
				return (minBounds + maxBounds) * 0.5f;
		}

		if (!mSkeletonRes.bones.empty())
		{
			std::vector<glm::mat4> worldTransforms;
			if (!TryBuildWorldTransformsFromAnimHierarchy(mAnimRes.rootNode, mSkeletonRes, worldTransforms))
				worldTransforms = BuildSkeletonWorldTransforms(mSkeletonRes);
			glm::vec3 minBounds(std::numeric_limits<float>::max());
			glm::vec3 maxBounds(std::numeric_limits<float>::lowest());
			for (const glm::mat4& transform : worldTransforms)
				ExpandBounds(minBounds, maxBounds, glm::vec3(transform[3]) + mPreviewContentOffset);
			return (minBounds + maxBounds) * 0.5f;
		}

		return glm::vec3(0.0f);
	}

	float AssetPreviewPanel::ComputePreviewRadius()
	{
		if (mPreviewEntity && mPreviewEntity.HasComponent<MeshComponent>())
		{
			const auto& meshComponent = mPreviewEntity.GetComponent<MeshComponent>();
			glm::vec3 minBounds;
			glm::vec3 maxBounds;
			const glm::mat4 worldTransform = mPreviewScene ? mPreviewScene->GetWorldTransform(mPreviewEntity) : glm::mat4(1.0f);
			if (TryBuildMeshBounds(meshComponent, worldTransform, minBounds, maxBounds))
				return SafeRadiusFromBounds(minBounds, maxBounds);
		}

		if (!mSkeletonRes.bones.empty())
		{
			std::vector<glm::mat4> worldTransforms;
			if (!TryBuildWorldTransformsFromAnimHierarchy(mAnimRes.rootNode, mSkeletonRes, worldTransforms))
				worldTransforms = BuildSkeletonWorldTransforms(mSkeletonRes);
			glm::vec3 minBounds(std::numeric_limits<float>::max());
			glm::vec3 maxBounds(std::numeric_limits<float>::lowest());
			for (const glm::mat4& transform : worldTransforms)
				ExpandBounds(minBounds, maxBounds, glm::vec3(transform[3]) + mPreviewContentOffset);
			return SafeRadiusFromBounds(minBounds, maxBounds);
		}

		return 1.0f;
	}
}
