#pragma once
#include "Runtime/Core/BuildFeatures.h"
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/Factory.h>

namespace HEngine
{
    class UnsupportedUiElementInstancer final : public Rml::ElementInstancer
    {
    public:
        explicit UnsupportedUiElementInstancer(BuildFeature feature) : mFeature(feature) {}
        Rml::ElementPtr InstanceElement(Rml::Element*, const Rml::String& tag, const Rml::XMLAttributes&) override
        {
            RequireBuildFeature(mFeature, "RmlUi <" + tag + ">");
            return nullptr;
        }
        void ReleaseElement(Rml::Element* element) override { delete element; }
    private:
        BuildFeature mFeature;
    };

    inline void RegisterUnsupportedUiElements()
    {
#if !HE_ENABLE_UI_SVG
        static UnsupportedUiElementInstancer svg(BuildFeature::Svg);
        Rml::Factory::RegisterElementInstancer("svg", &svg);
#endif
#if !HE_ENABLE_UI_LOTTIE
        static UnsupportedUiElementInstancer lottie(BuildFeature::Lottie);
        Rml::Factory::RegisterElementInstancer("lottie", &lottie);
#endif
    }
}
