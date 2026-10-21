#include "PluginPanel.h"
#include "PluginPanelBuildConfig.h"

#include "Editor/BinaryPluginSettings.h"
#include "Editor/ImGuiWrapper/ImGuiWrapper.h"
#include "Runtime/Core/Modules/ModuleManager.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/ModeManager/ModeManager.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"

#include <imgui/imgui.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>

namespace HEngine
{
    namespace
    {
        constexpr const char* c_Profiles[] = {"Plugins.json", "Plugins.Portable.json"};
        constexpr const char* c_Hosts[] = {"Editor", "DevGame", "Game", "Tests", "Tools", "RenderSmoke"};
        const char* Text(const char* english, const char* chinese)
        {
            return ConfigManager::GetLanguage() == Language::Chinese ? chinese : english;
        }
        bool Includes(const Json& list, const std::string& value)
        {
            return list.is_array() && std::find(list.begin(), list.end(), value) != list.end();
        }
        std::string Utf8(const std::filesystem::path& path)
        {
            const auto text = path.u8string();
            return {text.begin(), text.end()};
        }
        const char* StateName(PluginState state)
        {
            switch (state)
            {
            case PluginState::Discovered: return "Discovered";
            case PluginState::Registered: return "Registered";
            case PluginState::Loading: return "Loading";
            case PluginState::Loaded: return "Loaded";
            case PluginState::Started: return "Started";
            case PluginState::Stopping: return "Stopping";
            case PluginState::Unloaded: return "Unloaded";
            case PluginState::Failed: return "Failed";
            }
            return "Unknown";
        }
    }
    PluginPanel::PluginPanel(EditorLayer* parent, std::string name, bool* open)
        : BasePanel(parent, std::move(name), open) {}

    PluginPanel::~PluginPanel()
    {
        if (mWorker.joinable()) { mWorker.request_stop(); mWorker.join(); }
    }

    void PluginPanel::OnImGuiRender()
    {
        if (!IsOpen()) return;
        ImGuiWrapper::SetNextWindowSize(ImVec2(900, 650), ImGuiCond_FirstUseEver);
        BasePanel::OnImGuiRender();
    }

    void PluginPanel::Reload()
    {
        bInitialized = false;
        mManifests.clear();
        const auto path = ConfigManager::GetProjectFullPath(std::string("Config/") + c_Profiles[mProfile]);
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("Cannot read plugin configuration: " + Utf8(path));
        mOriginalText = {std::istreambuf_iterator<char>(input), {}};
        if (mOriginalText.starts_with("\xEF\xBB\xBF")) mOriginalText.erase(0, 3);
        mOriginal = Json::parse(mOriginalText);
        if (!mOriginal.is_object()) throw std::runtime_error("Plugin configuration must be an object.");
        for (const auto& [id, settings] : mOriginal.items())
        {
            if (!settings.is_object()) throw std::runtime_error("Plugin host settings must be an object: " + id);
            for (const auto& [host, enabled] : settings.items())
                if (!enabled.is_boolean()) throw std::runtime_error("Plugin switch must be boolean: " + id + "." + host);
        }
        mPending = mOriginal;
        const auto root = ConfigManager::GetEngineFullPath("Source/Plugins");
        for (const auto& entry : std::filesystem::directory_iterator(root))
        {
            const auto manifestPath = entry.path() / "Plugin.json";
            if (!entry.is_directory() || !std::filesystem::is_regular_file(manifestPath)) continue;
            auto manifest = Utils::LoadJsonFile(manifestPath);
            if (!manifest.contains("Id") || !manifest["Id"].is_string())
                throw std::runtime_error("Invalid plugin manifest: " + Utf8(manifestPath));
            for (const auto* name : {"DisplayName", "Version"})
                if (manifest.contains(name) && !manifest[name].is_string())
                    throw std::runtime_error(std::string("Invalid manifest string: ") + name);
            mManifests.push_back(std::move(manifest));
        }
        std::sort(mManifests.begin(), mManifests.end(), [](const Json& a, const Json& b) {
            const auto left = a.at("Id").get<std::string>();
            const auto right = b.at("Id").get<std::string>();
            const bool leftScript = left.starts_with("he.script.");
            const bool rightScript = right.starts_with("he.script.");
            return leftScript != rightScript ? leftScript : left < right;
        });
        bInitialized = true;
    }

    void PluginPanel::Apply()
    {
        const auto request = ConfigManager::GetSavedPath("PluginSettings/request-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
        std::filesystem::create_directories(request.parent_path());
        Utils::SaveJsonToFile(Json{{"Profile", c_Profiles[mProfile]}, {"OriginalText", mOriginalText}, {"Pending", mPending}}, request);
        const std::vector<std::string> arguments = {"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File",
            HE_PLUGIN_SETTINGS_SCRIPT, "-Project", Utf8(ConfigManager::GetInstance().GetRootFolder()),
            "-Request", Utf8(request), "-CMake", HE_PLUGIN_SETTINGS_CMAKE};
        bRunning = true;
        try
        {
            mWorker = std::jthread([this, arguments](std::stop_token stop) {
                PlatformProcess::CapturedResult result;
                try { result = PlatformProcess::RunCaptured(std::filesystem::u8path(HE_PLUGIN_SETTINGS_POWERSHELL), arguments, stop); }
                catch (const std::exception& error) { result.Output = error.what(); }
                { std::lock_guard lock(mMutex); mResult = std::move(result); }
                bFinished = true;
                bRunning = false;
            });
        }
        catch (...) { bRunning = false; throw; }
    }

    void PluginPanel::OnImGuiRenderImpl()
    {
        if (std::filesystem::is_regular_file(ConfigManager::GetEngineFolder().parent_path() / "HEngine.distribution.json"))
        {
            try
            {
                if (!bInitialized)
                {
                    mPending["OfflineReferenceRenderer"] = BinaryPluginSettings::OfflineRendererEnabled();
                    const auto contractPath = ConfigManager::GetEngineFolder().parent_path() / "HEngine.Runtime.json";
                    mPending["OidnAvailable"] = std::filesystem::is_regular_file(contractPath) &&
                        Utils::LoadJsonFile(contractPath).value("oidn", false);
                    bInitialized = true;
                }
                const bool available = mPending.value("OidnAvailable", false);
                bool enabled = mPending.value("OfflineReferenceRenderer", false);
                ImGui::BeginDisabled(!available || !ModeManager::IsEditState());
                const bool changed = ImGui::Checkbox(Text("Path Tracing / OIDN CPU denoising", "离线渲染 / OIDN CPU 降噪"), &enabled);
                ImGui::EndDisabled();
                if (changed)
                {
                    BinaryPluginSettings::SetOfflineRendererEnabled(enabled);
                    mPending["OfflineReferenceRenderer"] = enabled;
                }
                ImGui::TextWrapped("%s", Text("Applies after restarting this project's Editor. OIDN loads only when denoising is requested. It is excluded from exported games.",
                    "重启当前项目的编辑器后生效。仅在请求降噪时加载 OIDN，不进入导出的游戏。"));
                if (!available) ImGui::TextWrapped("%s", Text("This installation does not include the offline renderer.", "此安装未包含离线渲染插件。"));
                const bool active = ModuleManager::GetInstance().GetPluginManager().Contains("he.offline-reference-renderer");
                ImGui::Text("%s: %s", Text("Current process", "当前进程"), active ? Text("Enabled", "已启用") : Text("Disabled", "已关闭"));
                ImGui::TextWrapped("%s", Text("Other native plugin combinations require a matching engine build. Use Build Profiles to export games.",
                    "其他原生插件组合需要匹配的引擎构建。游戏导出使用发布配置。"));
            }
            catch (const std::exception& error) { ImGui::TextWrapped("%s", error.what()); }
            return;
        }
        try
        {
            if (bFinished.exchange(false))
            {
                if (mWorker.joinable()) mWorker.join();
                std::lock_guard lock(mMutex);
                mMessage = mResult.Output;
                // Keep invalid edits available for correction. Cancellation can race with publication.
                if (mResult.ExitCode == 0 || mResult.Cancelled) Reload();
            }
            if (!bInitialized) Reload();
            // File/process failures must not unwind through an open ImGui stack.
            const auto protect = [this](auto action) {
                try { action(); }
                catch (const std::exception& error) { mMessage = error.what(); }
            };
            ImGui::TextWrapped("%s", Text("C# is recommended for new gameplay. Python supports desktop tools and existing desktop scripts. Check your scene dependencies before disabling Python.",
                "新玩法推荐 C#。Python 保留用于桌面工具及已有脚本；关闭 Python 前请检查场景依赖。"));
            ImGui::TextWrapped("%s", Text("These switches change the next build. Currently loaded plugins stay active until restart. Save scenes, close the editor and rebuild affected targets after applying.",
                "开关仅影响下次构建。应用后请保存场景、退出编辑器，重建所需目标再启动；当前进程不会即时卸载插件。"));
            ImGui::BeginDisabled(bRunning || !ModeManager::IsEditState());
            const bool dirty = mPending != mOriginal;
            ImGui::BeginDisabled(dirty);
            if (ImGui::Combo(Text("Build profile", "构建配置"), &mProfile, Text("Desktop project\0Web / WeChat\0", "桌面项目\0Web / 微信\0"))) protect([this] { Reload(); });
            ImGui::EndDisabled();
            if (dirty) ImGui::TextUnformatted(Text("Unsaved changes. Apply or discard before switching profiles.", "有未保存改动。切换配置前请应用或放弃。"));
            ImGui::Text("Config/%s", c_Profiles[mProfile]);
            for (const auto& [id, settings] : mPending.items())
                if (std::none_of(mManifests.begin(), mManifests.end(), [&](const Json& item) { return item.at("Id") == id; }))
                    ImGui::TextWrapped("Missing plugin: %s. Configuration is preserved; restore the plugin before building.", id.c_str());
            auto& manager = ModuleManager::GetInstance().GetPluginManager();
            if (ImGui::BeginChild("PluginList", ImVec2(0, ImGuiWrapper::S(330)), true))
            {
                for (const auto& manifest : mManifests)
                {
                    const auto id = manifest.at("Id").get<std::string>();
                    ImGui::PushID(id.c_str());
                    if (ImGui::CollapsingHeader(manifest.value("DisplayName", id).c_str(), id.starts_with("he.script.") ? ImGuiTreeNodeFlags_DefaultOpen : 0))
                    {
                        ImGui::Text("%s | %s | API %s", id.c_str(), manifest.value("Version", "?").c_str(), manifest.value("ApiVersion", Json()).dump().c_str());
                        const auto status = manager.Contains(id) ? manager.GetStatus(id) : PluginStatus{};
                        ImGui::Text(Text("Current Editor: %s", "当前编辑器状态：%s"), manager.Contains(id) ? StateName(status.State) : Text("Not registered in this executable", "当前程序未注册"));
                        if (!status.Diagnostic.empty()) ImGui::TextWrapped("%s", status.Diagnostic.c_str());
                        ImGui::TextWrapped("Platforms: %s | Dependencies: %s", manifest.value("Platforms", Json::array()).dump().c_str(), manifest.value("Dependencies", Json::array()).dump().c_str());
                        const auto platforms = manifest.value("Platforms", Json::array());
                        for (const auto* host : c_Hosts)
                        {
                            if (mProfile == 1 && std::string_view(host) != "Game") continue;
                            const bool supported = Includes(manifest.value("SupportedHosts", Json::array()), host) &&
                                (mProfile == 0 ? Includes(platforms, "Windows") : Includes(platforms, "Web") && Includes(platforms, "WeChat"));
                            bool enabled = mPending.contains(id) && mPending[id].is_object() && mPending[id].value(host, false);
                            ImGui::BeginDisabled(!supported && !enabled);
                            if (ImGui::Checkbox(host, &enabled)) mPending[id][host] = enabled;
                            ImGui::EndDisabled();
                            ImGuiWrapper::SameLine(0);
                        }
                        ImGui::NewLine();
                        if (id == "he.script.python" && mProfile == 1) ImGui::TextUnformatted(Text("Python is unsupported on Web / WeChat; export rejects Python scenes.", "Python 不支持 Web / 微信；含 Python 的场景会被导出检查拒绝。"));
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            ImGui::BeginDisabled(!dirty || !bInitialized);
            if (ImGui::Button(Text("Validate and apply", "校验并应用"))) protect([this] { Apply(); });
            ImGuiWrapper::SameLine(0);
            if (ImGui::Button(Text("Discard changes", "放弃改动"))) protect([this] { Reload(); mMessage.clear(); });
            ImGui::EndDisabled();
            ImGuiWrapper::SameLine(0);
            if (ImGui::Button(Text("Reload from disk", "重新读取配置"))) protect([this] { Reload(); });
            ImGui::EndDisabled();
            if (bRunning) ImGui::TextUnformatted(Text("Validating dependencies and platform support...", "正在校验依赖及平台支持……"));
            if (!ModeManager::IsEditState()) ImGui::TextUnformatted(Text("Stop Play before changing build configuration.", "请先停止 Play，再修改构建配置。"));
            ImGui::TextWrapped("%s", Text("After applying: hectl editor check / hectl game check / hectl package. For Web / WeChat: hectl scripting build-wechat. Packaging validates reachable scene dependencies; disabling a plugin preserves its scene data.",
                "应用后按需运行：hectl editor check / hectl game check / hectl package。Web / 微信：hectl scripting build-wechat。发布时仍会检查场景依赖，关闭插件不会删除场景数据。"));
            if (!mMessage.empty()) ImGui::TextWrapped("%s", mMessage.c_str());
        }
        catch (const std::exception& error)
        {
            mMessage = error.what();
            ImGui::TextWrapped("Plugin settings error: %s", mMessage.c_str());
        }
    }
}
