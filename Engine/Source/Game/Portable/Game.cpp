#include "Game/Runtime/GameBootstrap.h"
#include "Game/Runtime/RuntimeGameLayer.h"

namespace HEngine
{
    Layer* CreateProjectGameLayer()
    {
        return new RuntimeGameLayer();
    }
}
