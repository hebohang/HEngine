#include "Hepch.h"

#include "Runtime/EcsFramework/System/Animation/AnimationSystem.h"
#include "Runtime/EcsFramework/Component/Animation/AnimatorComponent.h"
#include "Runtime/EcsFramework/Component/Basic/TagComponent.h"
#include "Runtime/EcsFramework/Component/Mesh/MeshComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/Resource/ResType/Animation.h"
#include "Runtime/Resource/ResType/AnimController.h"
#include "Runtime/Resource/ResType/Skeleton.h"
#include "Runtime/Core/UUID.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Core/Meta/Json.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

namespace HEngine
{
    namespace
    {
        struct BoneBlendOffsetCache
        {
            std::vector<glm::mat4> offsets;
            std::vector<glm::mat4> invOffsets;
        };

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

        static std::unordered_map<UUID, BoneBlendOffsetCache> sBoneBlendOffsetCache;
        static std::unordered_map<std::string, SharedPausedPose> sSharedPausedPoseCache;
        static std::unordered_map<std::string, CachedControllerData> sControllerCache;

        constexpr float kBlendEdgeEpsilon = 1e-4f;
        constexpr bool kFreezeFromPoseDuringTransition = true;

        static void ConfigureAnimatorRootMotion(Animator& animator, const AnimatorComponent& animComp)
        {
            animator.SetRootMotionOptions(animComp.bEnableRootMotion, animComp.bLockRootMotionXZ);
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
            out.version = j.value("version", 1);
            out.name = j.value("name", "");
            out.meshResPath = j.value("meshResPath", "");
            out.targetSkeletonPath = j.value("targetSkeletonPath", "");

            out.boneInfos.clear();
            if (j.contains("boneInfos") && j["boneInfos"].is_array())
            {
                for (const auto& biJ : j["boneInfos"])
                {
                    AnimBoneInfoRes bi;
                    bi.name = biJ.value("name", "");
                    bi.id = biJ.value("id", 0);
                    if (biJ.contains("offsetMatrix") && biJ["offsetMatrix"].is_array())
                        bi.offsetMatrix = biJ["offsetMatrix"].get<std::vector<float>>();
                    out.boneInfos.push_back(std::move(bi));
                }
            }

            out.clips.clear();
            if (j.contains("clips") && j["clips"].is_array())
            {
                for (const auto& clipJ : j["clips"])
                {
                    AnimClipRes clip;
                    clip.name = clipJ.value("name", "");
                    clip.duration = clipJ.value("duration", 0.0f);
                    clip.ticksPerSecond = clipJ.value("ticksPerSecond", 24.0f);

                    if (clipJ.contains("channels") && clipJ["channels"].is_array())
                    {
                        for (const auto& chJ : clipJ["channels"])
                        {
                            AnimBoneChannelRes ch;
                            ch.name = chJ.value("name", "");

                            if (chJ.contains("positionKeys") && chJ["positionKeys"].is_array())
                            {
                                for (const auto& kJ : chJ["positionKeys"])
                                {
                                    AnimKeyPositionRes k;
                                    k.px = kJ.value("px", 0.0f);
                                    k.py = kJ.value("py", 0.0f);
                                    k.pz = kJ.value("pz", 0.0f);
                                    k.timeStamp = kJ.value("timeStamp", 0.0f);
                                    ch.positionKeys.push_back(k);
                                }
                            }

                            if (chJ.contains("rotationKeys") && chJ["rotationKeys"].is_array())
                            {
                                for (const auto& kJ : chJ["rotationKeys"])
                                {
                                    AnimKeyRotationRes k;
                                    k.qw = kJ.value("qw", 1.0f);
                                    k.qx = kJ.value("qx", 0.0f);
                                    k.qy = kJ.value("qy", 0.0f);
                                    k.qz = kJ.value("qz", 0.0f);
                                    k.timeStamp = kJ.value("timeStamp", 0.0f);
                                    ch.rotationKeys.push_back(k);
                                }
                            }

                            if (chJ.contains("scaleKeys") && chJ["scaleKeys"].is_array())
                            {
                                for (const auto& kJ : chJ["scaleKeys"])
                                {
                                    AnimKeyScaleRes k;
                                    k.sx = kJ.value("sx", 1.0f);
                                    k.sy = kJ.value("sy", 1.0f);
                                    k.sz = kJ.value("sz", 1.0f);
                                    k.timeStamp = kJ.value("timeStamp", 0.0f);
                                    ch.scaleKeys.push_back(k);
                                }
                            }

                            clip.channels.push_back(std::move(ch));
                        }
                    }

                    out.clips.push_back(std::move(clip));
                }
            }

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
            for (const auto& transition : controller.transitions)
            {
                if (transition.fromNodeId == controller.entryNodeId && nodeToClip.count(transition.toNodeId))
                    return transition.toNodeId;
            }

            if (nodeToClip.count(controller.entryNodeId))
                return controller.entryNodeId;

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

        static glm::mat4 BlendTransformMatrix(const glm::mat4& from, const glm::mat4& to, float alpha)
        {
            glm::vec3 fromScale(1.0f);
            glm::quat fromRotation(1.0f, 0.0f, 0.0f, 0.0f);
            glm::vec3 fromTranslation(0.0f);
            glm::vec3 fromSkew(0.0f);
            glm::vec4 fromPerspective(0.0f);

            glm::vec3 toScale(1.0f);
            glm::quat toRotation(1.0f, 0.0f, 0.0f, 0.0f);
            glm::vec3 toTranslation(0.0f);
            glm::vec3 toSkew(0.0f);
            glm::vec4 toPerspective(0.0f);

            const bool bFromOk = glm::decompose(from, fromScale, fromRotation, fromTranslation, fromSkew, fromPerspective);
            const bool bToOk = glm::decompose(to, toScale, toRotation, toTranslation, toSkew, toPerspective);
            if (!bFromOk || !bToOk)
                return from * (1.0f - alpha) + to * alpha;

            if (glm::dot(fromRotation, toRotation) < 0.0f)
                toRotation = -toRotation;

            const glm::vec3 blendedTranslation = glm::mix(fromTranslation, toTranslation, alpha);
            const glm::quat blendedRotation = glm::normalize(glm::slerp(fromRotation, toRotation, alpha));
            const glm::vec3 blendedScale = glm::mix(fromScale, toScale, alpha);

            return glm::translate(glm::mat4(1.0f), blendedTranslation)
                * glm::mat4_cast(blendedRotation)
                * glm::scale(glm::mat4(1.0f), blendedScale);
        }

        static BoneBlendOffsetCache BuildOffsetByBoneIndex(const std::map<std::string, BoneInfo>& boneInfoMap, size_t count)
        {
            BoneBlendOffsetCache cache;
            cache.offsets.assign(count, glm::mat4(1.0f));
            cache.invOffsets.assign(count, glm::mat4(1.0f));

            for (const auto& [boneName, boneInfo] : boneInfoMap)
            {
                (void)boneName;
                if (boneInfo.id < 0)
                    continue;

                const size_t index = static_cast<size_t>(boneInfo.id);
                if (index >= cache.offsets.size())
                    continue;

                cache.offsets[index] = boneInfo.offset;
                cache.invOffsets[index] = glm::inverse(boneInfo.offset);
            }

            return cache;
        }

        static const BoneBlendOffsetCache& GetOrBuildBoneBlendOffsetCache(
            const UUID& entityUUID,
            const std::map<std::string, BoneInfo>& boneInfoMap,
            size_t count)
        {
            auto& cache = sBoneBlendOffsetCache[entityUUID];
            if (cache.offsets.size() < count || cache.invOffsets.size() < count)
                cache = BuildOffsetByBoneIndex(boneInfoMap, count);

            return cache;
        }

        static void BlendSkinnedBoneMatricesInPlace(
            std::vector<glm::mat4>& out,
            const std::vector<glm::mat4>& a,
            const std::vector<glm::mat4>& b,
            const BoneBlendOffsetCache& offsetCache,
            size_t maxCount,
            float t)
        {
            const size_t count = std::min(std::max(a.size(), b.size()), maxCount);
            if (out.size() != count)
                out.resize(count, glm::mat4(1.0f));

            for (size_t i = 0; i < count; ++i)
            {
                const bool bHasA = i < a.size();
                const bool bHasB = i < b.size();
                if (!bHasA)
                {
                    out[i] = b[i];
                    continue;
                }

                if (!bHasB)
                {
                    out[i] = a[i];
                    continue;
                }

                const glm::mat4 offset = i < offsetCache.offsets.size() ? offsetCache.offsets[i] : glm::mat4(1.0f);
                const glm::mat4 invOffset = i < offsetCache.invOffsets.size() ? offsetCache.invOffsets[i] : glm::mat4(1.0f);

                const glm::mat4 globalA = a[i] * invOffset;
                const glm::mat4 globalB = b[i] * invOffset;
                const glm::mat4 blendedGlobal = BlendTransformMatrix(globalA, globalB, t);
                out[i] = blendedGlobal * offset;
            }
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
            animComp.mInTransition = false;
            animComp.mTransitionTime = 0.0f;
            animComp.mTransitionDuration = 0.0f;
            animComp.mTransitionFromClipIndex = -1;
            animComp.mTransitionToClipIndex = -1;
            animComp.mTransitionFromAnimator.ClearAnimation();
            animComp.mTransitionToAnimator.ClearAnimation();
        }

        static bool LoadControllerForEntity(Entity entity, AnimatorComponent& animComp, MeshComponent& meshComp)
        {
            const std::string entityName = EntityNameSafe(entity);
            const std::string controllerPath = Utils::NormalizeAssetPath(animComp.controllerPath);
            const std::string meshSkeletonPath = Utils::NormalizeAssetPath(meshComp.mMesh->mSkeletonPath);

            auto cacheIt = sControllerCache.find(controllerPath);
            if (cacheIt == sControllerCache.end())
            {
                const std::filesystem::path controllerFullPath = ConfigManager::GetFullPath(controllerPath);
                if (!std::filesystem::exists(controllerFullPath))
                {
                    HE_CORE_ERROR("AnimationSystem: entity='{0}', component='AnimatorComponent', missing controllerPath='{1}'", entityName, controllerPath);
                    return false;
                }

                CachedControllerData cachedData;
                std::string controllerError;
                if (!LoadAnimControllerRes(controllerFullPath, cachedData.Controller, &controllerError))
                {
                    HE_CORE_ERROR("AnimationSystem: entity='{0}', component='AnimatorComponent', load controller failed '{1}': {2}",
                        entityName, controllerPath, controllerError);
                    return false;
                }

                cachedData.Controller.targetSkeletonPath =
                    Utils::NormalizeAssetPath(cachedData.Controller.targetSkeletonPath);
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
                    const std::filesystem::path clipFullPath = ConfigManager::GetFullPath(clipPath);
                    if (!std::filesystem::exists(clipFullPath))
                    {
                        HE_CORE_ERROR("AnimationSystem: entity='{0}', component='AnimatorComponent', state='{1}', missing clipPath='{2}'",
                            entityName, node.name, clipPath);
                        continue;
                    }

                    AnimRes clipRes;
                    try
                    {
                        ParseAnimResFromJson(Utils::LoadJsonFile(clipFullPath), clipRes);
                    }
                    catch (const std::exception& ex)
                    {
                        HE_CORE_ERROR("AnimationSystem: entity='{0}', component='AnimatorComponent', parse clip failed '{1}': {2}",
                            entityName, clipPath, ex.what());
                        continue;
                    }

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
            animComp.currentClipIndex =
                animComp.mNodeToClipIndex[cachedData.InitialNodeId];
            animComp.mCurrentStateName =
                GetNodeName(cachedData.Controller, cachedData.InitialNodeId);
            meshComp.mMesh->mAnimation = animComp.mClips[animComp.currentClipIndex];
            meshComp.mMesh->mAnimator = Animator(&meshComp.mMesh->mAnimation);
            ConfigureAnimatorRootMotion(meshComp.mMesh->mAnimator, animComp);
            meshComp.mMesh->bUseExternalBoneMatrices = true;
            CopyBonePalette(
                meshComp.mMesh->mExternalBoneMatrices,
                meshComp.mMesh->mAnimator.GetFinalBoneMatrices(),
                animComp.mBoneCount);

            return true;
        }

        static bool LoadLegacyAnimRes(AnimatorComponent& animComp, MeshComponent& meshComp)
        {
            auto fullPath = ConfigManager::GetFullPath(animComp.animResPath);
            if (!std::filesystem::exists(fullPath))
            {
                HE_CORE_WARN("AnimationSystem: .hanim file not found: {0}", animComp.animResPath);
                return false;
            }

            AnimRes animRes;
            try
            {
                ParseAnimResFromJson(Utils::LoadJsonFile(fullPath), animRes);
            }
            catch (const std::exception& ex)
            {
                HE_CORE_ERROR("AnimationSystem: failed to deserialize '{0}': {1}",
                    animComp.animResPath, ex.what());
                return false;
            }

            animComp.mBoneInfoMap.clear();
            for (const auto& bi : animRes.boneInfos)
            {
                BoneInfo info;
                info.id = bi.id;
                info.offset = bi.offsetMatrix.size() == 16 ? glm::make_mat4(bi.offsetMatrix.data()) : glm::mat4(1.0f);
                animComp.mBoneInfoMap[bi.name] = info;
            }
            animComp.mBoneCount = static_cast<int>(animRes.boneInfos.size());
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

            if (!bHasSkeleton)
            {
                HE_CORE_WARN("AnimationSystem: '{0}' has no boneInfos (node animation only). Current runtime only supports skeletal skinning animation.",
                    animComp.animResPath);
            }

            return true;
        }

        static bool LoadSharedPausedPose(AnimatorComponent& animComp, MeshComponent& meshComp)
        {
            const std::string normalizedPath = Utils::NormalizeAssetPath(animComp.animResPath);
            const int requestedClipIndex = std::max(0, animComp.currentClipIndex);
            const std::string cacheKey =
                normalizedPath + "#" + std::to_string(requestedClipIndex) +
                (animComp.bEnableRootMotion ? "#root-motion" : "#no-root-motion") +
                (animComp.bLockRootMotionXZ ? "#lock-xz" : "#free-xz");

            auto cacheIt = sSharedPausedPoseCache.find(cacheKey);
            if (cacheIt == sSharedPausedPoseCache.end())
            {
                const std::filesystem::path fullPath = ConfigManager::GetFullPath(normalizedPath);
                if (!std::filesystem::exists(fullPath))
                {
                    HE_CORE_WARN("AnimationSystem: .hanim file not found: {0}", normalizedPath);
                    return false;
                }

                AnimRes animRes;
                try
                {
                    ParseAnimResFromJson(Utils::LoadJsonFile(fullPath), animRes);
                }
                catch (const std::exception& ex)
                {
                    HE_CORE_ERROR("AnimationSystem: failed to deserialize '{0}': {1}", normalizedPath, ex.what());
                    return false;
                }

                if (animRes.clips.empty())
                {
                    HE_CORE_WARN("AnimationSystem: no clips found in '{0}'", normalizedPath);
                    return false;
                }

                SharedPausedPose cachedPose;
                for (const auto& boneInfoRes : animRes.boneInfos)
                {
                    BoneInfo boneInfo;
                    boneInfo.id = boneInfoRes.id;
                    boneInfo.offset = boneInfoRes.offsetMatrix.size() == 16
                        ? glm::make_mat4(boneInfoRes.offsetMatrix.data())
                        : glm::mat4(1.0f);
                    cachedPose.BoneInfoMap[boneInfoRes.name] = boneInfo;
                }

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
        sBoneBlendOffsetCache.clear();
        sSharedPausedPoseCache.clear();
        sControllerCache.clear();

        auto view = mLevel->mRegistry.view<AnimatorComponent, MeshComponent>();
        for (auto e : view)
        {
            auto& animComp = mLevel->mRegistry.get<AnimatorComponent>(e);
            ResetRuntimeState(animComp);
        }
    }

    void AnimationSystem::OnUpdateRuntime(Timestep ts)
    {
		HE_PROFILE;

        auto view = mLevel->mRegistry.view<AnimatorComponent, MeshComponent>();
        for (auto e : view)
        {
            if (const TransformComponent* transform = mLevel->mRegistry.try_get<TransformComponent>(e);
                transform && glm::dot(transform->Scale, transform->Scale) <= 1e-12f)
            {
                continue;
            }

            Entity entity = { e, mLevel };
            TickEntity(entity, ts, true);
        }
    }

    void AnimationSystem::OnRuntimeStop()
    {
        sBoneBlendOffsetCache.clear();
        sSharedPausedPoseCache.clear();
        sControllerCache.clear();

        auto view = mLevel->mRegistry.view<AnimatorComponent, MeshComponent>();
        for (auto e : view)
        {
            auto& animComp = mLevel->mRegistry.get<AnimatorComponent>(e);
            auto& meshComp = mLevel->mRegistry.get<MeshComponent>(e);
            if (meshComp.mMesh)
            {
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
            TickEntity(entity, ts, false);
        }
    }
#endif

    void AnimationSystem::TickEntity(Entity entity, Timestep ts, bool bRuntime)
    {
        auto& animComp = entity.GetComponent<AnimatorComponent>();
        auto& meshComp = entity.GetComponent<MeshComponent>();

        if (!meshComp.mMesh)
            return;

        if (!animComp.bLoaded)
        {
            ResetRuntimeState(animComp);

            const std::string normalizedControllerPath = Utils::NormalizeAssetPath(animComp.controllerPath);
            if (!normalizedControllerPath.empty() && normalizedControllerPath != "None")
            {
                if (!LoadControllerForEntity(entity, animComp, meshComp))
                    return;

                const size_t cacheCount = static_cast<size_t>(std::max(0, animComp.mBoneCount));
                GetOrBuildBoneBlendOffsetCache(entity.UUID(), animComp.mBoneInfoMap, cacheCount);

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
                if (!LoadSharedPausedPose(animComp, meshComp))
                    return;

                animComp.bLoaded = true;
                return;
            }

            if (!LoadLegacyAnimRes(animComp, meshComp))
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
            meshComp.mMesh->bPlayAnim = meshComp.mMesh->bAnimated;
            meshComp.mMesh->mAnimPlaySpeed = 1.0f;
        }
        else
        {
            meshComp.mMesh->bPlayAnim = animComp.bPlay && meshComp.mMesh->bAnimated;
            meshComp.mMesh->mAnimPlaySpeed = animComp.playSpeed;
        }

        if (!meshComp.mMesh->bPlayAnim)
        {
            ApplyBindPose(animComp, meshComp);
            return;
        }

        const float dt = static_cast<float>(ts) * animComp.playSpeed;

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

            meshComp.mMesh->mAnimator.UpdateAnimation(dt);
            animComp.mProgress = meshComp.mMesh->mAnimator.GetProgress();
            if (!(bRuntime && bUseController) && !animComp.bLoop && animComp.bPlay && animComp.mProgress >= 0.999f)
            {
                animComp.bPlay = false;
                meshComp.mMesh->bPlayAnim = false;
            }
            return;
        }

        meshComp.mMesh->bUseExternalBoneMatrices = true;

        if (animComp.currentClipIndex < 0 || animComp.currentClipIndex >= static_cast<int>(animComp.mClips.size()))
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
                    animComp.mTransitionFromClipIndex = animComp.currentClipIndex;
                    animComp.mTransitionToClipIndex = animComp.mNodeToClipIndex[transition.toNodeId];
                    animComp.mTargetNodeId = transition.toNodeId;

					animComp.mTransitionFromAnimator = meshComp.mMesh->mAnimator;
					ConfigureAnimatorRootMotion(animComp.mTransitionFromAnimator, animComp);
					Animation* toClip = &animComp.mClips[animComp.mTransitionToClipIndex];
					animComp.mTransitionToAnimator = Animator(toClip);
					ConfigureAnimatorRootMotion(animComp.mTransitionToAnimator, animComp);
					animComp.mTransitionToAnimator.UpdateAnimationAbsolute(0.0f);
					break;
				}
			}
		}

        if (animComp.mInTransition)
        {
            animComp.mTransitionTime += dt;
            if (!kFreezeFromPoseDuringTransition)
                animComp.mTransitionFromAnimator.UpdateAnimation(dt);
            animComp.mTransitionToAnimator.UpdateAnimation(dt);

            const float alpha = std::clamp(animComp.mTransitionTime / animComp.mTransitionDuration, 0.0f, 1.0f);
            const auto& fromMatrices = animComp.mTransitionFromAnimator.GetFinalBoneMatrices();
            const auto& toMatrices = animComp.mTransitionToAnimator.GetFinalBoneMatrices();
            if (alpha <= kBlendEdgeEpsilon)
                CopyBonePalette(meshComp.mMesh->mExternalBoneMatrices, fromMatrices, animComp.mBoneCount);
            else if (alpha >= 1.0f - kBlendEdgeEpsilon)
                CopyBonePalette(meshComp.mMesh->mExternalBoneMatrices, toMatrices, animComp.mBoneCount);
            else
            {
                const size_t cacheCount = std::min(
                    std::max(fromMatrices.size(), toMatrices.size()),
                    static_cast<size_t>(std::max(0, animComp.mBoneCount)));
                const auto& offsetCache = GetOrBuildBoneBlendOffsetCache(entity.UUID(), animComp.mBoneInfoMap, cacheCount);
                BlendSkinnedBoneMatricesInPlace(
                    meshComp.mMesh->mExternalBoneMatrices,
                    fromMatrices,
                    toMatrices,
                    offsetCache,
                    cacheCount,
                    alpha);
            }

            animComp.mProgress = alpha;

            if (alpha >= 1.0f)
            {
                animComp.mInTransition = false;
                animComp.mCurrentNodeId = animComp.mTargetNodeId;
                animComp.currentClipIndex = animComp.mTransitionToClipIndex;
                animComp.mCurrentStateName = GetNodeName(animComp.mControllerRes, animComp.mCurrentNodeId);
                meshComp.mMesh->mAnimation = animComp.mClips[animComp.currentClipIndex];
                meshComp.mMesh->mAnimator = animComp.mTransitionToAnimator;
                animComp.mLastClipIndex = animComp.currentClipIndex;
            }
        }
        else
        {
            if (animComp.currentClipIndex != animComp.mLastClipIndex)
            {
                meshComp.mMesh->mAnimation = animComp.mClips[animComp.currentClipIndex];
                meshComp.mMesh->mAnimator = Animator(&meshComp.mMesh->mAnimation);
                ConfigureAnimatorRootMotion(meshComp.mMesh->mAnimator, animComp);
                animComp.mLastClipIndex = animComp.currentClipIndex;
            }

            meshComp.mMesh->mAnimator.UpdateAnimation(dt);
            CopyBonePalette(
                meshComp.mMesh->mExternalBoneMatrices,
                meshComp.mMesh->mAnimator.GetFinalBoneMatrices(),
                animComp.mBoneCount);
            animComp.mProgress = meshComp.mMesh->mAnimator.GetProgress();
        }

        for (const auto& [paramName, paramType] : animComp.mParameterTypes)
        {
            if (paramType == AnimControllerParamType::Trigger)
                animComp.mParameters[paramName] = false;
        }
    }
}
