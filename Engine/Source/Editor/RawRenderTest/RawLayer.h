#pragma once

#include "HEngine.h"

namespace HEngine
{
    class RawLayer : public Layer
    {
    public:
        void OnUpdate(Timestep ts) override;
    };
}