#include "Hepch.h"

#include "Runtime/Resource/ResType/Skeleton.h"
#include "_generated/serializer/Skeleton.gen.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"

#include <fstream>
#include <iomanip>

namespace HEngine
{
    bool LoadSkeletonRes(const std::filesystem::path& fullPath, SkeletonRes& outRes, std::string* outError)
    {
        try
        {
            const Json json = Utils::LoadJsonFile(fullPath);
            outRes = json.get<SkeletonRes>();

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

    bool SaveSkeletonRes(const SkeletonRes& res, const std::filesystem::path& fullPath, std::string* outError)
    {
        try
        {
            Json json;
            json = res;
            std::ofstream fout(fullPath);
            if (!fout)
            {
                if (outError)
                    *outError = "Failed to open output file";
                return false;
            }

            fout << std::setw(4) << json;
            fout.flush();
            if (!fout)
            {
                if (outError)
                    *outError = "Failed to write output file";
                return false;
            }
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
