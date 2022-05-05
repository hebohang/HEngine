#pragma once 

#include "Runtime/Renderer/Texture.h"

namespace HEngine
{
    class SceneSettingsPanel
    {
    public:
        SceneSettingsPanel();

        void OnImGuiRender(bool* pOpen);

    private:
        std::vector<std::string> mPaths;

        //Visualization
        Ref<Texture> mRight;
        Ref<Texture> mLeft;
        Ref<Texture> mTop;
        Ref<Texture> mBottom;
        Ref<Texture> mFront;
        Ref<Texture> mBack;
    };
}