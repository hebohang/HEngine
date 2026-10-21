#include "Game/Runtime/RuntimeGameLayer.h"
#include "Game/Runtime/GameBootstrap.h"

namespace HEngine
{
    Layer* CreateProjectGameLayer()
    {
        return new RuntimeGameLayer();
    }
}
