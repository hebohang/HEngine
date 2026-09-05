#include "Hepch.h"

#include "Editor/Panels/ProjectSettingsPanel.h"

#include "Editor/ImGuiWrapper/ImGuiWrapper.h"
#include "Runtime/Graphics/Renderer/AntiAliasing.h"

#include <imgui/imgui.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <iterator>
#include <string_view>

namespace HEngine
{
    namespace
    {
        [[nodiscard]] const char* ChineseLabel(std::string_view id)
        {
            using namespace RenderSettingIds;
            if (id == Backend) return "渲染后端";
            if (id == VSync) return "垂直同步";
            if (id == RenderSettingIds::AntiAliasingMode) return "抗锯齿模式";
            if (id == MsaaSamples) return "MSAA 采样数";
            if (id == FxaaEdgeThreshold) return "FXAA 边缘阈值";
            if (id == FxaaEdgeThresholdMin) return "FXAA 最小边缘阈值";
            if (id == FxaaSubpixelQuality) return "FXAA 亚像素质量";
            if (id == SmaaEdgeThreshold) return "SMAA 边缘阈值";
            if (id == SmaaMaxSearchSteps) return "SMAA 最大搜索步数";
            if (id == SmaaMaxDiagonalSearchSteps) return "SMAA 对角搜索步数";
            if (id == SmaaCornerRounding) return "SMAA 转角圆滑";
            if (id == TaaHistoryWeight) return "TAA 历史权重";
            if (id == TaaSharpness) return "TAA 锐化";
            if (id == GiTechnique) return "全局光照技术";
            if (id == GiDenoiser) return "降噪器";
            if (id == GiUnsupportedFallback) return "不支持时回退";
            if (id == GiRenderScale) return "渲染比例";
            if (id == GiSamplesPerPixel) return "每像素采样数";
            if (id == GiMaxBounces) return "最大反弹次数";
            if (id == GiAtrousIterations) return "A-Trous 迭代次数";
            if (id == GiBilateralUpsample) return "双边上采样";
            if (id == GiSsgiRayCount) return "SSGI 射线数";
            if (id == GiSsgiStepCount) return "SSGI 步进次数";
            if (id == GiSsgiHiZStartMip) return "Hi-Z 起始层级";
            if (id == GiSsgiBinaryRefinementSteps) return "二分细化步数";
            if (id == GiSsgiMaximumRayDistance) return "最大射线距离";
            if (id == GiSsgiThickness) return "厚度";
            if (id == GiSsgiEdgeFade) return "边缘淡出";
            if (id == GiSsgiTemporalWeight) return "时间累积权重";
            if (id == PostProcessExposure) return "默认曝光";
            if (id == ShadowsEnabled) return "启用动态阴影";
            if (id == GiDebugView) return "GI 调试视图";
            if (id == GiDirectOnly) return "仅直接光照";
            if (id == PbrDebugMode) return "PBR 调试模式";
            if (id == RenderGraphValidation) return "RenderGraph 验证";
            if (id == ShadowCascadeDebug) return "阴影级联覆盖";
            if (id == CaptureSupersampleScale) return "捕获超采样倍率";
            return "渲染设置";
        }

        [[nodiscard]] const char* ChineseImpact(ApplyImpact impact)
        {
            switch (impact)
            {
            case ApplyImpact::Live: return "实时";
            case ApplyImpact::TemporalHistoryReset: return "重置历史";
            case ApplyImpact::RenderGraphRebuild: return "重建 RenderGraph";
            case ApplyImpact::RenderTargetRecreate: return "重建渲染目标";
            case ApplyImpact::SwapchainRecreate: return "重建交换链";
            case ApplyImpact::ProcessRestart: return "重启进程";
            case ApplyImpact::BuildReconfigure: return "重新配置构建";
            }
            return "未知";
        }

        [[nodiscard]] const char* ChineseLayer(SettingsLayer layer)
        {
            switch (layer)
            {
            case SettingsLayer::EmergencyFallback: return "安全兜底";
            case SettingsLayer::EngineDefault: return "引擎";
            case SettingsLayer::Project: return "项目";
            case SettingsLayer::User: return "用户";
            case SettingsLayer::Session: return "会话";
            case SettingsLayer::CommandLine: return "命令行";
            case SettingsLayer::World: return "世界";
            case SettingsLayer::Camera: return "相机";
            case SettingsLayer::Capability: return "能力解析";
            case SettingsLayer::Effective: return "运行时";
            }
            return "未知";
        }

        [[nodiscard]] const char* ChineseGroupLabel(std::string_view group)
        {
            if (group == "WindowAndDisplay") return "窗口与显示";
            if (group == "Engine.General") return "引擎常规";
            if (group == "Engine.Display") return "窗口与显示";
            if (group == "AntiAliasing") return "抗锯齿";
            if (group == "GlobalIllumination.Common") return "全局光照 · 通用";
            if (group == "GlobalIllumination.ScreenSpaceGI") return "全局光照 · 屏幕空间 GI";
            if (group == "GlobalIllumination.HardwareRTGI") return "全局光照 · 硬件光追 GI";
            if (group == "GlobalIllumination.Denoising") return "全局光照 · 降噪";
            if (group == "GlobalIllumination.Diagnostics") return "全局光照 · 诊断";
            if (group == "PostProcess") return "后期处理";
            if (group == "Shadows") return "阴影";
            if (group == "Diagnostics") return "渲染诊断";
            if (group == "Capture") return "捕获";
            if (group == "Engine") return "引擎";
            if (group == "Rendering") return "渲染";
            return "设置";
        }

        [[nodiscard]] std::string LowerAscii(std::string text)
        {
            std::ranges::transform(text, text.begin(), [](unsigned char value)
            {
                return static_cast<char>(std::tolower(value));
            });
            return text;
        }

        [[nodiscard]] RenderSettingAvailability AaAvailability(
            std::string_view mode,
            std::uint32_t samples,
            const AntiAliasingCapabilities& capabilities)
        {
            if (mode == "None")
                return { true, "关闭抗锯齿始终可用。" };
            const auto convert = [](const AntiAliasingAvailability& value)
            {
                return RenderSettingAvailability{
                    value.bAvailable, value.Reason };
            };
            if (mode == "FXAA") return convert(capabilities.Fxaa);
            if (mode == "SMAA") return convert(capabilities.Smaa);
            if (mode == "TAA") return convert(capabilities.Taa);
            if (samples == 2) return convert(capabilities.Msaa2x);
            if (samples == 4) return convert(capabilities.Msaa4x);
            if (samples == 8) return convert(capabilities.Msaa8x);
            return { false, "不支持该 MSAA 采样数。" };
        }

        [[nodiscard]] RenderSettingAvailability GiAvailability(
            std::string_view technique,
            const RenderSettingsCapabilities& capabilities)
        {
            if (technique == "Off") return { true, "关闭 GI 始终可用。" };
            if (technique == "ScreenSpaceGI")
                return { capabilities.bScreenSpaceGiAvailable,
                    capabilities.bScreenSpaceGiAvailable
                        ? "屏幕空间 GI 可用。" : "屏幕空间 GI 未注册。" };
            if (technique == "HardwareRTGI")
            {
                const bool bAvailable =
                    capabilities.Render.RayTracingAccelStruct.bEnabled &&
                    capabilities.Render.RayTracingPipeline.bEnabled;
                return { bAvailable, bAvailable
                    ? "硬件光追管线可用。"
                    : "需要硬件光追加速结构与光追管线。" };
            }
            return { capabilities.bExternalViewportRendererAvailable,
                capabilities.bExternalViewportRendererAvailable
                    ? "离线渲染插件可用。"
                    : "没有启用离线渲染插件。" };
        }

        [[nodiscard]] bool SupportsLayer(
            const RenderSettingDescriptor& descriptor,
            SettingsLayer layer)
        {
            return std::ranges::find(descriptor.WritableLayers, layer) !=
                descriptor.WritableLayers.end();
        }
    }

    ProjectSettingsPanel::ProjectSettingsPanel(
        EditorLayer* parent,
        std::string panelName,
        bool* pOpen)
        : BasePanel(parent, std::move(panelName), pOpen)
    {
    }

    void ProjectSettingsPanel::OnImGuiRender()
    {
        if (IsOpen())
        {
            ImGuiWrapper::SetNextWindowSize(
                ImVec2(1120.0f, 760.0f),
                ImGuiCond_Appearing);
        }
        BasePanel::OnImGuiRender();
    }

    void ProjectSettingsPanel::Refresh(bool bDiscardPending)
    {
        mSnapshot = RenderSettingsService::GetInstance().GetSnapshot();
        if (bDiscardPending)
            mPendingPatch = Json::object();
        bInitialized = true;
    }

    Json ProjectSettingsPanel::GetDisplayedValue(
        std::string_view propertyId) const
    {
        const std::string id(propertyId);
        if (mPendingPatch.contains(id) && !mPendingPatch[id].is_null())
            return mPendingPatch[id];
        Json values = RenderSettingsToFlatValues(mSnapshot.Requested);
        values.update(RenderDebugSettingsToFlatValues(mSnapshot.Debug));
        values.update(RenderCaptureSettingsToFlatValues(mSnapshot.Capture));
        return values.value(id, Json{});
    }

    bool ProjectSettingsPanel::MatchesFilter(
        const RenderSettingDescriptor& descriptor) const
    {
        if (!bShowAdvanced && descriptor.bAdvanced)
            return false;
        if (mSearch[0] != '\0')
        {
            const std::string needle = LowerAscii(mSearch.data());
            const std::string haystack = LowerAscii(
                descriptor.PropertyId + " " + descriptor.EnglishFallback + " " +
                ChineseLabel(descriptor.PropertyId));
            return haystack.find(needle) != std::string::npos;
        }
        return IsDescriptorOnSelectedPage(descriptor);
    }

    bool ProjectSettingsPanel::IsDescriptorOnSelectedPage(
        const RenderSettingDescriptor& descriptor) const
    {
        const std::string_view id = descriptor.PropertyId;
        if (mSelectedPage == "All")
            return true;
        if (mSelectedPage == "Engine.General")
            return id == RenderSettingIds::Backend;
        if (mSelectedPage == "Engine.Display")
            return id == RenderSettingIds::VSync;
        if (mSelectedPage == "Rendering.AntiAliasing")
            return id.starts_with("Rendering.AntiAliasing.");
        if (mSelectedPage == "Rendering.GI.General")
        {
            return id == RenderSettingIds::GiTechnique ||
                id == RenderSettingIds::GiUnsupportedFallback ||
                id == RenderSettingIds::GiRenderScale;
        }
        if (mSelectedPage == "Rendering.GI.ScreenSpace")
            return id.starts_with("Rendering.GI.SSGI.");
        if (mSelectedPage == "Rendering.GI.Hardware")
        {
            return id == RenderSettingIds::GiSamplesPerPixel ||
                id == RenderSettingIds::GiMaxBounces;
        }
        if (mSelectedPage == "Rendering.GI.Denoising")
        {
            return id == RenderSettingIds::GiDenoiser ||
                id == RenderSettingIds::GiAtrousIterations ||
                id == RenderSettingIds::GiBilateralUpsample;
        }
        if (mSelectedPage == "Rendering.PostProcess")
            return id.starts_with("Rendering.PostProcess.");
        if (mSelectedPage == "Rendering.Shadows")
            return id.starts_with("Rendering.Shadows.");
        if (mSelectedPage == "Rendering.Diagnostics")
        {
            return !descriptor.bPersistedRenderSetting ||
                id.starts_with("Rendering.Debug.") ||
                id.starts_with("Rendering.Capture.");
        }
        return false;
    }

    const char* ProjectSettingsPanel::GetSelectedPageTitle() const
    {
        if (mSearch[0] != '\0') return "搜索结果";
        if (mSelectedPage == "All") return "全部渲染设置";
        if (mSelectedPage == "Engine.General") return "引擎 / 常规";
        if (mSelectedPage == "Engine.Display") return "引擎 / 窗口与显示";
        if (mSelectedPage == "Rendering.AntiAliasing") return "渲染 / 抗锯齿";
        if (mSelectedPage == "Rendering.GI.General") return "渲染 / 全局光照 / 通用";
        if (mSelectedPage == "Rendering.GI.ScreenSpace") return "渲染 / 全局光照 / 屏幕空间 GI";
        if (mSelectedPage == "Rendering.GI.Hardware") return "渲染 / 全局光照 / 硬件光追 GI";
        if (mSelectedPage == "Rendering.GI.Denoising") return "渲染 / 全局光照 / 降噪";
        if (mSelectedPage == "Rendering.PostProcess") return "渲染 / 后期处理";
        if (mSelectedPage == "Rendering.Shadows") return "渲染 / 阴影";
        if (mSelectedPage == "Rendering.Diagnostics") return "渲染 / 诊断与捕获";
        return "项目设置";
    }

    const char* ProjectSettingsPanel::GetSelectedPageDescription() const
    {
        if (mSearch[0] != '\0')
            return "搜索会跨越全部分类，但仍遵循基础/高级过滤。";
        if (mSelectedPage == "All")
            return "查看全部 requested 项目请求、运行时 effective 值和来源层。";
        if (mSelectedPage == "Engine.General")
            return "选择项目请求的渲染后端；不可用请求会保留并由能力解析器回退。";
        if (mSelectedPage == "Engine.Display")
            return "控制窗口呈现行为；命令行覆盖会显示为更高优先级来源。";
        if (mSelectedPage == "Rendering.AntiAliasing")
            return "配置实时抗锯齿模式和各算法参数，不支持的选项会提前禁用。";
        if (mSelectedPage == "Rendering.GI.General")
            return "选择全局光照技术、回退策略和内部渲染比例。";
        if (mSelectedPage == "Rendering.GI.ScreenSpace")
            return "配置屏幕空间追踪步进、厚度、距离和时间累积。";
        if (mSelectedPage == "Rendering.GI.Hardware")
            return "配置硬件光追 GI 的采样与反弹预算。";
        if (mSelectedPage == "Rendering.GI.Denoising")
            return "配置可用降噪器、空间过滤次数和上采样方式。";
        if (mSelectedPage == "Rendering.PostProcess")
            return "设置场景没有 Volume/Camera 覆盖时使用的项目默认后处理值。";
        if (mSelectedPage == "Rendering.Shadows")
            return "设置项目级动态阴影总开关。";
        if (mSelectedPage == "Rendering.Diagnostics")
            return "仅 User/Session 可写的调试与捕获工具，不会污染项目渲染配置。";
        return "";
    }

    void ProjectSettingsPanel::DrawNavigation()
    {
        ImGui::TextUnformatted("项目设置");
        ImGui::TextDisabled("分类与搜索");
        ImGui::Separator();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint(
            "##ProjectSettingsSearch",
            "搜索设置…",
            mSearch.data(),
            mSearch.size());
        ImGui::Dummy(ImGuiWrapper::S(0.0f, 8.0f));

        const auto selectable = [this](const char* label, const char* page)
        {
            ImGui::PushID(page);
            if (ImGui::Selectable(label, mSelectedPage == page))
                mSelectedPage = page;
            ImGui::PopID();
        };
        ImGui::TextDisabled("引擎");
        selectable("  常规", "Engine.General");
        selectable("  窗口与显示", "Engine.Display");
        ImGui::Dummy(ImGuiWrapper::S(0.0f, 6.0f));
        ImGui::TextDisabled("渲染");
        selectable("  抗锯齿", "Rendering.AntiAliasing");
        if (ImGui::TreeNodeEx("GlobalIlluminationSettingsTree",
                ImGuiTreeNodeFlags_DefaultOpen |
                    ImGuiTreeNodeFlags_SpanAvailWidth,
                "  全局光照"))
        {
            selectable("    通用", "Rendering.GI.General");
            selectable("    屏幕空间 GI", "Rendering.GI.ScreenSpace");
            selectable("    硬件光追 GI", "Rendering.GI.Hardware");
            selectable("    降噪", "Rendering.GI.Denoising");
            ImGui::TreePop();
        }
        selectable("  后期处理", "Rendering.PostProcess");
        selectable("  阴影", "Rendering.Shadows");
        selectable("  诊断与捕获", "Rendering.Diagnostics");
        ImGui::Dummy(ImGuiWrapper::S(0.0f, 6.0f));
        selectable("全部设置", "All");

        ImGui::Dummy(ImGuiWrapper::S(0.0f, 12.0f));
        ImGui::Separator();
        ImGui::TextDisabled("当前快照");
        ImGui::Text("revision %llu",
            static_cast<unsigned long long>(mSnapshot.Revision));
        ImGui::TextDisabled("%s", mSnapshot.Hash.c_str());
    }

    void ProjectSettingsPanel::DrawSetting(
        const RenderSettingDescriptor& descriptor)
    {
        const std::string& id = descriptor.PropertyId;
        Json value = GetDisplayedValue(id);
        const Json effectiveValues =
            RenderSettingsToFlatValues(mSnapshot.Effective);
        const Json effective = effectiveValues.value(id, value);
        const bool bPendingReset = mPendingPatch.contains(id) &&
            mPendingPatch[id].is_null();
        bool bChanged = false;

        ImGui::PushID(id.c_str());
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(ChineseLabel(id));
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s\n%s\nTextId: %s",
                descriptor.EnglishFallback.c_str(), id.c_str(),
                descriptor.DescriptionTextId.c_str());
        }

        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (bPendingReset)
        {
            ImGui::TextDisabled("应用后恢复继承");
        }
        else if (descriptor.Type == RenderSettingValueType::Boolean)
        {
            bool current = value.is_boolean() && value.get<bool>();
            bChanged = ImGui::Checkbox("##Value", &current);
            value = current;
        }
        else if (descriptor.Type == RenderSettingValueType::Integer)
        {
            int current = value.is_number_integer() ? value.get<int>() : 0;
            if (id == RenderSettingIds::MsaaSamples ||
                id == RenderSettingIds::CaptureSupersampleScale)
            {
                const int options[] = { 1, 2, 4, 8 };
                if (ImGui::BeginCombo("##Value", std::to_string(current).c_str()))
                {
                    for (int option : options)
                    {
                        if (id == RenderSettingIds::MsaaSamples && option == 1)
                            continue;
                        if (id == RenderSettingIds::CaptureSupersampleScale &&
                            option == 8)
                            continue;
                        if (ImGui::Selectable(std::to_string(option).c_str(),
                                option == current))
                        {
                            current = option;
                            bChanged = true;
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            else
            {
                const int minimum = descriptor.Minimum
                    ? static_cast<int>(*descriptor.Minimum) : 0;
                const int maximum = descriptor.Maximum
                    ? static_cast<int>(*descriptor.Maximum) : 100;
                bChanged = ImGui::SliderInt("##Value", &current,
                    minimum, maximum);
            }
            value = current;
        }
        else if (descriptor.Type == RenderSettingValueType::Float)
        {
            float current = value.is_number() ? value.get<float>() : 0.0f;
            const float minimum = descriptor.Minimum
                ? static_cast<float>(*descriptor.Minimum) : 0.0f;
            const float maximum = descriptor.Maximum
                ? static_cast<float>(*descriptor.Maximum) : 1.0f;
            bChanged = ImGui::SliderFloat("##Value", &current,
                minimum, maximum, "%.4g");
            value = current;
        }
        else if (descriptor.Type == RenderSettingValueType::Enumeration)
        {
            std::string current = value.is_string()
                ? value.get<std::string>() : std::string{};
            if (ImGui::BeginCombo("##Value", current.c_str()))
            {
                for (const std::string& option : descriptor.EnumValues)
                {
                    RenderSettingAvailability availability;
                    if (id == RenderSettingIds::AntiAliasingMode)
                    {
                        const Json samples = GetDisplayedValue(
                            RenderSettingIds::MsaaSamples);
                        availability = AaAvailability(option,
                            samples.is_number_integer()
                                ? samples.get<std::uint32_t>() : 4u,
                            mSnapshot.Capabilities.AntiAliasing);
                    }
                    else if (id == RenderSettingIds::GiTechnique)
                        availability = GiAvailability(option,
                            mSnapshot.Capabilities);
                    else if (id == RenderSettingIds::Backend)
                        availability = GetRenderBackendAvailability(
                            ParseRenderBackend(option), mSnapshot.Capabilities);
                    ImGui::BeginDisabled(!availability.bAvailable);
                    if (ImGui::Selectable(option.c_str(), option == current) &&
                        availability.bAvailable)
                    {
                        current = option;
                        bChanged = true;
                    }
                    if (!availability.bAvailable && ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", availability.Reason.c_str());
                    ImGui::EndDisabled();
                }
                ImGui::EndCombo();
            }
            value = current;
        }

        if (bChanged)
            mPendingPatch[id] = value;

        ImGui::TableSetColumnIndex(2);
        const SettingsLayer source = mSnapshot.Sources.contains(id)
            ? mSnapshot.Sources.at(id) : SettingsLayer::EmergencyFallback;
        ImGui::TextDisabled("%s", ChineseLayer(source));
        if (!bPendingReset && effective != value)
        {
            ImGuiWrapper::SameLine(0.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.2f, 1.0f),
                "实际: %s", effective.dump().c_str());
            const auto effectiveSource = mSnapshot.EffectiveSources.find(id);
            if (effectiveSource != mSnapshot.EffectiveSources.end())
                ImGui::TextDisabled("实际来源：%s",
                    ChineseLayer(effectiveSource->second));
        }
        const auto availability = mSnapshot.Availability.find(id);
        if (availability != mSnapshot.Availability.end() &&
            !availability->second.bAvailable)
        {
            ImGui::TextWrapped("回退：%s",
                availability->second.Reason.c_str());
        }

        ImGui::TableSetColumnIndex(3);
        ImGui::TextDisabled("%s", ChineseImpact(descriptor.Impact));
        if (descriptor.bRestartRequired)
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f), "需要重启");
        ImGuiWrapper::SameLine(0.0f);
        if (ImGui::SmallButton("恢复继承"))
            mPendingPatch[id] = nullptr;
        ImGui::PopID();
    }

    void ProjectSettingsPanel::QueueCurrentPageReset()
    {
        const Json values = RenderSettingsService::GetInstance()
            .GetLayerValues(mTargetLayer);
        for (const auto& [id, value] : values.items())
        {
            (void)value;
            const RenderSettingDescriptor* descriptor =
                FindRenderSettingDescriptor(id);
            const bool bVisibleScope = descriptor &&
                (mSearch[0] != '\0'
                    ? MatchesFilter(*descriptor)
                    : IsDescriptorOnSelectedPage(*descriptor));
            if (bVisibleScope)
                mPendingPatch[id] = nullptr;
        }
    }

    void ProjectSettingsPanel::DrawSettings()
    {
        ImGui::TextUnformatted(GetSelectedPageTitle());
        ImGui::TextWrapped("%s", GetSelectedPageDescription());
        ImGui::Dummy(ImGuiWrapper::S(0.0f, 4.0f));
        ImGui::Checkbox("高级", &bShowAdvanced);
        ImGuiWrapper::SameLine(0.0f);
        if (ImGui::Button(mSearch[0] != '\0'
                ? "恢复搜索结果的继承"
                : "恢复本页继承"))
        {
            QueueCurrentPageReset();
        }

        if (mSearch[0] == '\0' &&
            mSelectedPage.starts_with("Rendering.GI"))
        {
            std::vector<RenderSettingDiagnostic> previewDiagnostics;
            Json renderPatch = Json::object();
            for (const auto& [id, value] : mPendingPatch.items())
            {
                const RenderSettingDescriptor* descriptor =
                    FindRenderSettingDescriptor(id);
                if (descriptor && descriptor->bPersistedRenderSetting &&
                    !value.is_null())
                    renderPatch[id] = value;
            }
            RenderSettings preview = ApplyRenderSettingsFlatValues(
                mSnapshot.Requested, renderPatch,
                previewDiagnostics);
            const GlobalIlluminationQualityPreset preset =
                DetectGlobalIlluminationQualityPreset(preview);
            ImGuiWrapper::SameLine(0.0f);
            ImGui::TextDisabled("质量预设：%s", ToString(preset));
            for (GlobalIlluminationQualityPreset option : {
                     GlobalIlluminationQualityPreset::Low,
                     GlobalIlluminationQualityPreset::Balanced,
                     GlobalIlluminationQualityPreset::High })
            {
                ImGuiWrapper::SameLine(0.0f);
                if (ImGui::Button(ToString(option)))
                    mPendingPatch.update(
                        GetGlobalIlluminationQualityPresetPatch(option));
            }
        }

        if (mSearch[0] == '\0' && mSelectedPage == "Engine.General")
        {
            ImGui::Dummy(ImGuiWrapper::S(0.0f, 8.0f));
            ImGui::TextUnformatted("当前运行环境");
            ImGui::Separator();
            if (ImGui::BeginTable("##RuntimeCapabilitySummary", 2,
                    ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_BordersInnerH |
                    ImGuiTableFlags_SizingStretchProp))
            {
                const auto summaryRow = [](const char* label,
                    const std::string& value)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextDisabled("%s", label);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(value.c_str());
                };
                summaryRow("显示适配器",
                    mSnapshot.Capabilities.Render.AdapterName.empty()
                        ? std::string("尚未探测")
                        : mSnapshot.Capabilities.Render.AdapterName);
                summaryRow("Effective 后端",
                    std::string(ToString(mSnapshot.Effective.Backend)));
                summaryRow("硬件光追",
                    mSnapshot.Capabilities.Render.bHardwareRayTracingRendererReady
                        ? std::string("可用") : std::string("不可用"));
                summaryRow("设置 Schema",
                    std::string(RenderSettingsSchema) + " v" +
                        std::to_string(RenderSettingsSchemaVersion));
                ImGui::EndTable();
            }
        }

        std::vector<std::pair<std::string,
            std::vector<const RenderSettingDescriptor*>>> groups;
        for (const RenderSettingDescriptor& descriptor :
            GetRenderSettingDescriptors())
        {
            if (!MatchesFilter(descriptor))
                continue;
            std::string groupName = descriptor.Subcategory.empty()
                ? descriptor.Category : descriptor.Subcategory;
            if (mSearch[0] == '\0' && mSelectedPage == "Engine.General")
                groupName = "Engine.General";
            else if (mSearch[0] == '\0' && mSelectedPage == "Engine.Display")
                groupName = "Engine.Display";
            auto iterator = std::ranges::find_if(groups,
                [&groupName](const auto& group)
                {
                    return group.first == groupName;
                });
            if (iterator == groups.end())
            {
                groups.emplace_back(groupName,
                    std::vector<const RenderSettingDescriptor*>{});
                iterator = std::prev(groups.end());
            }
            iterator->second.push_back(&descriptor);
        }

        ImGui::Separator();
        ImGui::TextDisabled("%zu 项设置 · requested 始终保留项目请求",
            [&groups]()
            {
                std::size_t count = 0;
                for (const auto& [name, descriptors] : groups)
                {
                    (void)name;
                    count += descriptors.size();
                }
                return count;
            }());

        if (groups.empty())
        {
            ImGui::Dummy(ImGuiWrapper::S(0.0f, 24.0f));
            ImGui::TextDisabled("当前分类和过滤条件下没有可显示的设置。");
            return;
        }

        for (const auto& [groupName, descriptors] : groups)
        {
            ImGui::PushID(groupName.c_str());
            ImGui::Dummy(ImGuiWrapper::S(0.0f, 8.0f));
            ImGui::TextUnformatted(ChineseGroupLabel(groupName));
            ImGui::Separator();
            if (ImGui::BeginTable("##SettingsGroup", 4,
                    ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_BordersInnerH |
                    ImGuiTableFlags_Resizable |
                    ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("设置",
                    ImGuiTableColumnFlags_WidthFixed,
                    ImGuiWrapper::S(180.0f));
                ImGui::TableSetupColumn("Requested",
                    ImGuiTableColumnFlags_WidthStretch, 1.4f);
                ImGui::TableSetupColumn("来源 / Effective",
                    ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("应用影响",
                    ImGuiTableColumnFlags_WidthFixed,
                    ImGuiWrapper::S(172.0f));
                ImGui::TableHeadersRow();
                for (const RenderSettingDescriptor* descriptor : descriptors)
                    DrawSetting(*descriptor);
                ImGui::EndTable();
            }
            ImGui::PopID();
        }

        DrawDiagnostics();
    }

    void ProjectSettingsPanel::ApplyPending(
        SettingsLayer layer,
        bool bPersist)
    {
        RenderSettingsDraft draft{
            .TargetLayer = layer,
            .BaseRevision = mSnapshot.Revision,
            .BaseHash = mSnapshot.Hash,
            .Patch = Json::object(),
        };
        for (const auto& [id, value] : mPendingPatch.items())
        {
            const RenderSettingDescriptor* descriptor =
                FindRenderSettingDescriptor(id);
            if (descriptor && SupportsLayer(*descriptor, layer))
                draft.Patch[id] = value;
        }
        const RenderSettingsApplyResult result =
            RenderSettingsService::GetInstance().ApplyDraft(draft, bPersist);
        mLastMessage = result.bSuccess
            ? (result.ChangePlan.bProcessRestartRequired
                ? "设置已保存；图形 API 将在重启 Editor/Game 后切换。"
                : "设置已作为一个事务应用。")
            : "应用失败：" + result.ErrorCode + " · " + result.Message;
        Refresh(result.bSuccess);
    }

    void ProjectSettingsPanel::DrawDiagnostics()
    {
        if (!mSnapshot.Diagnostics.empty() &&
            ImGui::CollapsingHeader("规范化、回退与加载诊断"))
        {
            for (const RenderSettingDiagnostic& diagnostic :
                mSnapshot.Diagnostics)
            {
                const ImVec4 color = diagnostic.Severity ==
                        RenderSettingDiagnosticSeverity::Error
                    ? ImVec4(1.0f, 0.35f, 0.25f, 1.0f)
                    : diagnostic.Severity ==
                            RenderSettingDiagnosticSeverity::Warning
                        ? ImVec4(1.0f, 0.72f, 0.2f, 1.0f)
                        : ImVec4(0.55f, 0.75f, 1.0f, 1.0f);
                ImGui::TextColored(color, "%s · %s",
                    diagnostic.PropertyId.c_str(), diagnostic.Code.c_str());
                ImGui::TextWrapped("%s", diagnostic.Message.c_str());
                if (!diagnostic.InputValue.is_null())
                {
                    ImGui::TextDisabled("输入 %s → 规范化 %s",
                        diagnostic.InputValue.dump().c_str(),
                        diagnostic.NormalizedValue.dump().c_str());
                }
            }
        }
    }

    void ProjectSettingsPanel::DrawFooter()
    {
        ImGui::Separator();
        const char* layerNames[] = { "项目", "我的设置", "会话" };
        int target = mTargetLayer == SettingsLayer::Project ? 0
            : mTargetLayer == SettingsLayer::User ? 1 : 2;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("编辑层");
        ImGuiWrapper::SameLine(0.0f);
        ImGui::SetNextItemWidth(ImGuiWrapper::S(112.0f));
        if (ImGui::Combo("##TargetSettingsLayer", &target, layerNames,
                static_cast<int>(std::size(layerNames))))
        {
            mTargetLayer = target == 0 ? SettingsLayer::Project
                : target == 1 ? SettingsLayer::User : SettingsLayer::Session;
        }
        ImGuiWrapper::SameLine(0.0f);
        if (ImGui::Button("应用到当前层"))
            ApplyPending(mTargetLayer, mTargetLayer != SettingsLayer::Session);
        ImGuiWrapper::SameLine(0.0f);
        if (ImGui::Button("放弃草稿"))
            Refresh(true);
        ImGuiWrapper::SameLine(0.0f);
        if (ImGui::Button("重新加载当前值"))
            Refresh(true);

        if (ImGui::Button("保存到项目"))
            ApplyPending(SettingsLayer::Project, true);
        ImGuiWrapper::SameLine(0.0f);
        if (ImGui::Button("保存到我的设置"))
            ApplyPending(SettingsLayer::User, true);
        ImGuiWrapper::SameLine(0.0f);
        if (ImGui::Button("仅在本次会话试用"))
            ApplyPending(SettingsLayer::Session, false);
        ImGuiWrapper::SameLine(0.0f);
        if (ImGui::Button("全部恢复 Engine Default"))
        {
            const RenderSettingsApplyResult result =
                RenderSettingsService::GetInstance().ResetToEngineDefault(
                    mSnapshot.Revision, mSnapshot.Hash);
            mLastMessage = result.bSuccess
                ? "所有可写层已清除，正在继承 Engine Default。"
                : "恢复失败：" + result.Message;
            Refresh(result.bSuccess);
        }

        if (!mLastMessage.empty())
            ImGui::TextWrapped("%s", mLastMessage.c_str());
        else if (!mPendingPatch.empty())
            ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f),
                "有 %zu 个待应用字段。", mPendingPatch.size());
        else
            ImGui::TextDisabled("没有待应用修改。");
    }

    void ProjectSettingsPanel::OnImGuiRenderImpl()
    {
        if (!bInitialized)
            Refresh(true);
        if (ImGui::BeginTable("##ProjectSettingsLayout", 2,
                ImGuiTableFlags_Resizable |
                ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_SizingFixedFit,
                ImGui::GetContentRegionAvail()))
        {
            ImGui::TableSetupColumn("##ProjectSettingsNavigation",
                ImGuiTableColumnFlags_WidthFixed,
                ImGuiWrapper::S(236.0f));
            ImGui::TableSetupColumn("##ProjectSettingsPage",
                ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::BeginChild("##ProjectSettingsNavigationChild",
                ImVec2(0.0f, 0.0f), false);
            DrawNavigation();
            ImGui::EndChild();

            ImGui::TableSetColumnIndex(1);
            ImGui::BeginChild("##ProjectSettingsContentChild",
                ImVec2(0.0f, -ImGuiWrapper::S(116.0f)), false);
            DrawSettings();
            ImGui::EndChild();
            DrawFooter();
            ImGui::EndTable();
        }
    }
}
