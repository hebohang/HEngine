#include "BaseSettingsPanel.h"

#include "Editor/EditorLayer.h"
#include "Editor/ImGuiWrapper/ImGuiWrapper.h"
#include "Runtime/Core/AppFramework/Application.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Utils/PlatformUtils.h"

#include <imgui/imgui.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <exception>
#include <filesystem>
#include <string_view>
#include <utility>

namespace HEngine
{
    namespace
    {
        [[nodiscard]] bool IsChinese()
        {
            return ConfigManager::GetLanguage() == Language::Chinese;
        }

        [[nodiscard]] const char* Localized(const char* chinese, const char* english)
        {
            return IsChinese() ? chinese : english;
        }

        [[nodiscard]] std::string LowerAscii(std::string text)
        {
            std::ranges::transform(text, text.begin(), [](unsigned char value)
            {
                return static_cast<char>(std::tolower(value));
            });
            return text;
        }

        [[nodiscard]] bool IsPathInside(
            const std::filesystem::path& path,
            const std::filesystem::path& root,
            std::filesystem::path& relativePath)
        {
            std::error_code error;
            const std::filesystem::path normalizedPath =
                std::filesystem::weakly_canonical(path, error);
            if (error)
                return false;

            const std::filesystem::path normalizedRoot =
                std::filesystem::weakly_canonical(root, error);
            if (error)
                return false;

            relativePath = normalizedPath.lexically_relative(normalizedRoot);
            if (relativePath.empty() || relativePath.is_absolute())
                return false;

            const auto firstComponent = relativePath.begin();
            return firstComponent != relativePath.end() &&
                *firstComponent != std::filesystem::path("..");
        }

        void DrawPageTitle(const char* title, const char* description)
        {
            ImGui::TextUnformatted(title);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(description);
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }

        void BeginSettingsCard(
            const char* id,
            const char* title,
            const char* description)
        {
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ImGuiWrapper::S(4.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImGuiWrapper::S(14.0f, 12.0f));
            ImVec4 cardColor = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
            cardColor.w *= 0.55f;
            ImGui::PushStyleColor(ImGuiCol_ChildBg, cardColor);
            ImGui::BeginChild(
                id,
                ImVec2(0.0f, 0.0f),
                ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

            ImGui::TextUnformatted(title);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(description);
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
        }

        void EndSettingsCard()
        {
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
        }

        template<typename DrawValue>
        void DrawPropertyRow(
            const char* id,
            const char* label,
            const char* description,
            DrawValue&& drawValue)
        {
            ImGui::PushID(id);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGuiWrapper::S(230.0f));
            ImGui::TextUnformatted(description);
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();

            ImGui::TableSetColumnIndex(1);
            ImGui::AlignTextToFramePadding();
            drawValue();
            ImGui::PopID();
        }

        [[nodiscard]] bool BeginPropertyTable(const char* id)
        {
            const ImGuiTableFlags flags =
                ImGuiTableFlags_SizingStretchProp |
                ImGuiTableFlags_NoPadOuterX |
                ImGuiTableFlags_BordersInnerH;
            if (!ImGui::BeginTable(id, 2, flags))
                return false;

            const float availableWidth = ImGui::GetContentRegionAvail().x;
            const float labelWidth = std::min(
                ImGuiWrapper::S(250.0f),
                std::max(ImGuiWrapper::S(110.0f), availableWidth * 0.42f));
            ImGui::TableSetupColumn(
                "##Property",
                ImGuiTableColumnFlags_WidthFixed,
                labelWidth);
            ImGui::TableSetupColumn(
                "##Value",
                ImGuiTableColumnFlags_WidthStretch,
                1.0f);
            return true;
        }
    }

    BaseSettingsPanel::BaseSettingsPanel(
        EditorLayer* parent,
        std::string panelName,
        bool* pOpen)
        : BasePanel(parent, std::move(panelName), pOpen)
    {
    }

    void BaseSettingsPanel::OnImGuiRender()
    {
        if (IsOpen())
        {
            ImGuiWrapper::SetNextWindowSize(
                ImVec2(960.0f, 680.0f),
                ImGuiCond_Appearing);
        }
        BasePanel::OnImGuiRender();
    }

    const char* BaseSettingsPanel::GetCategoryTitle(Category category) const
    {
        switch (category)
        {
        case Category::Appearance:
            return Localized("常规与外观", "General & Appearance");
        case Category::StartupAndScenes:
            return Localized("启动与场景", "Startup & Scenes");
        case Category::ViewportCamera:
            return Localized("视口相机", "Viewport Camera");
        }
        return "";
    }

    const char* BaseSettingsPanel::GetCategoryDescription(Category category) const
    {
        switch (category)
        {
        case Category::Appearance:
            return Localized(
                "调整仅影响当前编辑器实例的界面显示偏好。",
                "Adjust display preferences for this editor instance.");
        case Category::StartupAndScenes:
            return Localized(
                "选择项目下次启动编辑器时自动打开的场景。",
                "Choose the scene opened automatically the next time this project starts.");
        case Category::ViewportCamera:
            return Localized(
                "调整当前编辑器视口相机的镜头与导航参数。",
                "Adjust lens and navigation parameters for the current editor viewport camera.");
        }
        return "";
    }

    void BaseSettingsPanel::DrawHeader() const
    {
        ImGui::TextUnformatted(Localized("编辑器偏好设置", "Editor Preferences"));
        ImGuiWrapper::SameLine(0.0f, 10.0f);
        ImGui::TextDisabled("%s", Localized("本地编辑体验", "Local editor experience"));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(Localized(
            "这里的选项用于编辑器工作流；项目运行时渲染选项请使用“项目设置”。",
            "These options control the editor workflow. Use Project Settings for runtime rendering options."));
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    void BaseSettingsPanel::DrawNavigation()
    {
        ImGui::TextDisabled("%s", Localized("编辑器", "EDITOR"));
        ImGui::Spacing();

        constexpr Category categories[] = {
            Category::Appearance,
            Category::StartupAndScenes,
            Category::ViewportCamera
        };
        for (Category category : categories)
        {
            ImGui::PushID(static_cast<int>(category));
            if (ImGui::Selectable(
                    GetCategoryTitle(category),
                    mSelectedCategory == category,
                    ImGuiSelectableFlags_None,
                    ImVec2(0.0f, ImGuiWrapper::S(32.0f))))
            {
                mSelectedCategory = category;
            }
            ImGui::PopID();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", Localized(
            "渲染、抗锯齿、全局光照与后期处理位于“项目设置”。",
            "Rendering, anti-aliasing, global illumination, and post-processing are in Project Settings."));
        ImGui::PopTextWrapPos();
    }

    void BaseSettingsPanel::DrawAppearanceSettings()
    {
        BeginSettingsCard(
            "##AppearanceCard",
            Localized("界面缩放", "Interface Scale"),
            Localized(
                "在高分辨率显示器上调整编辑器文字大小，不改变游戏或项目内容。",
                "Adjust editor text size on high-resolution displays without changing game or project content."));

        if (BeginPropertyTable("##AppearanceProperties"))
        {
            DrawPropertyRow(
                "FontScale",
                mParent->GetText("Font Size"),
                Localized("立即生效，仅保留到本次编辑器会话结束。", "Applies immediately for this editor session only."),
                []()
                {
                    ImGuiLayer* imguiLayer = Application::GetInstance().GetImGuiLayer();
                    float fontScale = imguiLayer
                        ? imguiLayer->GetFontScale()
                        : ImGui::GetStyle().FontScaleMain;

                    const float resetButtonWidth =
                        ImGui::CalcTextSize(Localized("跟随 DPI", "Use DPI")).x +
                        ImGui::GetStyle().FramePadding.x * 2.0f;
                    const float availableWidth = ImGui::GetContentRegionAvail().x;
                    const bool bUseSingleLine = availableWidth >=
                        resetButtonWidth + ImGuiWrapper::S(130.0f);
                    ImGui::SetNextItemWidth(bUseSingleLine
                        ? availableWidth - resetButtonWidth - ImGuiWrapper::S(10.0f)
                        : -FLT_MIN);
                    if (ImGui::SliderFloat("##Value", &fontScale, 0.75f, 3.0f, "%.2fx") && imguiLayer)
                        imguiLayer->SetFontScale(fontScale);

                    if (bUseSingleLine)
                        ImGuiWrapper::SameLine(0.0f, 8.0f);
                    if (ImGui::Button(Localized("跟随 DPI", "Use DPI")) && imguiLayer)
                        imguiLayer->SetFontScale(imguiLayer->GetUIScale());
                });
            ImGui::EndTable();
        }
        EndSettingsCard();

        ImGui::Spacing();
        BeginSettingsCard(
            "##WindowModeCard",
            Localized("窗口模式", "Window Mode"),
            Localized(
                "控制编辑器主窗口的显示方式，并为当前项目保存该偏好。",
                "Control how the main editor window is displayed and save the preference for this project."));

        if (BeginPropertyTable("##WindowModeProperties"))
        {
            DrawPropertyRow(
                "Fullscreen",
                Localized("全屏", "Fullscreen"),
                Localized("使用当前显示器的全屏模式；关闭后恢复之前的窗口位置和大小。", "Use the current monitor's fullscreen mode; disabling it restores the previous window position and size."),
                [this]()
                {
                    Window& window = Application::GetWindow();
                    bool bFullscreen = window.IsFullscreen();
                    if (!ImGui::Checkbox("##Value", &bFullscreen))
                        return;

                    window.SetFullscreen(bFullscreen);
                    if (window.IsFullscreen() != bFullscreen)
                        return;

                    const Json previousConfig = mParent->mBaseEditorJson;
                    mParent->mBaseEditorJson["Fullscreen"] = bFullscreen;
                    try
                    {
                        mParent->SaveBaseEditorConfig();
                    }
                    catch (const std::exception& exception)
                    {
                        mParent->mBaseEditorJson = previousConfig;
                        window.SetFullscreen(!bFullscreen);
                        HE_CORE_ERROR("Could not save the editor fullscreen preference: {0}", exception.what());
                    }
                });
            ImGui::EndTable();
        }
        EndSettingsCard();
    }

    void BaseSettingsPanel::SelectDefaultScene()
    {
        std::string sceneFilter = std::string(mParent->GetText("Scene")) + " (*.he)";
        sceneFilter.push_back('\0');
        sceneFilter += "*.he";
        sceneFilter.push_back('\0');

        const std::filesystem::path selectedPath = FileDialogs::OpenFile(
            sceneFilter.c_str(),
            ConfigManager::GetInstance().GetAssetsFolder().string());
        if (selectedPath.empty())
            return;

        if (LowerAscii(selectedPath.extension().string()) != ".he")
        {
            mStartupMessage = Localized(
                "未保存：请选择 .he 场景文件。",
                "Not saved: select a .he scene file.");
            bStartupMessageError = true;
            return;
        }

        std::filesystem::path relativeToAssets;
        if (!IsPathInside(
                selectedPath,
                ConfigManager::GetInstance().GetAssetsFolder(),
                relativeToAssets))
        {
            mStartupMessage = Localized(
                "未保存：默认场景必须位于当前项目的 Assets 目录内。",
                "Not saved: the default scene must be inside this project's Assets directory.");
            bStartupMessageError = true;
            return;
        }

        const std::string projectRelativePath =
            (std::filesystem::path("Assets") / relativeToAssets).generic_string();
        const Json previousConfig = mParent->mBaseEditorJson;
        mParent->mBaseEditorJson["DefaultScene"] = projectRelativePath;
        try
        {
            mParent->SaveBaseEditorConfig();
            mStartupMessage = Localized(
                "已保存。新默认场景将在下次启动编辑器时打开。",
                "Saved. The new default scene will open the next time the editor starts.");
            bStartupMessageError = false;
        }
        catch (const std::exception& exception)
        {
            mParent->mBaseEditorJson = previousConfig;
            mStartupMessage = std::string(Localized("保存失败：", "Save failed: ")) + exception.what();
            bStartupMessageError = true;
        }
    }

    void BaseSettingsPanel::DrawStartupAndSceneSettings()
    {
        BeginSettingsCard(
            "##StartupSceneCard",
            Localized("项目启动", "Project Startup"),
            Localized(
                "默认场景是项目级编辑器偏好，保存在 Project/Config/BaseEditor.json。",
                "The default scene is a project-level editor preference stored in Project/Config/BaseEditor.json."));

        if (BeginPropertyTable("##StartupProperties"))
        {
            DrawPropertyRow(
                "DefaultScene",
                mParent->GetText("Default Scene"),
                Localized("只接受当前项目 Assets 目录中的 .he 场景。", "Only .he scenes inside this project's Assets directory are accepted."),
                [this]()
                {
                    std::string defaultScene =
                        mParent->mBaseEditorJson.contains("DefaultScene") &&
                        mParent->mBaseEditorJson["DefaultScene"].is_string()
                            ? mParent->mBaseEditorJson["DefaultScene"].get<std::string>()
                            : std::string{};

                    if (ImGui::BeginTable(
                            "##DefaultSceneValue",
                            2,
                            ImGuiTableFlags_SizingStretchProp |
                                ImGuiTableFlags_NoPadOuterX |
                                ImGuiTableFlags_NoPadInnerX))
                    {
                        ImGui::TableSetupColumn("##Path", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                        ImGui::TableSetupColumn(
                            "##Browse",
                            ImGuiTableColumnFlags_WidthFixed,
                            ImGui::CalcTextSize(mParent->GetText("Browse")).x +
                                ImGui::GetStyle().FramePadding.x * 2.0f +
                                ImGuiWrapper::S(8.0f));
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        ImGui::InputText(
                            "##PathValue",
                            defaultScene.data(),
                            defaultScene.size() + 1,
                            ImGuiInputTextFlags_ReadOnly);

                        ImGui::TableSetColumnIndex(1);
                        if (ImGui::Button(mParent->GetText("Browse")))
                            SelectDefaultScene();
                        ImGui::EndTable();
                    }

                    if (defaultScene.empty())
                    {
                        ImGui::TextColored(
                            ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
                            "%s",
                            Localized("当前配置没有有效的默认场景。", "The current configuration has no valid default scene."));
                    }
                    else if (!std::filesystem::is_regular_file(
                                 ConfigManager::GetFullPath(defaultScene)))
                    {
                        ImGui::TextColored(
                            ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
                            "%s",
                            Localized("找不到这个场景文件。", "This scene file could not be found."));
                    }
                });
            ImGui::EndTable();
        }

        if (!mStartupMessage.empty())
        {
            const ImVec4 messageColor = bStartupMessageError
                ? ImVec4(1.0f, 0.45f, 0.35f, 1.0f)
                : ImVec4(0.45f, 0.85f, 0.55f, 1.0f);
            ImGui::Spacing();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(messageColor, "%s", mStartupMessage.c_str());
            ImGui::PopTextWrapPos();
        }
        EndSettingsCard();
    }

    void BaseSettingsPanel::DrawViewportCameraSettings()
    {
        BeginSettingsCard(
            "##ViewportCameraCard",
            mParent->GetText("Editor Camera"),
            Localized(
                "这些参数立即作用于当前视口相机，不会修改场景中的 Camera 组件。",
                "These values affect the current viewport camera immediately and do not modify Camera components in the scene."));

        const char* resetLabel = Localized("恢复会话默认值", "Reset Session Defaults");
        const float resetWidth = ImGui::CalcTextSize(resetLabel).x +
            ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetCursorPosX(std::max(
            ImGui::GetCursorPosX(),
            ImGui::GetWindowContentRegionMax().x - resetWidth));
        if (ImGui::Button(resetLabel))
        {
            mParent->mEditorCamera.mFOV = 30.0f;
            mParent->mEditorCamera.mNearClip = 0.1f;
            mParent->mEditorCamera.mFarClip = 10000.0f;
            mParent->mEditorCamera.mCameraSpeed = 1.0f;
            mParent->mEditorCamera.UpdateProjection();
        }
        ImGui::Spacing();

        if (BeginPropertyTable("##ViewportCameraProperties"))
        {
            DrawPropertyRow(
                "CameraFov",
                mParent->GetText("FOV"),
                Localized("透视视口的垂直视野角。", "Vertical field of view for the perspective viewport."),
                [this]()
                {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::SliderFloat(
                            "##Value",
                            &mParent->mEditorCamera.mFOV,
                            1.0f,
                            179.0f,
                            "%.1f deg"))
                    {
                        mParent->mEditorCamera.UpdateProjection();
                    }
                });
            DrawPropertyRow(
                "CameraNearClip",
                mParent->GetText("Near Clip"),
                Localized("靠近相机的可见距离下限。", "Nearest visible distance from the camera."),
                [this]()
                {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::DragFloat(
                            "##Value",
                            &mParent->mEditorCamera.mNearClip,
                            0.01f,
                            0.01f,
                            std::max(0.01f, mParent->mEditorCamera.mFarClip - 0.01f),
                            "%.2f"))
                    {
                        mParent->mEditorCamera.UpdateProjection();
                    }
                });
            DrawPropertyRow(
                "CameraFarClip",
                mParent->GetText("Far Clip"),
                Localized("远离相机的可见距离上限。", "Farthest visible distance from the camera."),
                [this]()
                {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::DragFloat(
                            "##Value",
                            &mParent->mEditorCamera.mFarClip,
                            10.0f,
                            mParent->mEditorCamera.mNearClip + 0.01f,
                            100000.0f,
                            "%.1f"))
                    {
                        mParent->mEditorCamera.UpdateProjection();
                    }
                });
            DrawPropertyRow(
                "CameraSpeed",
                mParent->GetText("Camera Speed"),
                Localized("控制自由视角移动速度，也可在视口工具栏中调整。", "Controls free-camera movement speed and can also be changed from the viewport toolbar."),
                [this]()
                {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::SliderFloat(
                        "##Value",
                        &mParent->mEditorCamera.mCameraSpeed,
                        0.1f,
                        100.0f,
                        "%.1fx",
                        ImGuiSliderFlags_Logarithmic);
                });
            ImGui::EndTable();
        }
        EndSettingsCard();
    }

    void BaseSettingsPanel::DrawSelectedCategory()
    {
        DrawPageTitle(
            GetCategoryTitle(mSelectedCategory),
            GetCategoryDescription(mSelectedCategory));

        switch (mSelectedCategory)
        {
        case Category::Appearance:
            DrawAppearanceSettings();
            break;
        case Category::StartupAndScenes:
            DrawStartupAndSceneSettings();
            break;
        case Category::ViewportCamera:
            DrawViewportCameraSettings();
            break;
        }
    }

    void BaseSettingsPanel::OnImGuiRenderImpl()
    {
        DrawHeader();

        const float availableWidth = ImGui::GetContentRegionAvail().x;
        const float navigationWidth = std::min(
            ImGuiWrapper::S(220.0f),
            std::max(ImGuiWrapper::S(130.0f), availableWidth * 0.30f));
        const ImGuiTableFlags layoutFlags =
            ImGuiTableFlags_SizingFixedFit |
            ImGuiTableFlags_NoPadOuterX |
            ImGuiTableFlags_BordersInnerV;
        if (!ImGui::BeginTable("##EditorPreferencesLayout", 2, layoutFlags))
            return;

        ImGui::TableSetupColumn(
            "##Navigation",
            ImGuiTableColumnFlags_WidthFixed,
            navigationWidth);
        ImGui::TableSetupColumn(
            "##Settings",
            ImGuiTableColumnFlags_WidthStretch,
            1.0f);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImGuiWrapper::S(10.0f, 10.0f));
        if (ImGui::BeginChild("##EditorPreferencesNavigation"))
            DrawNavigation();
        ImGui::EndChild();
        ImGui::PopStyleVar();

        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImGuiWrapper::S(18.0f, 6.0f));
        if (ImGui::BeginChild(
                "##EditorPreferencesContent",
                ImVec2(0.0f, 0.0f),
                ImGuiChildFlags_None))
        {
            DrawSelectedCategory();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();

        ImGui::EndTable();
    }
}
