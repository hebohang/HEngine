#include "Hepch.h"

#include "Runtime/Resource/ResType/AnimController.h"
#include "_generated/serializer/AnimController.gen.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"

namespace HEngine
{
    bool LoadAnimControllerRes(const std::filesystem::path& fullPath, AnimControllerRes& outRes, std::string* outError)
    {
        try
        {
            const Json json = Utils::LoadJsonFile(fullPath);
            outRes = json.get<AnimControllerRes>();

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

    bool SaveAnimControllerRes(const AnimControllerRes& res, const std::filesystem::path& fullPath, std::string* outError)
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
