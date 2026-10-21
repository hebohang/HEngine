#include "Hepch.h"

#include "Runtime/Animation/FacialPerformance.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "_generated/serializer/FacialPerformance.gen.h"

namespace HEngine
{
    void FacialPlayer::Reset()
    {
        mTime = 0.0f;
        mLoadedPath.clear();
        mError.clear();
        mLoadedData = {};
        mWeights.clear();
    }

    void FacialPlayer::Seek(float seconds)
    {
        mTime = std::isfinite(seconds) ? std::max(seconds, 0.0f) : 0.0f;
    }

    const std::map<std::string, float>& FacialPlayer::Update(float deltaSeconds,
        const std::string& resourcePath, const FacialPerformanceRes& inlineData,
        const std::map<std::string, float>& weights,
        const std::string& blinkLeft, const std::string& blinkRight, float blinkInterval)
    {
        if (std::isfinite(deltaSeconds) && deltaSeconds > 0.0f) mTime += deltaSeconds;
        const bool external = !resourcePath.empty() && resourcePath != "None";
        if (resourcePath != mLoadedPath)
        {
            mLoadedPath = resourcePath;
            mLoadedData = {};
            mError.clear();
            if (external)
            {
                try
                {
                    Serializer::Deserialize(Utils::LoadJsonFile(ConfigManager::GetFullPath(resourcePath)), mLoadedData);
                    FacialPerformance::Validate(mLoadedData);
                }
                catch (const std::exception& error)
                {
                    mError = error.what();
                    HE_CORE_ERROR("Facial performance '{0}' failed: {1}", resourcePath, mError);
                }
            }
        }
        mWeights.clear();
        const std::string previousError = mError;
        if (!external) mError.clear(); // Edited inline curves can recover without restarting the component.
        if (mError.empty())
        {
            try
            {
                if (!external) FacialPerformance::Validate(inlineData);
                FacialPerformance::Evaluate(external ? mLoadedData : inlineData, mTime, mWeights);
            }
            catch (const std::exception& error)
            {
                mError = error.what();
                if (mError != previousError) HE_CORE_ERROR("Facial performance failed: {0}", mError);
            }
        }
        for (const auto& [name, value] : weights)
            if (std::isfinite(value)) mWeights[name] = std::clamp(value, -2.0f, 2.0f);
        const float blink = FacialPerformance::Blink(mTime, blinkInterval);
        if (!blinkLeft.empty()) mWeights[blinkLeft] = std::max(mWeights[blinkLeft], blink);
        if (!blinkRight.empty()) mWeights[blinkRight] = std::max(mWeights[blinkRight], blink);
        return mWeights;
    }
}
