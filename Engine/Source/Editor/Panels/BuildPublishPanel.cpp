#include "BuildPublishPanel.h"

#include "Editor/ImGuiWrapper/ImGuiWrapper.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/ModeManager/ModeManager.h"
#include "Runtime/Utils/PlatformUtils.h"
#include "Runtime/Platform/PlatformFileSystem.h"
#include "Runtime/Platform/PlatformPaths.h"

#include <imgui/imgui.h>

#include <array>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace HEngine
{
    namespace
    {
        std::string Utf8(const std::filesystem::path& path)
        {
            const auto text = path.u8string();
            return {text.begin(), text.end()};
        }
        void EditText(const char* label, Json& object, const char* key)
        {
            std::array<char, 1024> buffer{};
            const auto value = object.value(key, std::string{});
            std::copy_n(value.data(), std::min(value.size(), buffer.size() - 1), buffer.data());
            if (ImGui::InputText(label, buffer.data(), buffer.size())) object[key] = buffer.data();
        }
        PlatformProcess::CapturedResult Run(const std::vector<std::string>& arguments, std::stop_token stop)
        {
            try { return PlatformProcess::RunCaptured(PlatformPaths::GetPowerShellExecutable(), arguments, stop, 7200); }
            catch (const std::exception& error) { return {-1, false, error.what()}; }
        }
    }
    BuildPublishPanel::~BuildPublishPanel() { Shutdown(); }
    std::filesystem::path BuildPublishPanel::Root() const { return ConfigManager::GetInstance().GetRootFolder(); }
    void BuildPublishPanel::Shutdown()
    {
        mBuildStop.request_stop(); mPreviewStop.request_stop();
        if (mBuild.valid()) mBuild.wait();
        if (mPreview.valid()) mPreview.wait();
        mBuildAndPreview = false;
        Poll();
    }
    void BuildPublishPanel::Open() { mOpen = true; if (!mLoaded) Load(); }
    void BuildPublishPanel::Load()
    {
        try
        {
            std::ifstream stream(Root() / "Config/BuildProfiles.json");
            if (stream) stream >> mConfig;
            else mConfig = {{"schema", 1}, {"activeProfile", "windows"}, {"profiles", Json::array({
                {{"id", "windows"}, {"name", "Windows 桌面"}, {"platform", "Windows"}, {"configuration", "Release"}, {"output", "Dist/Windows"}},
                {{"id", "android"}, {"name", "Android 原生 Vulkan"}, {"platform", "Android"}, {"quality", "Desktop"}, {"configuration", "Release"}, {"output", "Dist/Android"}, {"androidConfig", "Config/Android.json"}},
                {{"id", "web"}, {"name", "Web 浏览器"}, {"platform", "Web"}, {"configuration", "Release"}, {"output", "Dist/Web"}, {"previewPort", 8769}, {"managedProject", ""}, {"contentExporter", ""}},
                {{"id", "wechat"}, {"name", "微信小游戏"}, {"platform", "WeChat"}, {"quality", "Portable"}, {"managedProject", ""}, {"contentExporter", ""}, {"platformExporter", ""}, {"configuration", "Release"}, {"output", "Dist/WeChat"}}
            })}};
            if (mConfig.value("schema", 0) != 1 || !mConfig.at("profiles").is_array() || mConfig["profiles"].empty())
                throw std::runtime_error("Invalid build profiles");
#if defined(HE_PLATFORM_MACOS)
            // Existing projects gain an editable local-native profile when first
            // opened on macOS; Save/Build persists it with the project's settings.
            const bool bHasMacProfile = std::any_of(mConfig["profiles"].begin(), mConfig["profiles"].end(),
                [](const Json& profile) { return profile.value("platform", "") == "macOS"; });
            if (!bHasMacProfile)
            {
                std::string macProfileId = "macos";
                for (int suffix = 1; std::any_of(mConfig["profiles"].begin(), mConfig["profiles"].end(),
                    [&macProfileId](const Json& profile) { return profile.value("id", "") == macProfileId; }); ++suffix)
                    macProfileId = "macos_" + std::to_string(suffix);
                mConfig["profiles"].push_back({{"id", macProfileId}, {"name", "macOS Apple Silicon"},
                    {"platform", "macOS"}, {"quality", "Desktop"}, {"configuration", "Release"}, {"output", "Dist/macOS"}});
                mConfig["activeProfile"] = macProfileId;
            }
#endif
            mSelected = 0;
            for (int i = 0; i < static_cast<int>(mConfig["profiles"].size()); ++i)
                if (mConfig["profiles"][i].value("id", "") == mConfig.value("activeProfile", "")) mSelected = i;
            mLoaded = true; mStatus = "选择平台配置，检查环境后构建。输出可直接交付或上传到静态网站。";
        }
        catch (const std::exception& error) { mLoaded = false; mStatus = error.what(); }
    }
    bool BuildPublishPanel::Save()
    {
        try
        {
            mConfig["activeProfile"] = mConfig["profiles"][mSelected]["id"];
            std::filesystem::create_directories(Root() / "Config");
            const auto path = Root() / "Config/BuildProfiles.json";
            std::ofstream stream(path); stream << mConfig.dump(2) << '\n'; stream.close();
            if (!stream) throw std::runtime_error("Cannot save build profiles");
            return true;
        }
        catch (const std::exception& error) { mStatus = error.what(); return false; }
    }
    void BuildPublishPanel::Start(const std::string& action)
    {
        if (Busy() || !Save()) { mBuildAndPreview = false; return; }
        mRunningProfile = mConfig["profiles"][mSelected].value("id", "");
        mLog.clear(); mLogPath.clear(); mStatus = action == "Check" ? "正在检查构建环境…" : "正在构建，请查看日志。关闭本窗口不会中断构建。";
        mBuildStop = {};
        const auto script = ConfigManager::GetEngineFolder().parent_path() / "Tools/Build/Publish.ps1";
        const std::vector<std::string> arguments{"-NoProfile", "-ExecutionPolicy", "Bypass", "-File", Utf8(script), "-Project", Utf8(Root()), "-Profile", mRunningProfile, "-Action", action};
        mBuild = std::async(std::launch::async, [arguments, stop = mBuildStop.get_token()] { return Run(arguments, stop); });
    }
    void BuildPublishPanel::Preview()
    {
        if (mPreview.valid() || !Save()) return;
        mPreviewStop = {};
        const auto script = ConfigManager::GetEngineFolder().parent_path() / "Tools/Build/Preview.ps1";
        const std::vector<std::string> arguments{"-NoProfile", "-ExecutionPolicy", "Bypass", "-File", Utf8(script), "-Project", Utf8(Root()), "-Profile", mConfig["profiles"][mSelected].value("id", ""), "-OpenBrowser"};
        mPreview = std::async(std::launch::async, [arguments, stop = mPreviewStop.get_token()] { return Run(arguments, stop); });
        mStatus = "正在启动浏览器预览。停止预览或关闭 Editor 会关闭本次预览服务。";
    }
    void BuildPublishPanel::Poll()
    {
        if (Busy() && std::chrono::steady_clock::now() >= mNextLogRead)
        {
            mNextLogRead = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            try
            {
                std::string receipt;
                if (PlatformFileSystem::ReadSharedTextTail(Root() / "Saved/Build" / (mRunningProfile + ".json"), 65536, receipt))
                    mLogPath = std::filesystem::u8path(Json::parse(receipt).value("log", ""));
                (void)PlatformFileSystem::ReadSharedTextTail(mLogPath, 24000, mLog);
            }
            catch (...) { /* Receipt is replaced while the worker changes stages. Retry next frame. */ }
        }
        if (Busy() && mBuild.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            const auto result = mBuild.get();
            if (result.Cancelled)
            {
                try
                {
                    const auto path = Root() / "Saved/Build" / (mRunningProfile + ".json");
                    std::ifstream stream(path); Json receipt; stream >> receipt; stream.close();
                    if (receipt.value("status", "") == "Running")
                    {
                        receipt["status"] = "Cancelled"; receipt["success"] = false;
                        receipt["error"] = "Cancelled by Editor";
                        std::ofstream output(path); output << receipt.dump(2);
                    }
                }
                catch (...) { /* A worker that failed before creating a receipt has no file to update. */ }
            }
            mStatus = result.Cancelled ? "构建已取消；上一次成功输出保留。" : result.ExitCode == 0 ? "操作成功，可打开输出目录或启动预览。" : "构建失败，请查看下方日志。";
            mLog += '\n' + result.Output;
            const bool bPreview = mBuildAndPreview && result.ExitCode == 0; mBuildAndPreview = false;
            if (bPreview) Preview();
        }
        if (mPreview.valid() && mPreview.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            const auto result = mPreview.get();
            mStatus = result.Cancelled ? "预览已停止。" : result.ExitCode != 0 ? "预览启动失败，请查看日志。" : "预览命令已完成；微信开发者工具可继续使用。";
            mLog += '\n' + result.Output;
        }
    }
    void BuildPublishPanel::Draw()
    {
        Poll(); if (!mOpen) return;
        ImGuiWrapper::ConstrainNextWindowToWorkArea("发布###BuildPublish", ImVec2(960, 720), ImVec2(720, 520));
        if (!ImGui::Begin("发布###BuildPublish", &mOpen)) { ImGui::End(); return; }
        if (!mLoaded) { ImGui::TextWrapped("%s", mStatus.c_str()); if (ImGui::Button("重新加载")) Load(); ImGui::End(); return; }
        const bool bBusy = Busy();
        bool bReloadRequested = false;
        ImGui::TextUnformatted("发布项目");
        ImGui::TextDisabled("选择平台与输出位置，检查环境，然后构建交付版本。");
        ImGui::Separator();
        ImGui::BeginDisabled(bBusy || mPreview.valid());
        const float settingsHeight = std::clamp(ImGui::GetContentRegionAvail().y - ImGuiWrapper::S(210), ImGuiWrapper::S(160), ImGuiWrapper::S(320));
        ImGui::BeginChild("profiles", ImVec2(ImGuiWrapper::S(180), settingsHeight), true);
        auto& profiles = mConfig["profiles"];
        for (int i = 0; i < static_cast<int>(profiles.size()); ++i)
        {
            ImGui::PushID(i);
            if (ImGui::Selectable(profiles[i].value("name", "Profile").c_str(), mSelected == i)) mSelected = i;
            ImGui::PopID();
        }
        if (ImGui::Button("复制配置"))
        {
            auto copy = profiles[mSelected]; copy["id"] = "profile_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            copy["name"] = copy.value("name", "") + " 副本"; copy["output"] = "Dist/" + copy["id"].get<std::string>();
            profiles.push_back(copy); mSelected = static_cast<int>(profiles.size()) - 1;
        }
        ImGui::EndChild(); ImGuiWrapper::SameLine(0.0f);
        ImGui::BeginChild("settings", ImVec2(0, settingsHeight), true);
        auto& profile = profiles[mSelected]; const auto platform = profile.value("platform", "");
        ImGui::Text("目标平台：%s", platform.c_str());
        ImGui::SetNextItemWidth(ImGuiWrapper::S(280)); EditText("配置名称", profile, "name");
        ImGui::TextUnformatted("输出目录");
        ImGui::SetNextItemWidth(std::max(ImGuiWrapper::S(160), ImGui::GetContentRegionAvail().x - ImGuiWrapper::S(145)));
        EditText("##Output", profile, "output");
        ImGuiWrapper::SameLine(0.0f);
        if (ImGui::Button("选择目录"))
        {
            const auto current = Root() / std::filesystem::u8path(profile.value("output", "Dist"));
            const auto selected = FileDialogs::SelectFolder(Utf8(current));
            if (!selected.empty()) profile["output"] = Utf8(selected.lexically_normal());
        }
        ImGuiWrapper::SameLine(0.0f);
        if (ImGui::Button("默认")) profile["output"] = "Dist/" + profile.value("id", "windows");
        ImGui::TextWrapped("支持项目相对路径或绝对路径。请选择空目录；后续构建只替换此配置生成的输出。");
        const bool bPortable = profile.value("quality", (platform == "Windows" || platform == "Android" || platform == "macOS") ? "Desktop" : "Portable") == "Portable";
        if (ImGui::BeginCombo("画质档位", bPortable ? "便携 / Portable" : "桌面 / Desktop"))
        {
            if (ImGui::Selectable("桌面 / Desktop", !bPortable)) profile["quality"] = "Desktop";
            if (ImGui::Selectable("便携 / Portable", bPortable)) profile["quality"] = "Portable";
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("分档影响表现资源；玩法和存档规则保持一致。");
        if (platform == "Web" || platform == "WeChat")
        {
            const bool bDebug = profile.value("configuration", "Release") == "Debug";
            if (ImGui::BeginCombo("构建类型", bDebug ? "开发 / Debug" : "发布 / Release"))
            {
                if (ImGui::Selectable("发布 / Release", !bDebug)) profile["configuration"] = "Release";
                if (ImGui::Selectable("开发 / Debug", bDebug)) profile["configuration"] = "Debug";
                ImGui::EndCombo();
            }
            if (platform == "Web") { int port = profile.value("previewPort", 8769); if (ImGui::InputInt("预览端口", &port)) profile["previewPort"] = port; }
            else { EditText("开发者工具 CLI", profile, "devToolsPath"); ImGui::TextDisabled("选择安装目录内 cli.bat；也可配置 HE_WECHAT_CLI。"); }
            if (ImGui::TreeNode("项目适配设置"))
            {
                EditText("C# 项目", profile, "managedProject"); EditText("资源导出脚本", profile, "contentExporter");
                if (platform == "WeChat") EditText("小游戏适配脚本", profile, "platformExporter");
                ImGui::TextWrapped("入口场景和资源范围由项目的导出器决定；引擎不引用任何游戏脚本。脚本接收 --output 参数。");
                ImGui::TreePop();
            }
        }
        else if (platform == "Android")
        {
            EditText("Android 配置", profile, "androidConfig");
            ImGui::TextWrapped("原生 Vulkan / ARM64 / 默认横屏。应用标识、版本和签名方式由项目 Android 配置决定。");
            ImGui::TextWrapped("构建输出 APK 或 AAB。开发签名用于安装测试；正式发布需配置签名密钥环境变量。");
        }
        else if (platform == "Windows" || platform == "macOS")
        {
            EditText("游戏配置（可选）", profile, "gameConfig");
            EditText("插件配置（可选）", profile, "pluginConfig");
            ImGui::TextWrapped("Release / Shipping。留空使用项目默认配置；通过 Cooker 验证后发布。");
            if (platform == "macOS")
                ImGui::TextWrapped("Apple Silicon / Vulkan · MoltenVK。输出独立 .app，可在 Mac 上直接启动。");
        }
        auto unavailable = profile.value("unavailableReason", "");
#if defined(HE_PLATFORM_MACOS)
        if (platform != "macOS" && unavailable.empty())
            unavailable = "该平台的 macOS 主机构建流程尚未接入。请选择 macOS 配置。";
#endif
        if (!unavailable.empty()) ImGui::TextWrapped("%s", unavailable.c_str());
        ImGui::EndChild();
        if (ImGui::Button("保存配置")) { if (Save()) mStatus = "配置已保存到当前项目。"; }
        ImGuiWrapper::SameLine(0.0f);
        if (ImGui::Button("重新加载")) ImGui::OpenPopup("重新加载配置");
        if (ImGui::BeginPopup("重新加载配置"))
        {
            ImGui::TextUnformatted("放弃未保存的修改并重新加载？");
            if (ImGui::Button("放弃并加载")) { bReloadRequested = true; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::BeginDisabled(bBusy || !ModeManager::IsEditState() || !unavailable.empty() || mPreview.valid());
        if (ImGui::Button("检查环境")) Start("Check"); ImGuiWrapper::SameLine(0.0f);
        if (ImGui::Button("构建")) Start("Build"); ImGuiWrapper::SameLine(0.0f);
        ImGui::BeginDisabled(platform != "Web" && platform != "WeChat");
        if (ImGui::Button("构建并预览")) { mBuildAndPreview = true; Start("Build"); } ImGuiWrapper::SameLine(0.0f);
        if (ImGui::Button("预览已有构建")) Preview();
        ImGui::EndDisabled(); ImGui::EndDisabled();
        if (bBusy) { ImGuiWrapper::SameLine(0.0f); if (ImGui::Button("取消构建")) mBuildStop.request_stop(); }
        if (mPreview.valid()) { ImGuiWrapper::SameLine(0.0f); if (ImGui::Button("停止预览")) mPreviewStop.request_stop(); }
        const auto output = Root() / std::filesystem::u8path(profiles[mSelected].value("output", "Dist"));
        std::error_code outputError;
        ImGui::BeginDisabled(!std::filesystem::is_directory(output, outputError));
        if (ImGui::Button("打开输出目录")) FileDialogs::OpenInExplorer(Utf8(output)); ImGui::EndDisabled(); ImGuiWrapper::SameLine(0.0f);
        ImGui::BeginDisabled(mLogPath.empty()); if (ImGui::Button("定位完整日志")) FileDialogs::OpenInExplorer(Utf8(mLogPath)); ImGui::EndDisabled();
        if (!ModeManager::IsEditState()) ImGui::TextDisabled("停止运行场景后可构建发布版本。");
        ImGui::TextWrapped("%s", mStatus.c_str());
        ImGui::BeginChild("build-log", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
        const bool bFollowLog = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
        ImGui::TextUnformatted(mLog.c_str());
        if (bFollowLog) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild(); ImGui::End();
        if (bReloadRequested) Load();
    }
}
