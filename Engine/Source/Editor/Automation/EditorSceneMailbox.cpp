#include "Hepch.h"

#include "Editor/Automation/EditorSceneMailbox.h"
#include "Runtime/Core/AppFramework/Application.h"
#include "Runtime/DevTools/GM/GMService.h"
#include "Runtime/EcsFramework/Level/Level.h"
#include "Runtime/EcsFramework/Component/Basic/IDComponent.h"
#include "Runtime/EcsFramework/Component/Animation/AnimatorComponent.h"
#include "Runtime/EcsFramework/Component/Mesh/MeshComponent.h"
#include "Runtime/EcsFramework/System/Embree/EmbreeSystem.h"
#include "Runtime/Graphics/Mesh/Mesh.h"
#include "Runtime/Graphics/VFX/EffekseerRuntime.h"
#include "Runtime/Resource/ModeManager/ModeManager.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <set>
#include <stdexcept>
#include <system_error>

namespace HEngine
{
    namespace
    {
        bool HexId(const std::string& text)
        {
            return text.size() == 32 && std::all_of(text.begin(), text.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
        }
        bool Under(const std::filesystem::path& child, const std::filesystem::path& parent)
        {
            const auto relative = child.lexically_relative(parent);
            return !relative.empty() && relative != "." && !relative.is_absolute() &&
                *relative.begin() != "..";
        }
    }


    Json EditorSceneMailbox::VfxState() const
    {
        return {{"active",mVfxActive},{"cleanupFailed",mVfxCleanupFailed},{"ticket",mVfxTicket},
            {"completedTicket",mVfxCompletedTicket},{"remainingFrames",mVfxRemaining},
            {"retiring",mVfxRetiring},{"retired",mVfxRetired},
            {"retirementScope","SDK updates and CPU allocator zero plus fresh RenderWorld submission; not GPU fence retirement"},
            {"totalFrames",mVfxTotalFrames},{"fixedDeltaSeconds",1.0 / 60.0},
            {"ackScope","active Editor scene update and RenderWorld submission; not GPU fence completion"}};
    }

    void EditorSceneMailbox::SetVfxSceneContext(const void* scene, bool pending)
    {
        mVfxCurrentScene = scene;
        mVfxScenePending = pending;
    }

    void EditorSceneMailbox::BeginVfxFrame(const void* scene)
    {
        if (!mVfxActive) return;
        if (scene != mVfxOwnedScene || !ModeManager::IsEditState() || !ModeManager::b3DMode)
        {
            (void)ShutdownVfx();
            throw std::logic_error("Editor VFX diagnostic lost its edit/3D context");
        }
        // Override interactive realtime toggles while the bounded owner holds the scene.
        ModeManager::bRealtimePreview = false;
        if (HasVfxStep()) mVfxUpdateBefore = EffekseerRuntime::GetStats().UpdateSerial;
    }

    void EditorSceneMailbox::CompleteVfxFrame(const void* scene)
    {
        if (!HasVfxStep()) return;
        if (scene != mVfxOwnedScene || EffekseerRuntime::GetStats().UpdateSerial != mVfxUpdateBefore + 1)
        {
            (void)ShutdownVfx();
            throw std::logic_error("Editor VFX step did not update exactly once");
        }
        ++mVfxTotalFrames;
        if (--mVfxRemaining == 0 && !mVfxRetiring) mVfxCompletedTicket = mVfxTicket;
    }

    void EditorSceneMailbox::FinishVfxRetirement()
    {
        if (!mVfxRetiring || mVfxRemaining != 0) return;
        // Poll runs on the following frame, after the last requested host frame.
        const auto stats = EffekseerRuntime::GetStats();
        if (mVfxCurrentScene != mVfxOwnedScene || !ModeManager::IsEditState() || !ModeManager::b3DMode ||
            EffekseerRuntime::GetRenderRequirements().bHasActiveEffects ||
            stats.ActiveEffects.Status != VFXMetricStatus::Exact || stats.ActiveEffects.Current != 0 ||
            stats.ActiveCpuInstances.Status != VFXMetricStatus::Exact || stats.ActiveCpuInstances.Current != 0 ||
            stats.GpuAllocatedEmitters != 0 || stats.GpuReservedParticles != 0 || stats.GpuReservedTrails != 0)
        {
            mVfxCleanupFailed = true;
            throw std::runtime_error("Editor VFX bounded retirement did not reach an empty allocator/scene");
        }
        mVfxHandles.clear();
        mVfxCompletedTicket = mVfxTicket;
        mVfxRetiring = false;
        mVfxRetired = true;
        mVfxActive = false;
        mVfxOwnedScene = nullptr;
        ModeManager::bRealtimePreview = mVfxSavedRealtime;
    }

    bool EditorSceneMailbox::ShutdownVfx() noexcept
    {
        bool success = !mVfxCleanupFailed;
        for (const auto handle : mVfxHandles)
        {
            try { EffekseerRuntime::StopEffect(handle); }
            catch (...) { success = false; }
        }
        mVfxHandles.clear();
        mVfxRemaining = 0;
        if (mVfxActive) mVfxRetired = false; // Forced shutdown is stop-only, not retirement ACK.
        mVfxRetiring = false;
        mVfxOwnedScene = nullptr;
        if (mVfxActive) ModeManager::bRealtimePreview = mVfxSavedRealtime;
        mVfxActive = false;
        mVfxCleanupFailed = !success;
        if (!success) HE_CORE_ERROR("Editor VFX diagnostic cleanup failed");
        return success;
    }

    Json EditorSceneMailbox::VfxRequest(const Json& params)
    {
        const std::string action = params.at("action").get<std::string>();
        std::set<std::string> keys{"action"};
        if (action == "play") keys.insert({"path","seed"});
        else if (action == "stop" || action == "status") keys.insert("handle");
        else if (action == "step") keys.insert("frames");
        else if (action != "begin" && action != "end" && action != "state" && action != "describe" && action != "stats")
            throw std::invalid_argument("Unknown Editor VFX diagnostic action");
        if (params.size() != keys.size()) throw std::invalid_argument("Unexpected VFX parameters");
        for (const auto& [key,value] : params.items())
            if (!keys.contains(key)) throw std::invalid_argument("Unknown VFX parameter");
        if (action == "state") return VfxState();
        if (action == "describe") return EffekseerRuntime::Describe();
        if (action == "stats") return EffekseerRuntime::DescribeStats();
        if (action == "begin")
        {
            const auto stats = EffekseerRuntime::GetStats();
            if (!mVfxCurrentScene || mVfxScenePending || mVfxTotalFrames > 35970 ||
                stats.ActiveEffects.Status != VFXMetricStatus::Exact || stats.ActiveEffects.Current != 0 ||
                stats.ActiveCpuInstances.Status != VFXMetricStatus::Exact || stats.ActiveCpuInstances.Current != 0 ||
                stats.GpuAllocatedEmitters != 0 || stats.GpuReservedParticles != 0 || stats.GpuReservedTrails != 0 ||
                mVfxCleanupFailed || mVfxActive || !ModeManager::IsEditState() || !ModeManager::b3DMode ||
                !EffekseerRuntime::IsInitialized() || EffekseerRuntime::GetRenderRequirements().bHasActiveEffects)
                throw std::invalid_argument("VFX begin requires initialized empty 3D edit scene");
            mVfxSavedRealtime = ModeManager::bRealtimePreview;
            ModeManager::bRealtimePreview = false;
            mVfxActive = true;
            mVfxRetired = false;
            mVfxOwnedScene = mVfxCurrentScene;
            return VfxState();
        }
        if (!mVfxActive) throw std::invalid_argument("VFX diagnostic is not active");
        if (action == "end")
        {
            if (mVfxRetiring) return VfxState();
            if (mVfxRemaining) throw std::invalid_argument("Wait for pending VFX step before normal end");
            for (const auto handle : mVfxHandles) EffekseerRuntime::StopEffect(handle);
            mVfxRetiring = true;
            mVfxRemaining = 30;
            ++mVfxTicket;
            return VfxState();
        }
        if (mVfxRemaining) throw std::invalid_argument("VFX step pending; query state before mutation");
        if (action == "step")
        {
            if (!params.at("frames").is_number_integer() || params.at("frames") < 1 || params.at("frames") > 3600)
                throw std::invalid_argument("VFX frames must be integer 1..3600");
            const auto frames = params.at("frames").get<uint32_t>();
            if (mVfxTotalFrames + frames + 30 > 36000) throw std::invalid_argument("VFX session step budget exhausted");
            mVfxRemaining = frames;
            ++mVfxTicket;
            return VfxState();
        }
        if (action == "play")
        {
            if (mVfxHandles.size() >= 16) throw std::invalid_argument("VFX owned handle budget exhausted");
            if (!params.at("seed").is_number_integer() || params.at("seed") < Json(INT32_MIN) || params.at("seed") > Json(INT32_MAX))
                throw std::invalid_argument("VFX seed must be int32");
            const std::filesystem::path requested(params.at("path").get<std::string>());
            const auto project = std::filesystem::weakly_canonical(ConfigManager::GetInstance().GetRootFolder());
            const auto path = std::filesystem::weakly_canonical(project / requested);
            if (requested.is_absolute() || requested.generic_string().find(':') != std::string::npos ||
                !Under(path, project / "Assets") || path.extension() != ".efkpkg" || !std::filesystem::is_regular_file(path))
                throw std::invalid_argument("VFX requires existing Assets efkpkg");
            VFXPlayParams play;
            play.bOverrideRandomSeed = true;
            play.RandomSeed = params.at("seed").get<int32_t>();
            const auto handle = EffekseerRuntime::PlayEffect(path.lexically_relative(project).generic_string(),play);
            if (handle < 0) throw std::runtime_error("VFX play rejected; inspect capability and resource identity");
            try { mVfxHandles.insert(handle); }
            catch (...) { EffekseerRuntime::StopEffect(handle); throw; }
            return EffekseerRuntime::GetEffectStatus(handle);
        }
        if (!params.at("handle").is_number_integer() || params.at("handle") < 0 || params.at("handle") > Json(INT32_MAX))
            throw std::invalid_argument("VFX handle must be nonnegative int32");
        const auto handle = params.at("handle").get<int32_t>();
        if (!mVfxHandles.contains(handle)) throw std::invalid_argument("VFX handle is not owned by this diagnostic");
        if (action == "stop")
        {
            EffekseerRuntime::StopEffect(handle);
            mVfxHandles.erase(handle);
        }
        return EffekseerRuntime::GetEffectStatus(handle);
    }

    void EditorSceneMailbox::Initialize(bool boundedRun)
    {
#if HE_ENABLE_GM
        const char* directory = std::getenv("HE_EDITOR_SCENE_MAILBOX");
        if (!directory || !*directory) return;
        try
        {
            const char* token = std::getenv("HE_EDITOR_SCENE_TOKEN");
            const char* sceneRoot = std::getenv("HE_EDITOR_SCENE_ROOT");
            if (!boundedRun || !token || !HexId(token) || !sceneRoot)
                throw std::invalid_argument("Editor mailbox requires a bounded run, token and scratch scene root.");
            const auto project = std::filesystem::weakly_canonical(ConfigManager::GetInstance().GetRootFolder());
            const auto candidate = std::filesystem::weakly_canonical(std::filesystem::path(directory));
            const auto scratch = std::filesystem::weakly_canonical(std::filesystem::path(sceneRoot));
            if (!candidate.is_absolute() || !scratch.is_absolute() ||
                !Under(candidate, ConfigManager::GetInstance().GetEngineContentFolder().parent_path().parent_path() /
                    "out/agent/rendering-evidence/vfx-authoring") ||
                !Under(scratch, project / "Assets/Effects/Authoring/EditorAcceptance") ||
                scratch.filename() != token || !std::filesystem::is_directory(candidate / "requests") ||
                !std::filesystem::is_directory(candidate / "responses") ||
                std::filesystem::is_symlink(std::filesystem::symlink_status(candidate / "requests")) ||
                std::filesystem::is_symlink(std::filesystem::symlink_status(candidate / "responses")))
                throw std::invalid_argument("Editor mailbox paths must be precreated owned evidence/scratch directories.");
            mSceneRoot = scratch;
            (void)CheckedScenePath(Application::GetInstance().GetRunOptions().StartupScene);
            mDirectory = candidate;
            mToken = token;
            Utils::SaveJsonToFile(Json{{"protocolVersion",1},{"state","ready"},{"host","Editor"},
                {"startupScene",Application::GetInstance().GetRunOptions().StartupScene},{"maxRequests",256},
                {"maxRequestBytes",262144}}, mDirectory / "ready.json");
        }
        catch (const std::exception& error)
        {
            mDirectory.clear(); mToken.clear();
            HE_CORE_ERROR("Editor scene mailbox disabled: {0}", error.what());
        }
#else
        (void)boundedRun;
#endif
    }

    std::filesystem::path EditorSceneMailbox::CheckedScenePath(const std::string& path) const
    {
        const auto candidate = std::filesystem::weakly_canonical(ConfigManager::GetProjectFullPath(path));
        if (!Under(candidate, mSceneRoot) || candidate.extension() != ".he")
            throw std::invalid_argument("Editor validation may open/save only .he files inside its scratch root.");
        return candidate;
    }

    void EditorSceneMailbox::Poll(const std::function<void(const std::filesystem::path&)>& openScene,
        const std::function<Json(const Json&)>& preview,
        const std::function<Json(const Json&)>& material)
    {
#if HE_ENABLE_GM
        if (mDirectory.empty()) return;
        try
        {
            FinishVfxRetirement();
            auto& application = Application::GetInstance();
            if (mCaptureDueFrame && application.GetFrameNumber() >= mCaptureDueFrame)
            {
                mCaptureDueFrame = 0;
                try
                {
                    if (!application.RequestFrameCapture(mCapturePath, FrameCaptureSource::Automatic))
                        throw std::runtime_error("Editor presented-backbuffer capture could not be queued.");
                    mCaptureReceipt["state"] = "inFlight";
                }
                catch (const std::exception& error)
                {
                    // A failed capture request is a bounded operation failure;
                    // retain the mailbox so its owner can inspect and stop it.
                    mCaptureReceipt["state"] = "failed";
                    mCaptureReceipt["error"] = std::string(error.what()).substr(0, 1800);
                }
            }
            if (!mCaptureReceipt.is_null() && mCaptureReceipt.value("state", "") == "inFlight")
            {
                const auto& capture = application.GetFrameCaptureProgress();
                if (capture.State == RuntimeFrameCaptureState::Complete || capture.State == RuntimeFrameCaptureState::Failed)
                {
                    if (capture.OutputPath != mCapturePath || capture.Source != FrameCaptureSource::Automatic ||
                        (capture.State == RuntimeFrameCaptureState::Complete && capture.Submission == 0))
                        throw std::runtime_error("Editor capture completion identity differs from its owned request.");
                    mCaptureReceipt["state"] = capture.State == RuntimeFrameCaptureState::Complete ? "complete" : "failed";
                    mCaptureReceipt["error"] = capture.Error;
                    mCaptureReceipt["renderFrame"] = capture.RenderFrame;
                    mCaptureReceipt["submission"] = capture.Submission;
                    application.ClearFrameCapture();
                }
            }
            if (mClaimedRequest.empty())
            {
                if (mClaimCandidate.empty())
                {
                    std::size_t scanned = 0;
                    for (const auto& entry : std::filesystem::directory_iterator(mDirectory / "requests"))
                    {
                        if (++scanned > 512) throw std::runtime_error("Editor mailbox directory entry budget exceeded.");
                        if (entry.path().extension() != ".json") continue;
                        const auto id = entry.path().stem().string();
                        if (!HexId(id)) continue;
                        if (std::filesystem::is_symlink(entry.symlink_status()) || !entry.is_regular_file())
                            throw std::runtime_error("Editor mailbox request must be a regular file.");
                        mClaimCandidate = entry.path();
                        mClaimRetries = 0;
                        break;
                    }
                }
                if (mClaimCandidate.empty()) return;
                // Claim before opening, then read on the next frame. Publication
                // and subsequent access are separate filesystem operations.
                auto claimed = mClaimCandidate;
                claimed.replace_extension(".claimed");
                std::error_code claimError;
                std::filesystem::rename(mClaimCandidate, claimed, claimError);
                if (claimError)
                {
                    // Retry only this filesystem operation, at most three later
                    // frames. error_code is captured directly by rename, unlike
                    // errno observed after a utility has constructed an exception.
                    if (mClaimRetries < 3)
                    {
                        ++mClaimRetries;
                        HE_CORE_WARN("Editor mailbox claim deferred: id={0}, error={1}:{2} ({3}), retry={4}/3",
                            mClaimCandidate.stem().string(), claimError.category().name(), claimError.value(),
                            claimError.message(), mClaimRetries);
                        return;
                    }
                    throw std::filesystem::filesystem_error("Editor mailbox claim failed after 3 retries",
                        mClaimCandidate, claimed, claimError);
                }
                mClaimedRequest = std::move(claimed);
                mClaimCandidate.clear();
                mOpenRetries = 0;
                return;
            }
            const auto id = mClaimedRequest.stem().string();
            Json response{{"protocolVersion",1},{"requestId",id},{"ok",false}};
            bool bClose = false;
            try
            {
                Json request;
                errno = 0;
                try
                {
                    request = Utils::LoadJsonFile(mClaimedRequest, Utils::JsonLoadLimits{262144,48,50000}).Value;
                }
                catch (const std::exception& error)
                {
                    const int observedErrno = errno;
                    const std::string message = error.what();
                    if (message.starts_with("Failed to open JSON file:"))
                    {
                        // JsonUtils does not preserve the open syscall's error.
                        // Do not classify its post-exception errno as authoritative.
                        // Bound retries to this exact open-failure branch; even a
                        // permanent open error receives at most three later attempts.
                        if (mOpenRetries < 3)
                        {
                            ++mOpenRetries;
                            HE_CORE_WARN("Editor mailbox request open deferred: id={0}, postExceptionErrno={1} (not authoritative), retry={2}/3",
                                id, observedErrno, mOpenRetries);
                            return; // Retry only this claimed request on a later frame.
                        }
                        throw std::runtime_error(message + "; postExceptionErrno=" + std::to_string(observedErrno) +
                            " (not authoritative), retries=" + std::to_string(mOpenRetries));
                    }
                    throw; // Parse, bounds and semantic failures are never retried.
                }
                if (request.value("protocolVersion",0) != 1 || request.at("requestId") != id ||
                    request.at("token") != mToken || !request.at("params").is_object())
                    throw std::invalid_argument("Editor mailbox request identity or shape is invalid.");
                const std::string fingerprint = request.dump();
                if (mProcessed.contains(id))
                {
                    if (mProcessed.at(id) != fingerprint) throw std::invalid_argument("requestId reused with different content.");
                    std::filesystem::remove(mClaimedRequest);
                    mClaimedRequest.clear();
                    return; // Existing immutable response is the receipt for this replay.
                }
                if (mProcessed.size() >= 256) throw std::runtime_error("Editor mailbox request budget exceeded.");
                mProcessed[id] = fingerprint;
                const std::string method = request.at("method");
                const Json& params = request.at("params");
                static const std::set<std::string> allowed{
                    "scene.describe","scene.query","scene.plan","scene.apply","scene.diff","scene.validate",
                    "scene.save","scene.undo","scene.redo","scene.result","scene.export_prefab"};
                if (method == "session.stop")
                {
                    if (!params.empty()) throw std::invalid_argument("session.stop takes no parameters.");
                    if (!ShutdownVfx()) throw std::runtime_error("VFX cleanup failed before session stop");
                    response["result"] = {{"stopping",true}}; bClose = true;
                }
                else if (method == "editor.vfx")
                {
                    response["result"] = VfxRequest(params);
                }
                else if (method == "editor.open")
                {
                    if (params.size() != 1 || !params.contains("path")) throw std::invalid_argument("editor.open requires path only.");
                    if (mVfxActive) throw std::invalid_argument("End VFX diagnostic before changing scene");
                    openScene(CheckedScenePath(params.at("path")));
                    response["result"] = {{"queued",true}};
                }
                else if (method == "editor.material")
                {
                    if (mVfxActive) throw std::invalid_argument("End VFX diagnostic before material authoring");
                    const std::string action = params.at("action").get<std::string>();
                    const std::set<std::string> expected = action == "set"
                        ? std::set<std::string>{"path","action","values"} : std::set<std::string>{"path","action"};
                    if (action != "open" && action != "state" && action != "set" && action != "save" &&
                        action != "revert" && action != "close") throw std::invalid_argument("Unknown material action");
                    if (params.size() != expected.size()) throw std::invalid_argument("Invalid material parameters");
                    for (const auto& [key, value] : params.items())
                        if (!expected.contains(key)) throw std::invalid_argument("Unknown material parameter");
                    const auto project = std::filesystem::weakly_canonical(ConfigManager::GetInstance().GetRootFolder());
                    const std::filesystem::path requested(params.at("path").get<std::string>());
                    const auto path = std::filesystem::weakly_canonical(project / requested);
                    if (requested.is_absolute() || requested.generic_string().find(':') != std::string::npos ||
                        !Under(path, mSceneRoot) || path.extension() != ".hmat" || !std::filesystem::is_regular_file(path))
                        throw std::invalid_argument("Material authoring requires an existing scratch-root .hmat");
                    for (auto part = project / requested; part != project && !part.empty(); part = part.parent_path())
                        if (std::filesystem::is_symlink(std::filesystem::symlink_status(part)))
                            throw std::invalid_argument("Material authoring does not follow symbolic links");
                    Json checked = params;
                    checked["path"] = path.lexically_relative(project).generic_string();
                    response["result"] = material(checked);
                }
                else if (method == "editor.pick")
                {
                    if (params.size() != 3 || !params.contains("origin") || !params.contains("direction") ||
                        !params.contains("maxDistance") || !params.at("maxDistance").is_number())
                        throw std::invalid_argument("editor.pick requires origin, direction and maxDistance");
                    if (!ModeManager::IsEditState() || mVfxActive || mVfxScenePending || !mVfxCurrentScene)
                        throw std::invalid_argument("Picking requires a settled editing scene");
                    const auto vector3 = [](const Json& value) {
                        if (!value.is_array() || value.size() != 3)
                            throw std::invalid_argument("Pick vector must have three coordinates");
                        glm::vec3 result;
                        for (size_t axis = 0; axis < 3; ++axis)
                        {
                            if (!value[axis].is_number()) throw std::invalid_argument("Pick coordinate must be numeric");
                            const double coordinate = value[axis].get<double>();
                            if (!std::isfinite(coordinate) || std::abs(coordinate) > 1'000'000)
                                throw std::invalid_argument("Pick coordinate must be finite and bounded");
                            result[axis] = static_cast<float>(coordinate);
                        }
                        return result;
                    };
                    const glm::vec3 origin = vector3(params.at("origin"));
                    const glm::vec3 direction = vector3(params.at("direction"));
                    const double maxDistance = params.at("maxDistance").get<double>();
                    if (!std::isfinite(maxDistance) || maxDistance <= 0 || maxDistance > 100'000 ||
                        glm::dot(direction, direction) < 1e-12f)
                        throw std::invalid_argument("Pick direction and maxDistance must be valid");
                    auto* level = const_cast<Level*>(static_cast<const Level*>(mVfxCurrentScene));
                    auto* embree = level->TryGetSystem<EmbreeSystem>();
                    if (!embree) throw std::runtime_error("Editor pick system is unavailable");
                    const auto hit = embree->Raycast(origin, glm::normalize(direction), 0.0f,
                        static_cast<float>(maxDistance));
                    response["result"] = hit
                        ? Json{{"hit",true},{"entity",hit->EntityID.str()},
                            {"subMeshIndex",hit->SubMeshIndex},{"materialIndex",hit->MaterialIndex}}
                        : Json{{"hit",false}};
                }
                else if (method == "editor.deformation")
                {
                    if (params.size() != 1 || !params.contains("uuid") || !params.at("uuid").is_string())
                        throw std::invalid_argument("editor.deformation requires uuid only");
                    if (!ModeManager::IsEditState() || mVfxActive || mVfxScenePending || !mVfxCurrentScene)
                        throw std::invalid_argument("Deformation inspection requires a settled editing scene");
                    const auto* level = static_cast<const Level*>(mVfxCurrentScene);
                    const auto view = level->mRegistry.view<const IDComponent, const AnimatorComponent, const MeshComponent>();
                    const auto uuid = params.at("uuid").get<std::string>();
                    bool found = false;
                    for (const auto handle : view)
                    {
                        if (view.get<const IDComponent>(handle).ID.str() != uuid) continue;
                        const auto& animator = view.get<const AnimatorComponent>(handle);
                        const auto& mesh = view.get<const MeshComponent>(handle).mMesh;
                        if (!mesh) throw std::invalid_argument("Inspected mesh is not loaded");
                        Json sections = Json::array();
                        size_t vertices = 0;
                        for (size_t index = 0; index < mesh->mSubMeshes.size(); ++index)
                        {
                            const auto& section = mesh->mSubMeshes[index];
                            if (!section) continue;
                            uint64_t positionHash = 14695981039346656037ull;
                            double maxHistoryDelta = 0;
                            const auto inspect = [&](const auto& stream) {
                                vertices += stream.size();
                                if (vertices > 1'000'000) throw std::invalid_argument("Deformation inspection vertex budget exceeded");
                                for (const auto& vertex : stream)
                                {
                                    for (int axis = 0; axis < 3; ++axis)
                                    {
                                        if (!std::isfinite(vertex.Pos[axis]) || !std::isfinite(vertex.PreviousPosition[axis]))
                                            throw std::runtime_error("Non-finite inspected deformation stream");
                                        positionHash ^= std::bit_cast<uint32_t>(vertex.Pos[axis]);
                                        positionHash *= 1099511628211ull;
                                    }
                                    maxHistoryDelta = std::max(maxHistoryDelta, static_cast<double>(glm::distance(vertex.Pos, vertex.PreviousPosition)));
                                }
                            };
                            if (section->IsSkinned()) inspect(section->mSkinnedVertices);
                            else inspect(section->mStaticVertices);
                            sections.push_back({{"index", index}, {"morphTargetCount", section->GetMorphTargets().size()},
                                {"vertices", section->IsSkinned() ? section->mSkinnedVertices.size() : section->mStaticVertices.size()},
                                {"positionHash", std::to_string(positionHash)}, {"revision", section->GetDeformationRevision()},
                                {"maxHistoryDelta", maxHistoryDelta}});
                        }
                        response["result"] = {{"uuid", uuid}, {"frame", Application::GetInstance().GetFrameNumber()},
                            {"playing", animator.bPlay}, {"loaded", animator.bLoaded}, {"clipCount", animator.mClips.size()},
                            {"facialTime", animator.mFacialPlayer.GetTime()}, {"clothInstances", animator.mClothSimulation.size()},
                            {"sections", std::move(sections)}};
                        found = true;
                        break;
                    }
                    if (!found) throw std::invalid_argument("Deformation entity was not found in the editing scene");
                }
                else if (method == "editor.preview")
                {
                    if (mVfxActive) throw std::invalid_argument("End VFX diagnostic before animation preview");
                    const std::string action = params.at("action").get<std::string>();
                    std::set<std::string> keys{"path","action"};
                    if (action == "play") { keys.insert("frames"); keys.insert("speed"); }
                    else if (action == "scrub") keys.insert("progress");
                    else if (action == "morph") { keys.insert("name"); keys.insert("weight"); }
                    else if (action == "focusMorph") keys.insert("name");
                    else if (action != "open" && action != "close" && action != "state" && action != "pause" && action != "focus" && action != "reset")
                        throw std::invalid_argument("Unknown editor preview action.");
                    if (params.size() != keys.size()) throw std::invalid_argument("Invalid preview parameters.");
                    for (const auto& [key, value] : params.items())
                        if (!keys.contains(key)) throw std::invalid_argument("Unknown preview parameter.");
                    if (action == "play" && (!params.at("frames").is_number_integer() || !params.at("speed").is_number()))
                        throw std::invalid_argument("Preview frames/speed must be numeric.");
                    if (action == "scrub" && !params.at("progress").is_number())
                        throw std::invalid_argument("Preview progress must be numeric.");
                    if (action == "morph" && (!params.at("name").is_string() || !params.at("weight").is_number()))
                        throw std::invalid_argument("Preview morph name/weight must be string/number.");
                    if (action == "focusMorph" && !params.at("name").is_string())
                        throw std::invalid_argument("Preview focus morph name must be a string.");
                    const auto project = std::filesystem::weakly_canonical(ConfigManager::GetInstance().GetRootFolder());
                    const std::filesystem::path requested(params.at("path").get<std::string>());
                    const auto path = std::filesystem::weakly_canonical(project / requested);
                    if (requested.is_absolute() || requested.generic_string().find(':') != std::string::npos ||
                        !Under(path, project / "Assets") || !std::filesystem::is_regular_file(path) ||
                        (path.extension() != ".hanim" && path.extension() != ".hanimctrl" && path.extension() != ".hmesh") ||
                        (path.extension() == ".hanimctrl" && action != "open" && action != "state" && action != "focus") ||
                        (path.extension() == ".hanim" && action != "open" && action != "close" && action != "state" && action != "play" && action != "pause" && action != "scrub") ||
                        (path.extension() == ".hmesh" && action != "open" && action != "close" && action != "state" && action != "morph" && action != "reset" && action != "focusMorph"))
                        throw std::invalid_argument("Preview requires a project Assets .hanim/.hmesh or .hanimctrl with a supported action.");
                    Json checked = params;
                    checked["path"] = path.lexically_relative(project).generic_string();
                    response["result"] = preview(checked);
                }
                else if (method == "editor.capture")
                {
                    if (params.size() != 2 || !params.contains("name") || !params.contains("settleFrames") ||
                        !params.at("settleFrames").is_number_integer())
                        throw std::invalid_argument("Capture requires name and integer settleFrames.");
                    const auto name = params.at("name").get<std::string>();
                    const auto settle = params.at("settleFrames").get<int64_t>();
                    if (name.empty() || name.size() > 48 || !std::all_of(name.begin(), name.end(), [](char c) {
                        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'; }) || settle < 2 || settle > 120)
                        throw std::invalid_argument("Capture name/settleFrames outside bounds.");
                    if (mCaptureDueFrame || (!mCaptureReceipt.is_null() && mCaptureReceipt.value("state", "") == "inFlight") || mCaptureTicket >= 32)
                        throw std::invalid_argument("Capture busy or session capture budget exhausted.");
                    const auto directory = mDirectory / "captures";
                    std::filesystem::create_directory(directory);
                    if (std::filesystem::is_symlink(std::filesystem::symlink_status(directory)) ||
                        std::filesystem::weakly_canonical(directory) != directory)
                        throw std::invalid_argument("Capture directory must remain in this owned mailbox.");
                    mCapturePath = directory / (name + ".png");
                    if (std::filesystem::exists(mCapturePath)) throw std::invalid_argument("Capture output already exists.");
                    mCaptureDueFrame = application.GetFrameNumber() + static_cast<uint64_t>(settle);
                    mCaptureReceipt = {{"ticket",++mCaptureTicket},{"state","queued"},{"path",mCapturePath.generic_string()},
                        {"target","presented-backbuffer"},{"requestedFrame",application.GetFrameNumber()},{"dueFrame",mCaptureDueFrame}};
                    response["result"] = mCaptureReceipt;
                }
                else if (method == "editor.capture.result")
                {
                    if (params.size() != 1 || !params.contains("ticket") || !params.at("ticket").is_number_unsigned() ||
                        params.at("ticket").get<uint64_t>() != mCaptureTicket || mCaptureReceipt.is_null())
                        throw std::invalid_argument("Unknown current capture ticket.");
                    response["result"] = mCaptureReceipt;
                }
                else
                {
                    if (mVfxActive) throw std::invalid_argument("End VFX diagnostic before scene transactions");
                    if (!allowed.contains(method)) throw std::invalid_argument("Method is outside the Editor scene validation allowlist.");
                    if (method == "scene.save" && params.contains("path")) (void)CheckedScenePath(params.at("path"));
                    const auto result = GMService::GetInstance().Execute(method,params,false,"editor-mailbox");
                    if (!result.bSuccess) throw std::runtime_error(result.Code + ": " + result.Message);
                    response["result"] = result.Data;
                }
                response["ok"] = true;
                if (response.dump().size() > 8u*1024u*1024u) throw std::runtime_error("Editor response exceeds 8 MiB.");
            }
            catch (const std::exception& error)
            {
                response.erase("result"); response["ok"] = false;
                response["error"] = {{"code","editor_mailbox_error"},{"message",std::string(error.what()).substr(0,2048)}};
            }
            Utils::SaveJsonToFile(response,mDirectory / "responses" / (id + ".json"));
            std::filesystem::remove(mClaimedRequest);
            mClaimedRequest.clear();
            if (bClose && response.at("ok") == true) Application::GetInstance().Close();
            return; // At most one request per frame; scene queue executes on this main thread.
        }
        catch (const std::exception& error)
        {
            (void)ShutdownVfx();
            HE_CORE_ERROR("Editor scene mailbox stopped: {0}", error.what());
            mDirectory.clear();
        }
#else
        (void)openScene;
        (void)preview;
        (void)material;
#endif
    }
}
