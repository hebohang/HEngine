#include "Hepch.h"

#include "Runtime/EcsFramework/System/AI/BehaviorTreeSystem.h"

#include "Runtime/AI/BehaviorTree/BTBuilder.h"
#include "Runtime/AI/BehaviorTree/BTBuiltinNodes.h"
#include "Runtime/AI/Navigation/NavigationService.h"
#include "Runtime/EcsFramework/Component/AI/BehaviorTreeComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/ResType/BehaviorTreeRes.h"

#include <algorithm>
#include <cmath>

namespace HEngine
{
    float AdvanceBehaviorTreeTickCadence(
        float& accumulator, float fixedDt, float tickInterval) noexcept
    {
        if (!std::isfinite(fixedDt) || fixedDt <= 0.0f) return 0.0f;
        if (!std::isfinite(tickInterval) || tickInterval <= fixedDt)
        {
            accumulator = 0.0f;
            return fixedDt;
        }
        if (!std::isfinite(accumulator) || accumulator < 0.0f)
            accumulator = 0.0f;
        const double elapsed = static_cast<double>(accumulator) + fixedDt;
        if (elapsed + 1e-7 * tickInterval < tickInterval)
        {
            accumulator = static_cast<float>(elapsed);
            return 0.0f;
        }
        accumulator = static_cast<float>(std::max(0.0, elapsed - tickInterval));
        return tickInterval;
    }

    void BehaviorTreeSystem::OnRuntiemStart()
    {
        RegisterBuiltinBTNodes();
#if HE_ENABLE_NAVIGATION
        NavigationService::Get().BuildNavMesh(mLevel);
#endif

        auto view = mLevel->mRegistry.view<BehaviorTreeComponent>();
        for (auto e : view)
        {
            auto& btComp = view.get<BehaviorTreeComponent>(e);
            btComp.mTickTimer = 0.0f;

            if (btComp.btResPath == "None" || btComp.btResPath.empty() || btComp.bLoaded)
                continue;

            const std::filesystem::path fullPath = ConfigManager::GetFullPath(btComp.btResPath);
            BehaviorTreeRes res;
            std::string error;
            if (!LoadBehaviorTreeRes(fullPath, res, &error))
            {
                HE_CORE_WARN("BehaviorTreeSystem: failed load '{0}', error='{1}'", btComp.btResPath, error);
                continue;
            }

            BTBuilder::Get().InitBlackboard(res, btComp.mBlackboard);
            btComp.mRootNode = BTBuilder::Get().Build(res);
            btComp.bLoaded = btComp.mRootNode != nullptr;
            if (btComp.bLoaded)
                HE_CORE_INFO("BehaviorTreeSystem: loaded bt '{0}'", btComp.btResPath);
        }
    }

    void BehaviorTreeSystem::OnUpdateRuntime(Timestep ts)
    {
		HE_PROFILE;

        const float dt = ts.GetSeconds();
        auto view = mLevel->mRegistry.view<BehaviorTreeComponent>();
        for (auto e : view)
        {
            auto& btComp = view.get<BehaviorTreeComponent>(e);
            if (!btComp.bEnabled || !btComp.mRootNode)
                continue;

            const float actualDt = AdvanceBehaviorTreeTickCadence(
                btComp.mTickTimer, dt, btComp.tickInterval);
            if (actualDt <= 0.0f)
                continue;

            Entity entity = { e, mLevel };
            btComp.mRootNode->Tick(actualDt, btComp.mBlackboard, entity);
        }
    }

    void BehaviorTreeSystem::OnRuntimeStop()
    {
        NavigationService::Get().Release();

        auto view = mLevel->mRegistry.view<BehaviorTreeComponent>();
        for (auto e : view)
        {
            auto& btComp = view.get<BehaviorTreeComponent>(e);
            if (btComp.mRootNode)
                btComp.mRootNode->Reset();

            btComp.mBlackboard.Clear();
            btComp.mRootNode.reset();
            btComp.bLoaded = false;
            btComp.mTickTimer = 0.0f;
        }
    }
}
