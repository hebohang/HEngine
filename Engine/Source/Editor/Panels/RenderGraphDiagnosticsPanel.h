#pragma once

#include "BasePanel.h"

#include "Runtime/Graphics/RenderGraph/RenderGraph.h"

#include <array>
#include <cstdint>
#include <string>

namespace HEngine
{
    class RenderGraphDiagnosticsPanel final : public BasePanel
    {
        REGISTER_PANEL(RenderGraphDiagnosticsPanel)

    public:
        RenderGraphDiagnosticsPanel(
            EditorLayer* parent,
            std::string panelName,
            bool* pOpen);

    protected:
        void OnImGuiRenderImpl() override;

    private:
        void Refresh();
        void DrawPassList();
        void DrawPassDetails();
        void DrawResources();
        void DrawDependencies();

        RenderGraphDiagnosticSnapshot mSnapshot;
        std::array<char, 128> mSearch{};
        std::uint64_t mCapturedFrame = 0;
        std::uint32_t mSelectedPass = InvalidRenderGraphIndex;
        bool bAutoRefresh = true;
        bool bShowCulled = true;
    };
}
