#include "Hepch.h"

#include "Runtime/EcsFramework/System/AI/BehaviorTreeSystem.h"

#include "Runtime/AI/BehaviorTree/BTBuilder.h"
#include "Runtime/AI/BehaviorTree/BTBuiltinNodes.h"
#include "Runtime/AI/Navigation/NavigationService.h"
#include "Runtime/EcsFramework/Component/AI/BehaviorTreeComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/ResType/BehaviorTreeRes.h"

namespace HEngine
{
    void BehaviorTreeSystem::OnRuntiemStart()
    {
        RegisterBuiltinBTNodes();
        NavigationService::Get().BuildNavMesh(mLevel);

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

            btComp.mTickTimer += dt;
            if (btComp.mTickTimer < btComp.tickInterval)
                continue;

            const float actualDt = btComp.mTickTimer;
            btComp.mTickTimer = 0.0f;

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
