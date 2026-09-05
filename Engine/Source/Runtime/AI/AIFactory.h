#pragma once

#include "Runtime/AI/BehaviorTree/BTNode.h"
#include "Runtime/Core/Base/Base.h"

namespace HEngine
{
    Scope<BTNode> CreateSimpleChaseMonsterAI();
    Scope<BTNode> CreateNavMeshChaseMonsterAI();
}
