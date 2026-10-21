#pragma once

#include "Runtime/EcsFramework/System/System.h"

#include <memory>

namespace HEngine
{
    class SequenceSystem : public System
    {
    public:
        explicit SequenceSystem(Level* level);
        ~SequenceSystem() override;
        void OnRuntiemStart() override;
        void OnRuntimeStop() override;
        void OnUpdateRuntime(Timestep ts) override;
        bool ShouldUpdateWhenRuntimePaused() const override { return true; }
    private:
        struct State;
        std::unique_ptr<State> mState;
    };
}
