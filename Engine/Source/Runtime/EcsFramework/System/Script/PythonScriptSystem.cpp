#include "Hepch.h"

#include "Runtime/EcsFramework/System/Script/PythonScriptSystem.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Component/Script/PythonScriptComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/Core/Log/Log.h"
#include "Runtime/Scripting/ScriptEngine.h"
#include "Runtime/UI/Rml/RmlUiManager.h"


#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>


namespace HEngine
{
    Level* PythonScriptSystem::sActiveLevel = nullptr;
    PythonScriptSystem* PythonScriptSystem::sActiveSystem = nullptr;
    std::optional<UUID> PythonScriptSystem::sCurrentScriptOwner = std::nullopt;

    namespace
    {
        class ScopedCurrentScriptOwner
        {
        public:
            explicit ScopedCurrentScriptOwner(const UUID& owner)
            {
                if (const UUID* currentOwner = PythonScriptSystem::GetCurrentScriptOwner())
                    mPreviousOwner = *currentOwner;
                PythonScriptSystem::SetCurrentScriptOwner(owner);
            }

            ~ScopedCurrentScriptOwner()
            {
                if (mPreviousOwner)
                    PythonScriptSystem::SetCurrentScriptOwner(*mPreviousOwner);
                else
                    PythonScriptSystem::ClearCurrentScriptOwner();
            }

        private:
            std::optional<UUID> mPreviousOwner;
        };
    }

    PythonScriptSystem::PythonScriptSystem(Level* level)
        : System(level)
    {
    }

    PythonScriptSystem::~PythonScriptSystem() noexcept
    {
        try
        {
            OnRuntimeStop();
        }
        catch (const std::exception& exception)
        {
            Log::WriteFallbackError("Python runtime destructor cleanup failed.", exception.what());
        }
        catch (...)
        {
            Log::WriteFallbackError("Python runtime destructor cleanup failed.", "unknown exception");
        }
    }

    const UUID* PythonScriptSystem::GetCurrentScriptOwner()
    {
        return sCurrentScriptOwner ? &(*sCurrentScriptOwner) : nullptr;
    }

    PythonScriptSystem* PythonScriptSystem::GetActiveSystem(Level* level) noexcept
    {
        return sActiveLevel == level ? sActiveSystem : nullptr;
    }

    void PythonScriptSystem::SetCurrentScriptOwner(const UUID& owner)
    {
        sCurrentScriptOwner = owner;
    }

    void PythonScriptSystem::ClearCurrentScriptOwner()
    {
        sCurrentScriptOwner.reset();
    }

    void PythonScriptSystem::OnScriptComponentAdded(
        Entity entity,
        PythonScriptComponent& component)
    {
        (void)entity;
        (void)component;

        // Component construction and script activation are separate transactions.
        // Existing scene owners are activated by OnRuntiemStart, while runtime
        // prefab owners are activated only after every serialized component exists.
        // Registering here would create an unstarted, untracked module generation.
    }

    void PythonScriptSystem::OnScriptComponentRemoving(
        Level* level,
        const UUID& owner) noexcept
    {
        if (sActiveSystem != nullptr && sActiveLevel == level)
            sActiveSystem->ReleaseRuntimeOwner(owner);
    }

    bool PythonScriptSystem::StartRuntimeOwner(Entity entity, bool bRefreshModule)
    {
        if (!mRuntimeActive || sActiveSystem != this || sActiveLevel != mLevel)
            throw std::logic_error("Python runtime owner cannot start outside its active system.");
        if (!entity || entity.GetLevel() != mLevel ||
            !entity.HasComponent<PythonScriptComponent>())
        {
            throw std::invalid_argument("Python runtime owner does not belong to the active level.");
        }

        const std::string scriptPath =
            entity.GetComponent<PythonScriptComponent>().Path;
        if (scriptPath.empty() || scriptPath == "None")
            return false;

        const UUID owner = entity.UUID();
        if (std::find(mStartedScriptOwners.begin(), mStartedScriptOwners.end(), owner) !=
            mStartedScriptOwners.end())
        {
            throw std::logic_error("Python runtime owner is already started.");
        }

        ScopedCurrentScriptOwner currentOwner(owner);
        bool bOwnerTracked = false;
        try
        {
            RmlUiManager::GetInstance().ClearOwnerDocuments(owner);

            if (bRefreshModule || !ScriptEngine::IsClassRegistered(owner))
            {
                HE_PROFILE_SECTION("ScriptEngine::RegisterModule");
                if (!ScriptEngine::RegisterModule(owner, scriptPath))
                {
                    std::string registrationError =
                        ScriptEngine::GetLastRegistrationError(owner);
                    const std::size_t firstLineEnd =
                        registrationError.find_first_of("\r\n");
                    if (firstLineEnd != std::string::npos)
                        registrationError.resize(firstLineEnd);
                    throw std::runtime_error(
                        "Failed to register Python runtime owner " +
                        owner.str() +
                        (registrationError.empty()
                            ? "."
                            : ": " + registrationError));
                }
            }

            // Import and class construction execute Python. They may synchronously
            // destroy the source entity, so validate again before publishing the
            // owner into the active runtime snapshot.
            if (!entity || entity.GetLevel() != mLevel ||
                !entity.HasComponent<PythonScriptComponent>())
            {
                throw std::runtime_error(
                    "Python runtime owner was destroyed during module registration: " +
                    owner.str() + ".");
            }

            mStartedScriptOwners.push_back(owner);
            bOwnerTracked = true;

#ifdef WITH_EDITOR
            ScriptEngine::ReloadModuleAndClass(owner);
#endif

            HE_PROFILE_SECTION("PythonScriptSystem::StartCallback");
            ScriptEngine::Invoke(owner, LegacyScriptCallback::Start, &entity);
            return true;
        }
        catch (...)
        {
            const std::exception_ptr startFailure = std::current_exception();
            const std::exception_ptr cleanupFailure = bOwnerTracked
                ? StopRuntimeOwner(owner, true)
                : CleanupRuntimeOwnerState(owner);
            if (cleanupFailure)
            {
                try
                {
                    std::rethrow_exception(cleanupFailure);
                }
                catch (const std::exception& exception)
                {
                    Log::WriteFallbackError(
                        "Python owner startup rollback failed.",
                        exception.what());
                }
                catch (...)
                {
                    Log::WriteFallbackError(
                        "Python owner startup rollback failed.",
                        "unknown exception");
                }
            }
            std::rethrow_exception(startFailure);
        }
    }

    bool PythonScriptSystem::StartRuntimeOwner(Entity entity)
    {
        return StartRuntimeOwner(entity, false);
    }

    std::exception_ptr PythonScriptSystem::CleanupRuntimeOwnerState(
        const UUID& owner) noexcept
    {
        std::exception_ptr firstFailure;
        const auto recordFailure = [&firstFailure](std::string_view context) noexcept
        {
            const std::exception_ptr failure = std::current_exception();
            if (!firstFailure)
            {
                firstFailure = failure;
                return;
            }

            try
            {
                std::rethrow_exception(failure);
            }
            catch (const std::exception& exception)
            {
                Log::WriteFallbackError(context, exception.what());
            }
            catch (...)
            {
                Log::WriteFallbackError(context, "unknown exception");
            }
        };

        try
        {
            RmlUiManager::GetInstance().ClearOwnerDocuments(owner);
        }
        catch (...)
        {
            recordFailure("Python owner document cleanup failed.");
        }

        try
        {
            if (ScriptEngine::IsInitialized())
            {
                ScriptEngine::UnregisterModule(owner);
            }
        }
        catch (...)
        {
            recordFailure("Python module unregister cleanup failed.");
        }

        return firstFailure;
    }

    std::exception_ptr PythonScriptSystem::StopRuntimeOwner(
        const UUID& owner,
        bool bInvokeStopCallback) noexcept
    {
        const auto ownerIt = std::find(
            mStartedScriptOwners.begin(),
            mStartedScriptOwners.end(),
            owner);
        if (ownerIt == mStartedScriptOwners.end())
            return {};

        // Remove first so Stop()->HEngine.Destroy(self) cannot recursively invoke
        // the same callback. The outer transaction retains cleanup ownership.
        mStartedScriptOwners.erase(ownerIt);
        mFaultedOwners.erase(owner);
        std::exception_ptr firstFailure;
        const auto recordFailure = [&firstFailure](std::string_view context) noexcept
        {
            const std::exception_ptr failure = std::current_exception();
            if (!firstFailure)
            {
                firstFailure = failure;
                return;
            }

            try
            {
                std::rethrow_exception(failure);
            }
            catch (const std::exception& exception)
            {
                Log::WriteFallbackError(context, exception.what());
            }
            catch (...)
            {
                Log::WriteFallbackError(context, "unknown exception");
            }
        };

        if (bInvokeStopCallback && ScriptEngine::IsInitialized())
        {
            try
            {
                ScopedCurrentScriptOwner currentOwner(owner);
                if (ScriptEngine::IsClassRegistered(owner))
                {
                    ScriptEngine::Invoke(owner, LegacyScriptCallback::Stop);
                }
            }
            catch (...)
            {
                recordFailure("Python Stop callback cleanup failed.");
            }
        }

        if (const std::exception_ptr cleanupFailure = CleanupRuntimeOwnerState(owner))
        {
            if (!firstFailure)
            {
                firstFailure = cleanupFailure;
            }
            else
            {
                try
                {
                    std::rethrow_exception(cleanupFailure);
                }
                catch (const std::exception& exception)
                {
                    Log::WriteFallbackError(
                        "Python runtime owner post-callback cleanup failed.",
                        exception.what());
                }
                catch (...)
                {
                    Log::WriteFallbackError(
                        "Python runtime owner post-callback cleanup failed.",
                        "unknown exception");
                }
            }
        }

        return firstFailure;
    }

    void PythonScriptSystem::ReleaseRuntimeOwner(const UUID& owner) noexcept
    {
        if (const std::exception_ptr failure = StopRuntimeOwner(owner, true))
        {
            try
            {
                std::rethrow_exception(failure);
            }
            catch (const std::exception& exception)
            {
                Log::WriteFallbackError("Python runtime owner cleanup failed.", exception.what());
            }
            catch (...)
            {
                Log::WriteFallbackError("Python runtime owner cleanup failed.", "unknown exception");
            }
        }
    }

    void PythonScriptSystem::OnRuntiemStart()
    {
        HE_PROFILE_SECTION("PythonScriptSystem::RuntimeStart");
        if (mRuntimeActive)
            throw std::logic_error("Python runtime scripts are already active.");
        if (sActiveSystem != nullptr)
            throw std::logic_error("Another Python runtime script system is already active.");

        auto view = mLevel->mRegistry.view<TransformComponent, PythonScriptComponent>();
        std::vector<EntityHandle> startupOwners;
        startupOwners.reserve(view.size_hint());
        for (const EntityHandle handle : view)
            startupOwners.push_back(handle);

        mStartedScriptOwners.reserve(startupOwners.size());
        mRuntimeActive = true;
        sActiveSystem = this;
        sActiveLevel = mLevel;
        for (const EntityHandle handle : startupOwners)
        {
            if (!mLevel->mRegistry.valid(handle) ||
                !mLevel->mRegistry.all_of<TransformComponent, PythonScriptComponent>(handle))
            {
                continue;
            }

            Entity entity = { handle, mLevel };
            const std::string ownerId = entity.UUID().str();
            const std::string scriptPath = entity.GetComponent<PythonScriptComponent>().Path;
            try { (void)StartRuntimeOwner(entity, true); }
            catch (const std::exception& error)
            {
                HE_CORE_ERROR("[Python] owner={0}, script={1}: {2}", ownerId, scriptPath, error.what());
            }
        }
    }

    void PythonScriptSystem::OnUpdateRuntime(Timestep ts)
    {
		HE_PROFILE;

        CallAll(mLevel->IsRuntimePaused() ? LegacyScriptCallback::UpdatePaused : LegacyScriptCallback::Update, ts.GetSeconds());
    }

	void PythonScriptSystem::OnFixedUpdate(Timestep ts)
	{
		HE_PROFILE_SECTION("PythonScriptSystem::FixedUpdate");
		CallAll(LegacyScriptCallback::FixedUpdate, ts.GetSeconds());
	}

    void PythonScriptSystem::OnRuntimeStop()
    {
        const bool bWasRuntimeActive = std::exchange(mRuntimeActive, false);
        std::exception_ptr firstFailure;
        const auto recordCleanupFailure = [&firstFailure](
            const std::exception_ptr& failure,
            std::string_view context) noexcept
        {
            if (!failure)
                return;
            if (!firstFailure)
            {
                firstFailure = failure;
                return;
            }

            try
            {
                std::rethrow_exception(failure);
            }
            catch (const std::exception& exception)
            {
                Log::WriteFallbackError(context, exception.what());
            }
            catch (...)
            {
                Log::WriteFallbackError(context, "unknown exception");
            }
        };

        while (!mStartedScriptOwners.empty())
        {
            const UUID owner = mStartedScriptOwners.front();
            recordCleanupFailure(
                StopRuntimeOwner(owner, bWasRuntimeActive),
                bWasRuntimeActive
                    ? "Python runtime owner cleanup failed."
                    : "Python partial owner cleanup failed.");
        }

        if (sActiveSystem == this)
        {
            ClearCurrentScriptOwner();
            sActiveSystem = nullptr;
            sActiveLevel = nullptr;
        }

        if (firstFailure)
            std::rethrow_exception(firstFailure);
    }

    void PythonScriptSystem::OnImGuiRender()
    {
        // Legacy runtime ImGui UI path. New runtime UI should use HEngine.RmlUI
        // from Start/Update/OnUIEvent and render through RmlUiManager instead.
        CallAll(LegacyScriptCallback::UI);
    }

#if WITH_EDITOR
    void PythonScriptSystem::OnUpdateEditor(Timestep ts, EditorCamera& camera)
    {
        (void)ts;
        (void)camera;

        // Runtime Python behavior is only executed in PIE/runtime.
        // The previous editor path imported, reloaded and logged every script each frame,
        // which kept the editor busy even when nothing was happening in the scene.
    }
#endif

    void PythonScriptSystem::CallAll(LegacyScriptCallback callback, float seconds)
    {
        const auto owners = mStartedScriptOwners;
        for (const UUID& owner : owners)
        {
            if (mFaultedOwners.contains(owner) || !ScriptEngine::IsClassRegistered(owner))
                continue;
            if (std::find(mStartedScriptOwners.begin(), mStartedScriptOwners.end(), owner) == mStartedScriptOwners.end())
                continue;
            try
            {
                ScopedCurrentScriptOwner currentOwner(owner);
                ScriptEngine::Invoke(owner, callback, nullptr, seconds);
            }
            catch (const std::exception& error)
            {
                mFaultedOwners.insert(owner);
                HE_CORE_ERROR("[Python] owner={0}: {1}. Updates disabled until scene restart.", owner.str(), error.what());
            }
        }
    }
}
