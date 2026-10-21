#pragma once

#include <stdexcept>

namespace HEngine
{
    enum class ScriptPlayState { Edit, Starting, Playing, Paused, Stepping, Stopping };

    // Host-independent transition contract. Compilation is an independent task;
    // replacing a generation is only permitted at the Edit boundary.
    class ScriptPlaySession final
    {
    public:
        ScriptPlayState State() const noexcept { return mState; }
        bool IsPaused() const noexcept { return mState == ScriptPlayState::Paused || mState == ScriptPlayState::Stepping; }
        bool NeedsStep() const noexcept { return mState == ScriptPlayState::Stepping; }
        bool IsTransitioning() const noexcept { return mState == ScriptPlayState::Starting || mState == ScriptPlayState::Stopping; }
        void BeginPlay() { Move(ScriptPlayState::Edit, ScriptPlayState::Starting); }
        void CommitPlay() { Move(ScriptPlayState::Starting, ScriptPlayState::Playing); }
        void AbortPlay() { Move(ScriptPlayState::Starting, ScriptPlayState::Edit); }
        void Pause() { Move(ScriptPlayState::Playing, ScriptPlayState::Paused); }
        void Resume() { Move(ScriptPlayState::Paused, ScriptPlayState::Playing); }
        void Step() { Move(ScriptPlayState::Paused, ScriptPlayState::Stepping); }
        void CompleteStep() { Move(ScriptPlayState::Stepping, ScriptPlayState::Paused); }
        void BeginStop()
        {
            if (mState != ScriptPlayState::Playing && mState != ScriptPlayState::Paused && mState != ScriptPlayState::Stepping)
                throw std::logic_error("Stop requires an active Play session.");
            mState = ScriptPlayState::Stopping;
        }
        void CommitStop() { Move(ScriptPlayState::Stopping, ScriptPlayState::Edit); }
    private:
        void Move(ScriptPlayState expected, ScriptPlayState next)
        {
            if (mState != expected) throw std::logic_error("Illegal or reentrant Play state transition.");
            mState = next;
        }
        ScriptPlayState mState = ScriptPlayState::Edit;
    };
}
