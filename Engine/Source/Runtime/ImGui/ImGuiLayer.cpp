#include "Hepch.h"

#include "Runtime/ImGui/ImGuiLayer.h"
#include "Runtime/ImGui/NvrhiImGuiLayer.h"

#include "Runtime/Core/Log/Log.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <imgui.h>
#ifdef WITH_EDITOR
#include <imgui-notify-v2/imgui_notify.h>
#endif
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>

namespace HEngine
{
    namespace
    {
        ImFont* AddFontFromPath(
            ImFontAtlas& atlas,
            const std::filesystem::path& path,
            float fontSize,
            const ImWchar* glyphRanges)
        {
            std::ifstream stream(path, std::ios::binary | std::ios::ate);
            std::streamoff fileSize = -1;
            if (stream)
                fileSize = static_cast<std::streamoff>(stream.tellg());
            if (fileSize <= 100 || fileSize > std::numeric_limits<int>::max())
            {
                HE_CORE_ERROR("Could not load ImGui font: {0}", Utils::PathToUtf8(path));
                return nullptr;
            }

            void* fontData = ImGui::MemAlloc(static_cast<std::size_t>(fileSize));
            stream.seekg(0, std::ios::beg);
            if (!stream.read(
                    static_cast<char*>(fontData),
                    static_cast<std::streamsize>(fileSize)))
            {
                ImGui::MemFree(fontData);
                HE_CORE_ERROR("Could not read ImGui font: {0}", Utils::PathToUtf8(path));
                return nullptr;
            }

            ImFontConfig fontConfig;
            fontConfig.FontDataOwnedByAtlas = true;
            const std::string fontName = Utils::PathToUtf8(path.filename());
            std::snprintf(fontConfig.Name, sizeof(fontConfig.Name), "%s", fontName.c_str());
            return atlas.AddFontFromMemoryTTF(
                fontData,
                static_cast<int>(fileSize),
                fontSize,
                &fontConfig,
                glyphRanges);
        }
    }

    //bool ImGuiLayer::bReloadFont = false;

    ImGuiLayer::ImGuiLayer()
        : Layer("ImGuiLayer")
    {
    }

    ImGuiLayer::ImGuiLayer(const std::string& name)
        : Layer(name)
    {
    }

    ImGuiLayer::~ImGuiLayer()
    {
    }

    void ImGuiLayer::OnEvent(Event& e)
    {
        if (mBlockEvents && ImGui::GetCurrentContext() != nullptr)
        {
            ImGuiIO& io = ImGui::GetIO();
            e.Handled |= e.IsInCategory(EventCategoryMouse) & io.WantCaptureMouse;
            e.Handled |= e.IsInCategory(EventCategoryKeyboard) & io.WantCaptureKeyboard;
        }
    }

    ImGuiLayer* ImGuiLayer::Create()
    {
        HE_CORE_ERROR(
            "ImGui requires explicit NVRHI initialization. "
            "Call ImGuiLayer::CreateNvrhi after RenderDevice initialization.");
        return nullptr;
    }

    ImGuiLayer* ImGuiLayer::CreateNvrhi(const NvrhiImGuiLayerDesc& desc)
    {
        auto layer = std::make_unique<NvrhiImGuiLayer>();
        layer->Initialize(desc);
        return layer.release();
    }

    void ImGuiLayer::SetDarkThemeColors()
    {
        ImGuiStyle& style = ImGui::GetStyle();
        auto& colors = style.Colors;

		const ImVec4 accentBlue = ImVec4(0.26f, 0.52f, 0.86f, 1.00f);
		const ImVec4 accentBlueSoft = ImVec4(0.31f, 0.58f, 0.92f, 0.78f);

		style.WindowRounding = 5.0f;
		style.ChildRounding = 5.0f;
		style.FrameRounding = 4.0f;
		style.GrabRounding = 4.0f;
		style.PopupRounding = 5.0f;
		style.ScrollbarRounding = 6.0f;
		style.TabRounding = 4.0f;
		style.WindowBorderSize = 1.0f;
		style.ChildBorderSize = 1.0f;
		style.PopupBorderSize = 1.0f;
		style.FrameBorderSize = 1.0f;
		style.TabBorderSize = 0.0f;
		style.WindowPadding = ImVec2(8.0f, 8.0f);
		style.FramePadding = ImVec2(8.0f, 5.0f);
		style.CellPadding = ImVec2(8.0f, 6.0f);
		style.ItemSpacing = ImVec2(8.0f, 6.0f);
		style.ItemInnerSpacing = ImVec2(6.0f, 5.0f);
		style.IndentSpacing = 20.0f;
		style.ScrollbarSize = 14.0f;
		style.GrabMinSize = 10.0f;

		colors[ImGuiCol_Text] = ImVec4(0.92f, 0.93f, 0.95f, 1.00f);
		colors[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.58f, 0.63f, 1.00f);

		colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
		colors[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.10f, 0.11f, 1.00f);
		colors[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.09f, 0.10f, 0.98f);
		colors[ImGuiCol_Border] = ImVec4(0.22f, 0.24f, 0.29f, 0.78f);
		colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

		colors[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.13f, 0.15f, 1.00f);
		colors[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.17f, 0.20f, 1.00f);
		colors[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);

		colors[ImGuiCol_TitleBg] = ImVec4(0.06f, 0.06f, 0.07f, 1.00f);
		colors[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
		colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.06f, 0.06f, 0.07f, 1.00f);

		colors[ImGuiCol_MenuBarBg] = ImVec4(0.07f, 0.07f, 0.08f, 1.00f);
		colors[ImGuiCol_ScrollbarBg] = ImVec4(0.06f, 0.06f, 0.07f, 0.90f);
		colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.21f, 0.24f, 0.29f, 1.00f);
		colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.27f, 0.31f, 0.37f, 1.00f);
		colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.33f, 0.39f, 0.47f, 1.00f);

        colors[ImGuiCol_CheckMark] = accentBlue;
        colors[ImGuiCol_SliderGrab] = accentBlue;
		colors[ImGuiCol_SliderGrabActive] = ImVec4(0.33f, 0.60f, 0.96f, 1.00f);

		colors[ImGuiCol_Button] = ImVec4(0.15f, 0.17f, 0.20f, 1.00f);
		colors[ImGuiCol_ButtonHovered] = accentBlueSoft;
		colors[ImGuiCol_ButtonActive] = ImVec4(0.24f, 0.44f, 0.70f, 0.95f);

		colors[ImGuiCol_Header] = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
		colors[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.39f, 0.62f, 0.72f);
		colors[ImGuiCol_HeaderActive] = ImVec4(0.26f, 0.46f, 0.74f, 0.86f);

        colors[ImGuiCol_Separator] = ImVec4(0.24f, 0.26f, 0.30f, 0.70f);
        colors[ImGuiCol_SeparatorHovered] = ImVec4(0.28f, 0.52f, 0.86f, 0.70f);
        colors[ImGuiCol_SeparatorActive] = ImVec4(0.32f, 0.60f, 0.96f, 0.84f);

        colors[ImGuiCol_ResizeGrip] = ImVec4(0.30f, 0.55f, 0.85f, 0.20f);
        colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.30f, 0.55f, 0.85f, 0.55f);
        colors[ImGuiCol_ResizeGripActive] = ImVec4(0.30f, 0.55f, 0.85f, 0.80f);

		colors[ImGuiCol_Tab] = ImVec4(0.09f, 0.09f, 0.10f, 1.00f);
		colors[ImGuiCol_TabHovered] = ImVec4(0.18f, 0.32f, 0.52f, 0.88f);
		colors[ImGuiCol_TabActive] = ImVec4(0.12f, 0.16f, 0.22f, 1.00f);
		colors[ImGuiCol_TabUnfocused] = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
		colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.10f, 0.12f, 0.16f, 1.00f);

		colors[ImGuiCol_TableHeaderBg] = ImVec4(0.11f, 0.12f, 0.14f, 1.00f);
		colors[ImGuiCol_TableBorderStrong] = ImVec4(0.21f, 0.23f, 0.27f, 1.00f);
		colors[ImGuiCol_TableBorderLight] = ImVec4(0.15f, 0.17f, 0.20f, 1.00f);
		colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
		colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);

        colors[ImGuiCol_DockingPreview] = ImVec4(0.28f, 0.52f, 0.86f, 0.48f);
        colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.07f, 0.07f, 0.08f, 1.00f);
        colors[ImGuiCol_NavHighlight] = accentBlue;
        colors[ImGuiCol_NavWindowingHighlight] = accentBlue;
        colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.35f);
        colors[ImGuiCol_TextSelectedBg] = ImVec4(0.28f, 0.52f, 0.86f, 0.35f);
        colors[ImGuiCol_DragDropTarget] = ImVec4(0.95f, 0.76f, 0.24f, 0.90f);
        colors[ImGuiCol_PlotHistogram] = accentBlue;
		colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.33f, 0.60f, 0.96f, 1.00f);
        colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.38f);
    }

    void ImGuiLayer::ImguiInit()
    {
        ImguiInit(1.0f, true, false);
    }

    void ImGuiLayer::ImguiInit(
        float initialDpiScale,
        bool bEnableDocking,
        bool bEnableViewports)
    {
        // Setup Dear ImGui context
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO(); (void)io;
#if !defined(WITH_EDITOR)
        // Game builds own the viewport and must not write editor layout state into their working directory.
        io.IniFilename = nullptr;
#endif
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;       // Enable Keyboard Controls
        //io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
        if (bEnableDocking)
            io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        if (bEnableViewports)
            io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        else
            io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
        //io.ConfigViewportsNoAutoMerge = true;
        //io.ConfigViewportsNoTaskBarIcon = true;

        ImguiInitFont(mBaseFontSize);

        // Setup Dear ImGui style
        ImGui::StyleColorsDark();
        //ImGui::StyleColorsClassic();

        // When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones.
        ImGuiStyle& style = ImGui::GetStyle();
        SetDarkThemeColors();

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            style.WindowRounding = 0.0f;
            style.Colors[ImGuiCol_WindowBg].w = 1.0f;
        }

        mBaseStyle = ImGui::GetStyle();
        ApplyUIScale(initialDpiScale);
    }

    void ImGuiLayer::ImguiInitFont(float fontSize)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->Clear();

        ImVector<ImWchar> ranges;
        ImFontGlyphRangesBuilder builder;
#ifdef WITH_EDITOR
        //builder.AddText((const char*)u8"如果不添加全部汉字则可以这样手动添加所需汉字");
        //builder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        builder.AddRanges(io.Fonts->GetGlyphRangesChineseFull());
        builder.BuildRanges(&ranges);

        // remember ranges.Data!
        io.FontDefault = AddFontFromPath(
            *io.Fonts,
            ConfigManager::GetFullPath("Assets/Fonts/Noto_Sans_SC/NotoSansSC-Regular.otf"),
            fontSize,
            ranges.Data);
#else
        builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
        builder.BuildRanges(&ranges);
        io.FontDefault = AddFontFromPath(
            *io.Fonts,
            ConfigManager::GetFullPath("Assets/Fonts/opensans/OpenSans-Regular.ttf"),
            fontSize,
            ranges.Data);
#endif

        // TODO: Change Font. if language is English we do not need to load chinese ttf font file
        //io.Fonts->Clear();  // should after Imgui::Render
        //if (ConfigManager::GetLanguage() == Language::English)
        //{
        //    io.Fonts->AddFontFromFileTTF(ConfigManager::GetFullPath("Assets/fonts/opensans/OpenSans-Bold.ttf").string().c_str(), fontSize);
        //    io.FontDefault = io.Fonts->AddFontFromFileTTF(ConfigManager::GetFullPath("Assets/fonts/opensans/OpenSans-Regular.ttf").string().c_str(), fontSize);
        //}
        //else
        //{
        //    ImVector<ImWchar> ranges;
        //    ImFontGlyphRangesBuilder builder;
        //    //builder.AddText((const char*)u8"如果不添加全部汉字则可以这样手动添加汉字");
        //    //builder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        //    builder.AddRanges(io.Fonts->GetGlyphRangesChineseFull());
        //    builder.BuildRanges(&ranges);

        //    // remember ranges.Data!
        //    io.FontDefault = io.Fonts->AddFontFromFileTTF(ConfigManager::GetFullPath("Assets/Fonts/chinese/W03.ttf").string().c_str(), fontSize, NULL, ranges.Data);
        //    io.Fonts->Build();
        //}

#ifdef WITH_EDITOR
        ImGui::MergeIconsWithLatestFont(16.f, false);
#endif
    }

    void ImGuiLayer::UpdateUIScaleFromWindow()
    {
        // Legacy platform layers call this method without an injected Window.
        // Keep their scale stable until they are removed with the old backends.
        ApplyUIScale(mCurrentDpiScale > 0.0f ? mCurrentDpiScale : 1.0f);
    }

    void ImGuiLayer::ApplyUIScale(float dpiScale)
    {
        dpiScale = std::clamp(dpiScale, 0.75f, 3.0f);
        if (std::abs(dpiScale - mCurrentDpiScale) < 0.01f)
            return;

        mCurrentDpiScale = dpiScale;
        mFontScale = dpiScale;

        ImGuiStyle& style = ImGui::GetStyle();
        style = mBaseStyle;
        style.ScaleAllSizes(mCurrentDpiScale);
        style.FontScaleMain = mFontScale;
        style.FontScaleDpi = 1.0f;
    }

    void ImGuiLayer::SetFontScale(float fontScale)
    {
        mFontScale = fontScale;

        ImGuiStyle& style = ImGui::GetStyle();
        style.FontScaleMain = mFontScale;
        style.FontScaleDpi = 1.0f;
    }

    void ImGuiLayer::ImguiEnd()
    {
        //if (bReloadFont) [[unlikely]]
        //{
        //    ImguiInitFont();
        //    ImGuiLayer::bReloadFont = false;
        //}
    }
}
