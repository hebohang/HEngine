#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace HEngine
{
    enum class BuildFeature { None, Physics2D, Physics3D, Navigation, Svg, Lottie };

    constexpr bool IsBuildFeatureEnabled(BuildFeature feature)
    {
        switch (feature)
        {
        case BuildFeature::None: return true;
        case BuildFeature::Physics2D: return HE_ENABLE_PHYSICS_2D;
        case BuildFeature::Physics3D: return HE_ENABLE_PHYSICS_3D;
        case BuildFeature::Navigation: return HE_ENABLE_NAVIGATION;
        case BuildFeature::Svg: return HE_ENABLE_UI_SVG;
        case BuildFeature::Lottie: return HE_ENABLE_UI_LOTTIE;
        }
        return false;
    }

    constexpr const char* BuildFeatureOption(BuildFeature feature)
    {
        switch (feature)
        {
        case BuildFeature::Physics2D: return "HE_ENABLE_PHYSICS_2D";
        case BuildFeature::Physics3D: return "HE_ENABLE_PHYSICS_3D";
        case BuildFeature::Navigation: return "HE_ENABLE_NAVIGATION";
        case BuildFeature::Svg: return "HE_ENABLE_UI_SVG";
        case BuildFeature::Lottie: return "HE_ENABLE_UI_LOTTIE";
        default: return "None";
        }
    }

    inline void RequireBuildFeature(BuildFeature feature, std::string_view context)
    {
        if (!IsBuildFeatureEnabled(feature))
            throw std::runtime_error(std::string(context) + ": unsupported by this build; enable " + BuildFeatureOption(feature));
    }
}
