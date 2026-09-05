#include "Hepch.h"

#include "Runtime/Resource/ResType/BehaviorTreeRes.h"
#include "_generated/serializer/BehaviorTreeRes.gen.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"

namespace HEngine
{
    bool LoadBehaviorTreeRes(const std::filesystem::path& fullPath, BehaviorTreeRes& outRes, std::string* outError)
    {
        try
        {
            const Json json = Utils::LoadJsonFile(fullPath);
            outRes = json.get<BehaviorTreeRes>();

            if (outRes.name.empty())
                outRes.name = fullPath.stem().string();
        }
        catch (const std::exception& ex)
        {
            if (outError)
                *outError = ex.what();
            return false;
        }

        return true;
    }

    bool SaveBehaviorTreeRes(const BehaviorTreeRes& res, const std::filesystem::path& fullPath, std::string* outError)
    {
        try
        {
            Json json;
            json = res;
            Utils::SaveJsonToFile(json, fullPath);
        }
        catch (const std::exception& ex)
        {
            if (outError)
                *outError = ex.what();
            return false;
        }

        return true;
    }
}
