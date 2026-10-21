#include "Hepch.h"

#include "Runtime/EcsFramework/System/Animation/AnimationSystem.h"
#include "Runtime/EcsFramework/Component/Animation/AnimatorComponent.h"
#include "Runtime/EcsFramework/Component/Animation/BoneAttachmentComponent.h"
#include "Runtime/EcsFramework/Component/Basic/TagComponent.h"
#include "Runtime/EcsFramework/Component/Mesh/MeshComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/Resource/ResType/Animation.h"
#include "Runtime/Animation/MotionBlend.h"
#include "Runtime/Animation/CharacterDeformation.h"
#include "Runtime/Profile/RenderFrameTiming.h"
#include "Runtime/EcsFramework/System/Physics/PhysicSystem3D.h"
#include "Runtime/Resource/ResType/AnimController.h"
#include "Runtime/Resource/ResType/Skeleton.h"
#include "Runtime/Core/UUID.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Core/Meta/Json.h"
#include "_generated/serializer/Animation.gen.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <stop_token>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

namespace HEngine
{
    namespace
    {
        struct SharedPausedPose
        {
            std::map<std::string, BoneInfo> BoneInfoMap;
            std::vector<glm::mat4> BoneMatrices;
            int BoneCount = 0;
            int ClipIndex = 0;
        };

        struct CachedControllerData
        {
            AnimControllerRes Controller;
            std::map<std::string, BoneInfo> BoneInfoMap;
            std::vector<Animation> Clips;
            std::vector<std::string> ClipNames;
            std::map<int, int> NodeToClipIndex;
            int BoneCount = 0;
            int InitialNodeId = -1;
        };

        struct PreparedControllerFiles
        {
            AnimControllerRes Controller;
            std::unordered_map<int, AnimRes> ClipsByNode;
        };

        static std::unordered_map<std::string, SharedPausedPose> sSharedPausedPoseCache;
        static std::unordered_map<std::string, CachedControllerData> sControllerCache;
        static std::unordered_set<std::string> sFailedControllerDiagnostics;
        static std::unordered_set<std::string> sFailedLegacyDiagnostics;

        static bool UseAsyncAnimationPreparation()
        {
            static const bool enabled = []
            {
                const char* setting = std::getenv("HE_ANIMATION_PREPARE_ASYNC");
                return !setting || setting[0] != '0';
            }();
            return enabled;
        }

        constexpr float kBlendEdgeEpsilon = 1e-4f;
        constexpr bool kFreezeFromPoseDuringTransition = true;

        static void ConfigureAnimatorRootMotion(Animator& animator, const AnimatorComponent& animComp)
        {
            animator.SetRootMotionOptions(animComp.bEnableRootMotion, animComp.bLockRootMotionXZ);
            animator.SetLoop(animComp.bLoop);
        }

        static size_t GetBonePaletteCount(
            const std::vector<glm::mat4>& matrices,
            int boneCount)
        {
            return std::min(
                matrices.size(),
                static_cast<size_t>(std::max(0, boneCount)));
        }

        static void CopyBonePalette(
            std::vector<glm::mat4>& destination,
            const std::vector<glm::mat4>& source,
            int boneCount)
        {
            const size_t count = GetBonePaletteCount(source, boneCount);
            destination.assign(source.begin(), source.begin() + count);
        }

        static void ParseAnimNodeRes(const Json& j, AnimNodeRes& node)
        {
            node.name = j.value("name", "");
            if (j.contains("localTransform") && j["localTransform"].is_array())
                node.localTransform = j["localTransform"].get<std::vector<float>>();

            node.children.clear();
            if (j.contains("children") && j["children"].is_array())
            {
                for (const auto& childJ : j["children"])
                {
                    AnimNodeRes child;
                    ParseAnimNodeRes(childJ, child);
                    node.children.push_back(std::move(child));
                }
            }
        }

        static void ParseAnimResFromJson(const Json& j, AnimRes& out)
        {
            out = AnimRes{};
            Serializer::Deserialize(j, out);
            out.rootNode = AnimNodeRes{};
            if (j.contains("rootNode") && j["rootNode"].is_object())
                ParseAnimNodeRes(j["rootNode"], out.rootNode);
        }

        static std::string EntityNameSafe(Entity entity)
        {
            if (entity.HasComponent<TagComponent>())
                return entity.GetComponent<TagComponent>().Tag;
            return "UnknownEntity";
        }

        static int FindInitialStateNodeId(const AnimControllerRes& controller, const std::map<int, int>& nodeToClip)
        {
            if (nodeToClip.count(controller.entryNodeId))
                return controller.entryNodeId;

            for (const auto& transition : controller.transitions)
            {
                if (transition.fromNodeId == controller.entryNodeId && nodeToClip.count(transition.toNodeId))
                    return transition.toNodeId;
            }

            if (!nodeToClip.empty())
                return nodeToClip.begin()->first;

            return -1;
        }

        static std::string GetNodeName(const AnimControllerRes& controller, int nodeId)
        {
            for (const auto& node : controller.nodes)
            {
                if (node.id == nodeId)
                    return node.name;
            }
            return "";
        }

        static std::map<std::string, BoneInfo> BuildBoneInfoMapFromSkeletonPath(const std::string& skeletonPath)
        {
            std::map<std::string, BoneInfo> boneInfoMap;

            const std::string normalizedSkeletonPath = Utils::NormalizeAssetPath(skeletonPath);
            if (normalizedSkeletonPath.empty())
                return boneInfoMap;

            SkeletonRes skeletonRes;
            std::string skeletonError;
            if (!LoadSkeletonRes(ConfigManager::GetFullPath(normalizedSkeletonPath), skeletonRes, &skeletonError))
            {
                HE_CORE_WARN("AnimationSystem: failed to load skeleton '{0}': {1}", normalizedSkeletonPath, skeletonError);
                return boneInfoMap;
            }

            for (size_t i = 0; i < skeletonRes.bones.size(); ++i)
            {
                const auto& boneRes = skeletonRes.bones[i];

                BoneInfo info;
                info.id = boneRes.index;
                if (i < skeletonRes.inverseBindMatrices.size() && skeletonRes.inverseBindMatrices[i].size() == 16)
                    info.offset = glm::make_mat4(skeletonRes.inverseBindMatrices[i].data());
                else
                    info.offset = glm::mat4(1.0f);

                boneInfoMap[boneRes.name] = info;
            }

            return boneInfoMap;
        }

        static bool ResolveLegacyBoneInfoMap(const AnimRes& animRes, const MeshComponent& meshComp,
            const std::string& animationPath, std::map<std::string, BoneInfo>& boneInfoMap)
        {
            const std::string meshSkeleton = Utils::NormalizeAssetPath(meshComp.mMesh->mSkeletonPath);
            const std::string clipSkeleton = Utils::NormalizeAssetPath(animRes.targetSkeletonPath);
            if (!clipSkeleton.empty() && !Utils::IsSameAssetPath(meshSkeleton, clipSkeleton))
            {
                HE_CORE_ERROR("AnimationSystem: direct clip skeleton mismatch mesh='{0}', clip='{1}', animation='{2}'",
                    meshSkeleton, clipSkeleton, animationPath);
                return false;
            }

            boneInfoMap.clear();
            if (animRes.boneInfos.empty())
            {
                // Animation-only FBX exports contain named bone tracks but no skin
                // offsets. The mesh skeleton is authoritative for palette IDs and
                // inverse binds, just as it is for controller-driven clips.
                boneInfoMap = BuildBoneInfoMapFromSkeletonPath(meshSkeleton);
                if (boneInfoMap.empty())
                {
                    HE_CORE_WARN("AnimationSystem: '{0}' has no boneInfos and no usable mesh skeleton",
                        animationPath);
                    return false;
                }
                return true;
            }

            for (const auto& boneInfoRes : animRes.boneInfos)
            {
                BoneInfo info;
                info.id = boneInfoRes.id;
                info.offset = boneInfoRes.offsetMatrix.size() == 16
                    ? glm::make_mat4(boneInfoRes.offsetMatrix.data()) : glm::mat4(1.0f);
                boneInfoMap[boneInfoRes.name] = info;
            }
            return true;
        }

        static void ApplyBindPose(AnimatorComponent& animComp, MeshComponent& meshComp)
        {
            if (!meshComp.mMesh)
                return;

            if (!animComp.mClips.empty())
            {
                const int clipIndex = std::clamp(
                    animComp.currentClipIndex,
                    0,
                    static_cast<int>(animComp.mClips.size()) - 1);
                const bool bAlreadyUsingSharedPose =
                    !meshComp.mMesh->bPlayAnim &&
                    meshComp.mMesh->bUseExternalBoneMatrices &&
                    !meshComp.mMesh->mExternalBoneMatrices.empty() &&
                    animComp.mLastClipIndex == clipIndex;
                if (bAlreadyUsingSharedPose)
                    return;

                meshComp.mMesh->mAnimation = animComp.mClips[clipIndex];
                meshComp.mMesh->mAnimator = Animator(&meshComp.mMesh->mAnimation);
                ConfigureAnimatorRootMotion(meshComp.mMesh->mAnimator, animComp);
                meshComp.mMesh->mAnimator.UpdateAnimationAbsolute(0.0f);
                meshComp.mMesh->bPlayAnim = false;
                meshComp.mMesh->bUseExternalBoneMatrices = true;
                CopyBonePalette(
                    meshComp.mMesh->mExternalBoneMatrices,
                    meshComp.mMesh->mAnimator.GetFinalBoneMatrices(),
                    animComp.mBoneCount);
                animComp.mLastClipIndex = clipIndex;
                animComp.mProgress = 0.0f;
                return;
            }

            const bool bAlreadyInBindPose =
                !meshComp.mMesh->bPlayAnim &&
                !meshComp.mMesh->bUseExternalBoneMatrices &&
                meshComp.mMesh->mExternalBoneMatrices.empty() &&
                animComp.mLastClipIndex == -1 &&
                animComp.mProgress <= 0.0f;
            if (bAlreadyInBindPose)
                return;

            meshComp.mMesh->bPlayAnim = false;
            meshComp.mMesh->bUseExternalBoneMatrices = false;
            meshComp.mMesh->mExternalBoneMatrices.clear();
            meshComp.mMesh->mAnimator.ClearAnimation();
            meshComp.mMesh->mAnimation = Animation();
            animComp.mLastClipIndex = -1;
            animComp.mProgress = 0.0f;
        }

        static void ResetRuntimeState(AnimatorComponent& animComp)
        {
            animComp.mSecondaryMotion.Reset();
            animComp.mCharacterPose.Reset();
            animComp.mClothSimulation.clear();
            animComp.mFacialPlayer.Reset();
            animComp.mDeformationWasActive = false;
            animComp.bLoaded = false;
            animComp.mClips.clear();
            animComp.mClipNames.clear();
            animComp.mBoneInfoMap.clear();
            animComp.mBoneCount = 0;
            animComp.mLastClipIndex = -1;
            animComp.mProgress = 0.0f;
            animComp.mControllerRes = {};
            animComp.mParameters.clear();
            animComp.mParameterTypes.clear();
            animComp.mNodeToClipIndex.clear();
            animComp.mCurrentNodeId = -1;
            animComp.mTargetNodeId = -1;
            animComp.mCurrentStateName.clear();
            animComp.mAnimationEvents = Json::array();
            animComp.bHoldFinalPose = false;
            animComp.mInTransition = false;
            animComp.mTransitionTime = 0.0f;
            animComp.mTransitionDuration = 0.0f;
            animComp.mTransitionFromClipIndex = -1;
            animComp.mTransitionToClipIndex = -1;
            animComp.mTransitionFromAnimator.ClearAnimation();
            animComp.mTransitionToAnimator.ClearAnimation();
        }

        static PreparedControllerFiles PrepareControllerFiles(
            const std::string& controllerPath, std::stop_token stopToken = {})
        {
            const std::filesystem::path controllerFullPath = ConfigManager::GetFullPath(controllerPath);
            if (!std::filesystem::exists(controllerFullPath))
                throw std::runtime_error("Missing animation controller: " + controllerPath);

            PreparedControllerFiles prepared;
            std::string controllerError;
            if (!LoadAnimControllerRes(controllerFullPath, prepared.Controller, &controllerError))
                throw std::runtime_error("Cannot load animation controller " + controllerPath + ": " + controllerError);
            prepared.Controller.targetSkeletonPath =
                Utils::NormalizeAssetPath(prepared.Controller.targetSkeletonPath);

            for (const auto& node : prepared.Controller.nodes)
            {
                if (stopToken.stop_requested())
                    throw std::runtime_error("Animation controller preparation cancelled");
                if (node.type != "State" || node.clipPath.empty()) continue;
                const std::string clipPath = Utils::NormalizeAssetPath(node.clipPath);
                const std::filesystem::path clipFullPath = ConfigManager::GetFullPath(clipPath);
                if (!std::filesystem::exists(clipFullPath))
                {
                    HE_CORE_ERROR("AnimationSystem: state='{}' missing clipPath='{}'",
                        node.name, clipPath);
                    continue;
                }
                try
                {
                    AnimRes clip;
                    ParseAnimResFromJson(Utils::LoadJsonFile(clipFullPath), clip);
                    prepared.ClipsByNode.emplace(node.id, std::move(clip));
                }
                catch (const std::exception& error)
                {
                    HE_CORE_ERROR("AnimationSystem: parse clip failed '{}': {}",
                        clipPath, error.what());
                }
            }
            return prepared;
        }

        static AnimRes ReadLegacyAnimRes(const std::string& assetPath)
        {
            const std::filesystem::path fullPath = ConfigManager::GetFullPath(assetPath);
            if (!std::filesystem::exists(fullPath))
                throw std::runtime_error("Missing animation asset: " + assetPath);
            AnimRes result;
            ParseAnimResFromJson(Utils::LoadJsonFile(fullPath), result);
            return result;
        }

        static std::shared_ptr<const AnimRes> AcquireLegacyAnimRes(
            Level* level, const std::string& assetPath, bool bAllowAsyncPreparation)
        {
            RuntimeResourceRegistry* registry = level ? level->GetRuntimeResourceRegistry() : nullptr;
            if (bAllowAsyncPreparation && UseAsyncAnimationPreparation() &&
                registry && registry->IsInitialized())
            {
                const RuntimeResourceKey key = RuntimeResourceKey::FromProjectPath(
                    RuntimeResourceKind::Generic, assetPath, "animation-legacy-json-v1");
                auto ready = registry->TryAcquireReady<AnimRes>(key);
                if (ready) return ready.GetShared();
                const RuntimeResourceState state = registry->GetState(key);
                if (state == RuntimeResourceState::Unloaded)
                {
                    if (RuntimeResourceGroup* group = level->GetOrCreateRuntimeResourceGroup())
                        (void)registry->QueueAsyncPinned<AnimRes, AnimRes>(
                            key, *group,
                            [assetPath](std::stop_token stopToken)
                            {
                                if (stopToken.stop_requested())
                                    throw std::runtime_error("Animation preparation cancelled");
                                return ReadLegacyAnimRes(assetPath);
                            },
                            [](AnimRes&& resource)
                            { return std::make_shared<AnimRes>(std::move(resource)); });
                }
                else if (state == RuntimeResourceState::Failed &&
                    sFailedLegacyDiagnostics.insert(assetPath).second)
                {
                    try { registry->RethrowFailure(key); }
                    catch (const std::exception& error)
                    {
                        HE_CORE_ERROR("AnimationSystem: legacy preparation failed '{}': {}",
                            assetPath, error.what());
                    }
                }
                return {};
            }
            try { return std::make_shared<AnimRes>(ReadLegacyAnimRes(assetPath)); }
            catch (const std::exception& error)
            {
                HE_CORE_ERROR("AnimationSystem: failed to read '{}': {}", assetPath, error.what());
                return {};
            }
        }

        static bool LoadControllerForEntity(Entity entity, AnimatorComponent& animComp,
            MeshComponent& meshComp, bool bAllowAsyncPreparation)
        {
            const std::string entityName = EntityNameSafe(entity);
            const std::string controllerPath = Utils::NormalizeAssetPath(animComp.controllerPath);
            const std::string meshSkeletonPath = Utils::NormalizeAssetPath(meshComp.mMesh->mSkeletonPath);

            auto cacheIt = sControllerCache.find(controllerPath);
            if (cacheIt == sControllerCache.end())
            {
                std::shared_ptr<PreparedControllerFiles> prepared;
                Level* level = entity.GetLevel();
                RuntimeResourceRegistry* registry = level ? level->GetRuntimeResourceRegistry() : nullptr;
                if (bAllowAsyncPreparation && UseAsyncAnimationPreparation() &&
                    registry && registry->IsInitialized())
                {
                    const RuntimeResourceKey key = RuntimeResourceKey::FromProjectPath(
                        RuntimeResourceKind::Generic, controllerPath, "animation-controller-files-v1");
                    auto ready = registry->TryAcquireReady<PreparedControllerFiles>(key);
                    if (!ready)
                    {
                        const RuntimeResourceState state = registry->GetState(key);
                        if (state == RuntimeResourceState::Unloaded)
                        {
                            if (RuntimeResourceGroup* group = level->GetOrCreateRuntimeResourceGroup())
                                (void)registry->QueueAsyncPinned<PreparedControllerFiles, PreparedControllerFiles>(
                                    key, *group,
                                    [controllerPath](std::stop_token stopToken)
                                    { return PrepareControllerFiles(controllerPath, stopToken); },
                                    [](PreparedControllerFiles&& files)
                                    { return std::make_shared<PreparedControllerFiles>(std::move(files)); });
                        }
                        else if (state == RuntimeResourceState::Failed &&
                            sFailedControllerDiagnostics.insert(controllerPath).second)
                        {
                            try { registry->RethrowFailure(key); }
                            catch (const std::exception& error)
                            {
                                HE_CORE_ERROR("AnimationSystem: controller preparation failed '{}': {}",
                                    controllerPath, error.what());
                            }
                        }
                        return false;
                    }
                    prepared = ready.GetShared();
                }
                else
                {
                    try { prepared = std::make_shared<PreparedControllerFiles>(
                        PrepareControllerFiles(controllerPath)); }
                    catch (const std::exception& error)
                    {
                        HE_CORE_ERROR("AnimationSystem: entity='{}' controller preparation failed '{}': {}",
                            entityName, controllerPath, error.what());
                        return false;
                    }
                }
                CachedControllerData cachedData;
                cachedData.Controller = prepared->Controller;
                if (!Utils::IsSameAssetPath(meshSkeletonPath, cachedData.Controller.targetSkeletonPath))
                {
                    HE_CORE_ERROR("AnimationSystem: entity='{0}', component='AnimatorComponent', skeleton mismatch mesh='{1}', controller='{2}'",
                        entityName, meshSkeletonPath, cachedData.Controller.targetSkeletonPath);
                    return false;
                }

                cachedData.BoneInfoMap =
                    BuildBoneInfoMapFromSkeletonPath(cachedData.Controller.targetSkeletonPath);

                for (const auto& node : cachedData.Controller.nodes)
                {
                    if (node.type != "State" || node.clipPath.empty())
                        continue;
                    const std::string clipPath = Utils::NormalizeAssetPath(node.clipPath);
                    const auto clipIt = prepared->ClipsByNode.find(node.id);
                    if (clipIt == prepared->ClipsByNode.end()) continue;
                    const AnimRes& clipRes = clipIt->second;

                    std::string clipSkeleton = Utils::NormalizeAssetPath(clipRes.targetSkeletonPath);
                    if (clipSkeleton.empty())
                        clipSkeleton = cachedData.Controller.targetSkeletonPath;

                    if (!Utils::IsSameAssetPath(clipSkeleton, cachedData.Controller.targetSkeletonPath))
                    {
                        HE_CORE_ERROR("AnimationSystem: entity='{0}', component='AnimatorComponent', clip skeleton mismatch clip='{1}', controller='{2}', stateClip='{3}'",
                            entityName, clipSkeleton, cachedData.Controller.targetSkeletonPath, clipPath);
                        continue;
                    }

                    if (clipRes.clips.empty())
                    {
                        HE_CORE_WARN("AnimationSystem: entity='{0}', component='AnimatorComponent', state='{1}' clip has no clips: '{2}'",
                            entityName, node.name, clipPath);
                        continue;
                    }

                    std::map<std::string, BoneInfo> clipBoneMap = cachedData.BoneInfoMap;
                    cachedData.Clips.emplace_back(clipRes, 0, clipBoneMap);

                    const std::string finalName = node.name.empty() ? clipRes.clips[0].name : node.name;
                    cachedData.ClipNames.push_back(finalName);
                    cachedData.NodeToClipIndex[node.id] =
                        static_cast<int>(cachedData.Clips.size()) - 1;

                    cachedData.BoneInfoMap = std::move(clipBoneMap);
                }

                cachedData.BoneCount = static_cast<int>(cachedData.BoneInfoMap.size());
                cachedData.InitialNodeId = FindInitialStateNodeId(
                    cachedData.Controller,
                    cachedData.NodeToClipIndex);
                if (cachedData.InitialNodeId < 0 ||
                    !cachedData.NodeToClipIndex.count(cachedData.InitialNodeId))
                {
                    HE_CORE_ERROR("AnimationSystem: entity='{0}', component='AnimatorComponent', no valid initial state in controller '{1}'",
                        entityName, controllerPath);
                    return false;
                }

                cacheIt = sControllerCache.emplace(controllerPath, std::move(cachedData)).first;
            }

            const CachedControllerData& cachedData = cacheIt->second;
            if (!Utils::IsSameAssetPath(meshSkeletonPath, cachedData.Controller.targetSkeletonPath))
            {
                HE_CORE_ERROR("AnimationSystem: entity='{0}', component='AnimatorComponent', skeleton mismatch mesh='{1}', controller='{2}'",
                    entityName, meshSkeletonPath, cachedData.Controller.targetSkeletonPath);
                return false;
            }

            animComp.mControllerRes = cachedData.Controller;
            animComp.controllerPath = controllerPath;
            animComp.mBoneInfoMap = cachedData.BoneInfoMap;
            animComp.mClips = cachedData.Clips;
            animComp.mClipNames = cachedData.ClipNames;
            animComp.mNodeToClipIndex = cachedData.NodeToClipIndex;
            animComp.mBoneCount = cachedData.BoneCount;

            for (const auto& parameter : cachedData.Controller.parameters)
            {
                animComp.mParameterTypes[parameter.name] = parameter.type;
                animComp.mParameters[parameter.name] = parameter.defaultValue;
            }

            meshComp.mMesh->GetBoneInfoMap() = animComp.mBoneInfoMap;
            meshComp.mMesh->GetBoneCount() = animComp.mBoneCount;
            meshComp.mMesh->bAnimated = animComp.mBoneCount > 0;

            animComp.mCurrentNodeId = cachedData.InitialNodeId;
            const int initialClipIndex = animComp.mNodeToClipIndex[cachedData.InitialNodeId];
            animComp.mCurrentStateName =
                GetNodeName(cachedData.Controller, cachedData.InitialNodeId);
            meshComp.mMesh->mAnimation = animComp.mClips[initialClipIndex];
            meshComp.mMesh->mAnimator = Animator(&meshComp.mMesh->mAnimation);
            ConfigureAnimatorRootMotion(meshComp.mMesh->mAnimator, animComp);
            meshComp.mMesh->bUseExternalBoneMatrices = true;
            ApplyMotionBlend(meshComp.mMesh->mAnimator, animComp.mCurrentNodeId, animComp.mControllerRes,
                animComp.mNodeToClipIndex, animComp.mClips, animComp.mParameters, animComp.bLoop,
                animComp.bEnableRootMotion, animComp.bLockRootMotionXZ);
            CopyBonePalette(
                meshComp.mMesh->mExternalBoneMatrices,
                meshComp.mMesh->mAnimator.GetFinalBoneMatrices(),
                animComp.mBoneCount);

            return true;
        }

        static bool LoadLegacyAnimRes(Level* level, AnimatorComponent& animComp,
            MeshComponent& meshComp, bool bAllowAsyncPreparation)
        {
            const auto prepared = AcquireLegacyAnimRes(level,
                Utils::NormalizeAssetPath(animComp.animResPath), bAllowAsyncPreparation);
            if (!prepared) return false;
            const AnimRes& animRes = *prepared;

            if (!ResolveLegacyBoneInfoMap(animRes, meshComp, animComp.animResPath, animComp.mBoneInfoMap))
                return false;
            animComp.mBoneCount = static_cast<int>(animComp.mBoneInfoMap.size());
            const bool bHasSkeleton = animComp.mBoneCount > 0;

            animComp.mClips.clear();
            animComp.mClipNames.clear();
            for (int i = 0; i < static_cast<int>(animRes.clips.size()); ++i)
            {
                std::map<std::string, BoneInfo> clipBoneMap = animComp.mBoneInfoMap;
                animComp.mClips.emplace_back(animRes, i, clipBoneMap);
                animComp.mClipNames.push_back(animRes.clips[i].name);
            }

            if (animComp.mClips.empty())
            {
                HE_CORE_WARN("AnimationSystem: no clips found in '{0}'", animComp.animResPath);
                return false;
            }

            meshComp.mMesh->GetBoneInfoMap() = animComp.mBoneInfoMap;
            meshComp.mMesh->GetBoneCount() = animComp.mBoneCount;
            meshComp.mMesh->bAnimated = bHasSkeleton;
            meshComp.mMesh->bUseExternalBoneMatrices = false;

            return true;
        }

        static bool LoadSharedPausedPose(Level* level, AnimatorComponent& animComp,
            MeshComponent& meshComp, bool bAllowAsyncPreparation)
        {
            const std::string normalizedPath = Utils::NormalizeAssetPath(animComp.animResPath);
            const int requestedClipIndex = std::max(0, animComp.currentClipIndex);
            const std::string cacheKey =
                normalizedPath + "#" + Utils::NormalizeAssetPath(meshComp.mMesh->mSkeletonPath) +
                "#" + std::to_string(requestedClipIndex) +
                (animComp.bEnableRootMotion ? "#root-motion" : "#no-root-motion") +
                (animComp.bLockRootMotionXZ ? "#lock-xz" : "#free-xz");

            auto cacheIt = sSharedPausedPoseCache.find(cacheKey);
            if (cacheIt == sSharedPausedPoseCache.end())
            {
                const auto prepared = AcquireLegacyAnimRes(level,
                    normalizedPath, bAllowAsyncPreparation);
                if (!prepared) return false;
                const AnimRes& animRes = *prepared;

                if (animRes.clips.empty())
                {
                    HE_CORE_WARN("AnimationSystem: no clips found in '{0}'", normalizedPath);
                    return false;
                }

                SharedPausedPose cachedPose;
                if (!ResolveLegacyBoneInfoMap(animRes, meshComp, normalizedPath, cachedPose.BoneInfoMap))
                    return false;

                cachedPose.ClipIndex = std::clamp(
                    requestedClipIndex,
                    0,
                    static_cast<int>(animRes.clips.size()) - 1);
                Animation pausedAnimation(animRes, cachedPose.ClipIndex, cachedPose.BoneInfoMap);
                Animator pausedAnimator(&pausedAnimation);
                ConfigureAnimatorRootMotion(pausedAnimator, animComp);
                pausedAnimator.UpdateAnimationAbsolute(0.0f);

                cachedPose.BoneCount = static_cast<int>(cachedPose.BoneInfoMap.size());
                CopyBonePalette(
                    cachedPose.BoneMatrices,
                    pausedAnimator.GetFinalBoneMatrices(),
                    cachedPose.BoneCount);
                cacheIt = sSharedPausedPoseCache.emplace(cacheKey, std::move(cachedPose)).first;
            }

            const SharedPausedPose& cachedPose = cacheIt->second;
            animComp.mClips.clear();
            animComp.mClipNames.clear();
            animComp.mBoneInfoMap = cachedPose.BoneInfoMap;
            animComp.mBoneCount = cachedPose.BoneCount;
            animComp.mLastClipIndex = cachedPose.ClipIndex;
            animComp.mProgress = 0.0f;

            meshComp.mMesh->GetBoneInfoMap() = cachedPose.BoneInfoMap;
            meshComp.mMesh->GetBoneCount() = cachedPose.BoneCount;
            meshComp.mMesh->bAnimated = cachedPose.BoneCount > 0;
            meshComp.mMesh->bPlayAnim = false;
            meshComp.mMesh->bUseExternalBoneMatrices = true;
            meshComp.mMesh->mExternalBoneMatrices = cachedPose.BoneMatrices;
            meshComp.mMesh->mAnimator.ClearAnimation();
            meshComp.mMesh->mAnimation = Animation();
            return true;
        }
    }

    AnimationSystem::AnimationSystem(Level* level)
        : System(level)
    {
    }

    void AnimationSystem::OnRuntiemStart()
    {
        sSharedPausedPoseCache.clear();
        sControllerCache.clear();
        sFailedControllerDiagnostics.clear();
        sFailedLegacyDiagnostics.clear();

        auto view = mLevel->mRegistry.view<AnimatorComponent, MeshComponent>();
        for (auto e : view)
        {
            auto& animComp = mLevel->mRegistry.get<AnimatorComponent>(e);
            ResetRuntimeState(animComp);
        }

        // Resolve clips and the initial pose before the first gameplay frame.
        // A controller may parse several large animation assets; deferring that
        // work to the first input tick causes a visible hitch on movement.
        // Meshes that are still loading retain TickEntity's normal lazy path.
        for (auto e : view)
        {
            Entity entity = { e, mLevel };
            auto& mesh = entity.GetComponent<MeshComponent>().mMesh;
            if (!mesh) continue;
            if (const TransformComponent* transform = mLevel->mRegistry.try_get<TransformComponent>(e);
                transform && glm::dot(transform->Scale, transform->Scale) <= 1e-12f)
                continue;
            TickEntity(entity, Timestep(0.0f), true, false);

            auto& animator = entity.GetComponent<AnimatorComponent>();
            if (!animator.bLoaded || !animator.bPlay || !entity.HasComponent<TransformComponent>())
                continue;
            const glm::mat4 world = mLevel->GetWorldTransform(entity);
            const Entity camera = mLevel->GetPrimaryCameraEntity();
            if (camera && animator.DeformationDistance > 0 &&
                glm::distance(glm::vec3(world[3]), glm::vec3(mLevel->GetWorldTransform(camera)[3])) >= animator.DeformationDistance)
                continue;
            // Build per-instance morph/cloth topology at the initial pose. A zero
            // step leaves springs and facial playback at their authored start.
            UpdateCharacterDeformation(animator, *mesh, world, 0.0f, true);
        }
    }

    void AnimationSystem::OnUpdateRuntime(Timestep ts)
    {
		HE_PROFILE;
        ScopedAnimationCpuTiming timing(sAnimationCpuTimings.AnimationMilliseconds);
        static const bool bLogSlowInstances = []
        {
            const char* setting = std::getenv("HE_ANIMATION_INSTANCE_TIMINGS");
            return setting && setting[0] == '1';
        }();

        auto view = mLevel->mRegistry.view<AnimatorComponent, MeshComponent>();
        for (auto e : view)
        {
            if (const TransformComponent* transform = mLevel->mRegistry.try_get<TransformComponent>(e);
                transform && glm::dot(transform->Scale, transform->Scale) <= 1e-12f)
            {
                continue;
            }

            Entity entity = { e, mLevel };
            if (sAnimationCpuTimingEnabled) ++sAnimationCpuTimings.InstanceUpdates;
            const auto simulationTick = ResolveCharacterDeformationTick(
                entity.GetComponent<AnimatorComponent>(), static_cast<float>(ts), true);
            const bool bLoadedBefore = entity.GetComponent<AnimatorComponent>().bLoaded;
            const auto tickStart = bLogSlowInstances ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};
            TickEntity(entity, ts, true);
            if (bLogSlowInstances)
            {
                const double tickMilliseconds = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - tickStart).count();
                if (tickMilliseconds >= 2.0)
                    HE_CORE_WARN("[AnimationInstanceTiming] entity={} loadedBefore={} loadedAfter={} tickMs={} controller={}",
                        static_cast<uint32_t>(entity), bLoadedBefore,
                        entity.GetComponent<AnimatorComponent>().bLoaded,
                        tickMilliseconds, entity.GetComponent<AnimatorComponent>().controllerPath);
            }
            auto& animator = entity.GetComponent<AnimatorComponent>();
            auto& mesh = entity.GetComponent<MeshComponent>().mMesh;
            if (!mesh || !entity.HasComponent<TransformComponent>()) continue;
            const glm::mat4 world = mLevel->GetWorldTransform(entity);
            const bool playing = ResolveCharacterDeformationTick(animator, 0.0f, true).Active;
            const Entity camera = mLevel->GetPrimaryCameraEntity();
            const float cameraDistance = camera
                ? glm::distance(glm::vec3(world[3]), glm::vec3(mLevel->GetWorldTransform(camera)[3]))
                : 0.0f;
            const bool deformationActive = playing && (!camera || animator.DeformationDistance <= 0 ||
                cameraDistance < animator.DeformationDistance);
            const float secondaryDistance = animator.SecondaryMotionDistance > 0
                ? animator.SecondaryMotionDistance : animator.DeformationDistance;
            const bool secondaryActive = playing && (!camera || secondaryDistance <= 0 ||
                cameraDistance < secondaryDistance);
            const bool bodyConstraints = !animator.Feet.empty() || !animator.LookAt.empty() ||
                !animator.LimbTargets.empty();
            const bool poseConstraints = (deformationActive && bodyConstraints) ||
                (secondaryActive && !animator.SecondaryBones.empty());
            const bool hasController = !animator.controllerPath.empty() && animator.controllerPath != "None";
            if (!poseConstraints)
            {
                // Legacy Feet/LookAt/LimbTargets also publish an external palette.
                // Opt-out must restore its normal animator even without spring bones.
                if (mesh->bUseExternalBoneMatrices && !hasController)
                {
                    mesh->bUseExternalBoneMatrices = false;
                    mesh->mExternalBoneMatrices.clear();
                }
                animator.mSecondaryMotion.Reset();
                animator.mCharacterPose.Reset();
            }
            else
            {
                if (!secondaryActive) animator.mSecondaryMotion.Reset();
                if (!deformationActive) animator.mCharacterPose.Reset();
            }
            if (poseConstraints && simulationTick.DeltaSeconds > 0.0f && !animator.mClips.empty())
            {
                if (!hasController)
                    CopyBonePalette(mesh->mExternalBoneMatrices, mesh->mAnimator.GetFinalBoneMatrices(), animator.mBoneCount);
                mesh->bUseExternalBoneMatrices = true;
                if (auto* physics = mLevel->TryGetSystem<PhysicSystem3D>(); deformationActive && physics && !animator.Feet.empty())
                    animator.mCharacterPose.ApplyFeet(simulationTick.DeltaSeconds, world, mesh->mAnimation,
                        animator.Feet, animator.PelvisBone,
                        [physics](const glm::vec3& point, float& height)
                        { return physics->RaycastFloorHeight(point, .45f, .65f, height); },
                        !animator.GetBool("IsInAir", false), mesh->mExternalBoneMatrices);
                if (deformationActive)
                {
                    CharacterPose::ApplyLook(world, mesh->mAnimation, animator.LookAt, mesh->mExternalBoneMatrices);
                    for (auto limb : animator.LimbTargets)
                    {
                        if (!limb.WeightParameter.empty())
                            limb.Weight *= std::clamp(animator.GetFloat(limb.WeightParameter, 0.0f), 0.0f, 1.0f);
                        if (!std::isfinite(limb.Weight) || limb.Weight <= 0) continue;
                        glm::vec3 target = limb.WorldTarget;
                        if (!limb.TargetBone.empty())
                        {
                            glm::mat4 bone;
                            if (!CharacterPose::BoneMatrix(mesh->mAnimation, mesh->mExternalBoneMatrices, limb.TargetBone, bone)) continue;
                            target = glm::vec3(world * bone * glm::vec4(limb.TargetOffset, 1));
                        }
                        else if (!limb.TargetUUID.empty())
                        {
                            const UUID targetId(limb.TargetUUID);
                            if (!targetId.isValid()) continue;
                            const Entity targetEntity = mLevel->TryGetEntityByUUID(targetId);
                            if (!targetEntity) continue;
                            target = glm::vec3(mLevel->GetWorldTransform(targetEntity) * glm::vec4(limb.TargetOffset, 1));
                        }
                        CharacterPose::SolveLimbPose(mesh->mAnimation, limb, world, target, mesh->mExternalBoneMatrices);
                    }
                }
                if (secondaryActive)
                    animator.mSecondaryMotion.Apply(simulationTick.DeltaSeconds, world,
                        mesh->mAnimation, animator.SecondaryBones, animator.SecondaryColliders,
                        animator.SecondaryWind, mesh->mExternalBoneMatrices);
            }
            uint64_t deformationVertexUpdates = 0;
            {
                ScopedAnimationCpuTiming deformationTiming(sAnimationCpuTimings.DeformationMilliseconds);
                deformationVertexUpdates = UpdateCharacterDeformation(animator, *mesh, world, simulationTick.DeltaSeconds, deformationActive);
            }
            if (sAnimationCpuTimingEnabled && deformationVertexUpdates > 0)
            {
                ++sAnimationCpuTimings.DeformationInstanceUpdates;
                sAnimationCpuTimings.DeformationVertexUpdates += deformationVertexUpdates;
            }
        }
        // Resolve attachments after all character palettes, including IK and secondary motion.
        for (auto raw : mLevel->mRegistry.view<BoneAttachmentComponent, TransformComponent>())
        {
            Entity attachment{raw, mLevel};
            const auto& socket = attachment.GetComponent<BoneAttachmentComponent>();
            if (!socket.bEnabled || socket.Bone.empty()) continue;
            Entity parent = mLevel->GetParent(attachment);
            if (!parent || !parent.HasComponent<MeshComponent>()) continue;
            const auto& mesh = parent.GetComponent<MeshComponent>().mMesh;
            if (!mesh) continue;
            const auto& bones = mesh->mAnimation.GetBoneIDMap();
            const auto found = bones.find(socket.Bone);
            if (found == bones.end() || found->second.id < 0) continue;
            const auto& palette = mesh->bUseExternalBoneMatrices ? mesh->mExternalBoneMatrices
                : mesh->mAnimator.GetFinalBoneMatrices();
            if (static_cast<size_t>(found->second.id) >= palette.size()) continue;
            const glm::mat4 offset = glm::translate(glm::mat4(1), socket.Offset) *
                glm::toMat4(glm::quat(socket.Rotation)) * glm::scale(glm::mat4(1), socket.Scale);
            mLevel->SetWorldTransform(attachment, mLevel->GetWorldTransform(parent) *
                palette[found->second.id] * glm::inverse(found->second.offset) * offset);
        }
    }

    void AnimationSystem::OnRuntimeStop()
    {
        sSharedPausedPoseCache.clear();
        sControllerCache.clear();
        sFailedControllerDiagnostics.clear();
        sFailedLegacyDiagnostics.clear();

        auto view = mLevel->mRegistry.view<AnimatorComponent, MeshComponent>();
        for (auto e : view)
        {
            auto& animComp = mLevel->mRegistry.get<AnimatorComponent>(e);
            auto& meshComp = mLevel->mRegistry.get<MeshComponent>(e);
            if (meshComp.mMesh)
            {
                if (animComp.mDeformationWasActive)
                    for (auto& section : meshComp.mMesh->mSubMeshes)
                        if (section && section->GetDeformationRevision() > 0)
                        {
                            section->BeginDeformation();
                            section->ApplyMorphWeights({});
                            section->EndDeformation(true);
                        }
                animComp.mClothSimulation.clear();
                animComp.mCharacterPose.Reset();
                animComp.mFacialPlayer.Reset();
                animComp.mDeformationWasActive = false;
                meshComp.mMesh->bPlayAnim = false;
                meshComp.mMesh->mAnimator.ClearAnimation();
                meshComp.mMesh->bUseExternalBoneMatrices = false;
                meshComp.mMesh->mExternalBoneMatrices.clear();
            }
            animComp.mProgress = 0.0f;
        }
    }

#if WITH_EDITOR
    void AnimationSystem::OnUpdateEditor(Timestep ts, EditorCamera& camera)
    {
        auto view = mLevel->mRegistry.view<AnimatorComponent, MeshComponent>();
        for (auto e : view)
        {
            Entity entity = { e, mLevel };
            const auto simulationTick = ResolveCharacterDeformationTick(
                entity.GetComponent<AnimatorComponent>(), static_cast<float>(ts), true);
            TickEntity(entity, ts, false);
            auto& animator = entity.GetComponent<AnimatorComponent>();
            auto& mesh = entity.GetComponent<MeshComponent>().mMesh;
            if (mesh)
            {
                const bool playingClip = ResolveCharacterDeformationTick(animator, 0.0f, true).Active &&
                    !animator.mClips.empty() && entity.HasComponent<TransformComponent>();
                if (playingClip)
                    UpdateCharacterDeformation(animator, *mesh, mLevel->GetWorldTransform(entity),
                        simulationTick.DeltaSeconds, true);
                else
                    PreviewCharacterDeformation(animator, *mesh);
            }
        }
    }
#endif

    void AnimationSystem::TickEntity(Entity entity, Timestep ts,
        bool bRuntime, bool bAllowAsyncPreparation)
    {
        auto& animComp = entity.GetComponent<AnimatorComponent>();
        auto& meshComp = entity.GetComponent<MeshComponent>();
        // Consume even when loading/stopped paths return before animation evaluation.
        const float requestedStep = std::exchange(animComp.mStepSeconds, 0.0f);

        if (!meshComp.mMesh)
            return;

        if (!animComp.bLoaded)
        {
            ResetRuntimeState(animComp);

            const std::string normalizedControllerPath = Utils::NormalizeAssetPath(animComp.controllerPath);
            if (!normalizedControllerPath.empty() && normalizedControllerPath != "None")
            {
                if (!LoadControllerForEntity(entity, animComp, meshComp,
                    bRuntime && bAllowAsyncPreparation))
                    return;

                animComp.bLoaded = true;
                return;
            }

            if (animComp.animResPath.empty() || animComp.animResPath == "None")
            {
                ApplyBindPose(animComp, meshComp);
                animComp.bLoaded = true;
                return;
            }

            if (!animComp.bPlay)
            {
                if (!LoadSharedPausedPose(entity.GetLevel(), animComp, meshComp,
                    bRuntime && bAllowAsyncPreparation))
                    return;

                animComp.bLoaded = true;
                return;
            }

            if (!LoadLegacyAnimRes(entity.GetLevel(), animComp, meshComp,
                bRuntime && bAllowAsyncPreparation))
                return;

            animComp.mLastClipIndex = -1;
            animComp.bLoaded = true;
        }

        if (animComp.mClips.empty())
        {
            if (!animComp.bPlay &&
                meshComp.mMesh->bUseExternalBoneMatrices &&
                !meshComp.mMesh->mExternalBoneMatrices.empty())
            {
                meshComp.mMesh->bPlayAnim = false;
                return;
            }

            ApplyBindPose(animComp, meshComp);
            return;
        }

        const std::string normalizedControllerPath = Utils::NormalizeAssetPath(animComp.controllerPath);
        const bool bUseController = !normalizedControllerPath.empty() && normalizedControllerPath != "None";

        if (bRuntime && bUseController)
        {
            meshComp.mMesh->bPlayAnim = animComp.bPlay && meshComp.mMesh->bAnimated;
            meshComp.mMesh->mAnimPlaySpeed = 1.0f;
        }
        else
        {
            meshComp.mMesh->bPlayAnim = animComp.bPlay && meshComp.mMesh->bAnimated;
            meshComp.mMesh->mAnimPlaySpeed = animComp.playSpeed;
        }

        if (!meshComp.mMesh->bPlayAnim)
        {
            if (animComp.bHoldFinalPose) return;
            ApplyBindPose(animComp, meshComp);
            return;
        }

        animComp.mAnimationEvents = Json::array();
        const float dt = (animComp.bPaused ? requestedStep : static_cast<float>(ts)) * animComp.playSpeed;
        if (dt == 0.0f) return;
        const auto collectEvents = [&](Animator& animator, int clipIndex)
        {
            for (const auto& fired : animator.ConsumeEvents())
                animComp.mAnimationEvents.push_back({{"id", fired.Event.id}, {"name", fired.Event.name},
                    {"timeStamp", fired.Event.timeStamp}, {"payload", fired.Event.payload},
                    {"cycle", fired.Cycle}, {"clipIndex", clipIndex}});
        };

        if (!bUseController)
        {
            if (animComp.currentClipIndex < 0)
            {
                ApplyBindPose(animComp, meshComp);
                return;
            }

            const int clampedIndex = std::clamp(animComp.currentClipIndex, 0,
                static_cast<int>(animComp.mClips.size()) - 1);
            animComp.currentClipIndex = clampedIndex;

            if (animComp.currentClipIndex != animComp.mLastClipIndex)
            {
                meshComp.mMesh->mAnimation = animComp.mClips[animComp.currentClipIndex];
                meshComp.mMesh->mAnimator = Animator(&meshComp.mMesh->mAnimation);
                ConfigureAnimatorRootMotion(meshComp.mMesh->mAnimator, animComp);
                animComp.mLastClipIndex = animComp.currentClipIndex;
            }

            meshComp.mMesh->mAnimator.SetLoop(animComp.bLoop);
            meshComp.mMesh->mAnimator.UpdateAnimation(dt);
            collectEvents(meshComp.mMesh->mAnimator, animComp.currentClipIndex);
            animComp.mProgress = meshComp.mMesh->mAnimator.GetProgress();
            if (!(bRuntime && bUseController) && !animComp.bLoop && animComp.bPlay && meshComp.mMesh->mAnimator.HasFinished())
            {
                animComp.bHoldFinalPose = true;
                animComp.bPlay = false;
                meshComp.mMesh->bPlayAnim = false;
            }
            return;
        }

        meshComp.mMesh->bUseExternalBoneMatrices = true;

        if (animComp.GetCurrentClipIndex() < 0 || animComp.GetCurrentClipIndex() >= static_cast<int>(animComp.mClips.size()))
        {
            animComp.mInTransition = false;
            animComp.mTransitionFromAnimator.ClearAnimation();
            animComp.mTransitionToAnimator.ClearAnimation();
            ApplyBindPose(animComp, meshComp);
            return;
        }

        if (!animComp.mInTransition)
        {
            for (const auto& transition : animComp.mControllerRes.transitions)
            {
                if (transition.fromNodeId != animComp.mCurrentNodeId)
                    continue;
                if (!animComp.mNodeToClipIndex.count(transition.toNodeId))
                    continue;

                bool allPass = true;
                for (const auto& condition : transition.conditions)
                {
                    if (!animComp.EvaluateTransitionCondition(condition))
                    {
                        allPass = false;
                        break;
                    }
                }

                if (allPass)
                {
                    animComp.mInTransition = true;
                    animComp.mTransitionTime = 0.0f;
                    animComp.mTransitionDuration = std::max(0.0001f, transition.duration);
                    animComp.mTransitionFromClipIndex = animComp.GetCurrentClipIndex();
                    animComp.mTransitionToClipIndex = animComp.mNodeToClipIndex[transition.toNodeId];
                    animComp.mTargetNodeId = transition.toNodeId;

					animComp.mTransitionFromAnimator = meshComp.mMesh->mAnimator;
					ConfigureAnimatorRootMotion(animComp.mTransitionFromAnimator, animComp);
					Animation* toClip = &animComp.mClips[animComp.mTransitionToClipIndex];
					animComp.mTransitionToAnimator = Animator(toClip);
					ConfigureAnimatorRootMotion(animComp.mTransitionToAnimator, animComp);
					animComp.mTransitionToAnimator.Reset();
					break;
				}
			}
		}

        if (animComp.mInTransition)
        {
            animComp.mTransitionTime += dt;
            animComp.mTransitionFromAnimator.SetLoop(animComp.bLoop);
            animComp.mTransitionToAnimator.SetLoop(animComp.bLoop);
            if (!kFreezeFromPoseDuringTransition)
                animComp.mTransitionFromAnimator.UpdateAnimation(dt);
            animComp.mTransitionToAnimator.UpdateAnimation(dt);
            ApplyMotionBlend(animComp.mTransitionToAnimator, animComp.mTargetNodeId, animComp.mControllerRes,
                animComp.mNodeToClipIndex, animComp.mClips, animComp.mParameters, animComp.bLoop,
                animComp.bEnableRootMotion, animComp.bLockRootMotionXZ);
            // Transition target owns events; the outgoing pose cannot repeat actions.
            (void)animComp.mTransitionFromAnimator.ConsumeEvents();
            collectEvents(animComp.mTransitionToAnimator, animComp.mTransitionToClipIndex);

            const float alpha = std::clamp(animComp.mTransitionTime / animComp.mTransitionDuration, 0.0f, 1.0f);
            const auto& fromMatrices = animComp.mTransitionFromAnimator.GetFinalBoneMatrices();
            const auto& toMatrices = animComp.mTransitionToAnimator.GetFinalBoneMatrices();
            if (alpha <= kBlendEdgeEpsilon)
                CopyBonePalette(meshComp.mMesh->mExternalBoneMatrices, fromMatrices, animComp.mBoneCount);
            else if (alpha >= 1.0f - kBlendEdgeEpsilon)
                CopyBonePalette(meshComp.mMesh->mExternalBoneMatrices, toMatrices, animComp.mBoneCount);
            else
            {
                const auto blended = Animator::BlendFinalMatrices(
                    animComp.mTransitionFromAnimator, animComp.mTransitionToAnimator, alpha);
                CopyBonePalette(meshComp.mMesh->mExternalBoneMatrices, blended, animComp.mBoneCount);
            }

            animComp.mProgress = alpha;

            if (alpha >= 1.0f)
            {
                animComp.mInTransition = false;
                animComp.mCurrentNodeId = animComp.mTargetNodeId;
                animComp.mCurrentStateName = GetNodeName(animComp.mControllerRes, animComp.mCurrentNodeId);
                meshComp.mMesh->mAnimation = animComp.mClips[animComp.GetCurrentClipIndex()];
                meshComp.mMesh->mAnimator = animComp.mTransitionToAnimator;
                animComp.mLastClipIndex = animComp.GetCurrentClipIndex();
            }
        }
        else
        {
            if (animComp.GetCurrentClipIndex() != animComp.mLastClipIndex)
            {
                meshComp.mMesh->mAnimation = animComp.mClips[animComp.GetCurrentClipIndex()];
                meshComp.mMesh->mAnimator = Animator(&meshComp.mMesh->mAnimation);
                ConfigureAnimatorRootMotion(meshComp.mMesh->mAnimator, animComp);
                animComp.mLastClipIndex = animComp.GetCurrentClipIndex();
            }

            meshComp.mMesh->mAnimator.SetLoop(animComp.bLoop);
            meshComp.mMesh->mAnimator.UpdateAnimation(dt);
            ApplyMotionBlend(meshComp.mMesh->mAnimator, animComp.mCurrentNodeId, animComp.mControllerRes,
                animComp.mNodeToClipIndex, animComp.mClips, animComp.mParameters, animComp.bLoop,
                animComp.bEnableRootMotion, animComp.bLockRootMotionXZ);
            CopyBonePalette(
                meshComp.mMesh->mExternalBoneMatrices,
                meshComp.mMesh->mAnimator.GetFinalBoneMatrices(),
                animComp.mBoneCount);
            collectEvents(meshComp.mMesh->mAnimator, animComp.GetCurrentClipIndex());
            animComp.mProgress = meshComp.mMesh->mAnimator.GetProgress();
        }

        if (!animComp.mInTransition && !animComp.bLoop && meshComp.mMesh->mAnimator.HasFinished())
        {
            animComp.bHoldFinalPose = true;
            animComp.bPlay = false;
            meshComp.mMesh->bPlayAnim = false;
        }

        for (const auto& [paramName, paramType] : animComp.mParameterTypes)
        {
            if (paramType == AnimControllerParamType::Trigger)
                animComp.mParameters[paramName] = false;
        }
    }
}
