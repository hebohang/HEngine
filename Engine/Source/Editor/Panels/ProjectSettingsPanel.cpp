#include "Hepch.h"

#include "Editor/Panels/ProjectSettingsPanel.h"

#include "Editor/ImGuiWrapper/ImGuiWrapper.h"
#include "Runtime/Graphics/Renderer/AntiAliasing.h"
#include "Runtime/Graphics/Settings/PipelineProfile.h"
#include "Runtime/Graphics/Renderer/RenderFeature.h"
#include "Runtime/Graphics/Renderer/RenderPipelineRegistry.h"
#include "Editor/EditorLayer.h"
#include "Editor/Panels/OfflineRendererPanel.h"
#include "Runtime/Core/AppFramework/Application.h"
#include "Runtime/Core/Input/InputActions.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/ModeManager/ModeManager.h"
#include "Runtime/AI/Navigation/NavigationService.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "Editor/Panels/SceneHierarchyPanel.h"
#if HE_ENABLE_NAVIGATION
#include "Runtime/EcsFramework/System/Navigation/NavPathDebugSystem.h"
#endif
#include "_generated/serializer/NavigationSettings.gen.h"

#include <imgui/imgui.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
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
            if (id == Path) return "渲染路径";
            if (id == FramesInFlight) return "并行帧数（需重启）";
            if (id == GpuOcclusionEnabled) return "GPU 遮挡剔除";
            if (id == GpuBatchingEnabled) return "GPU 绘制合批";
            if (id == GpuShadowBatchingEnabled) return "GPU 阴影合批";
            if (id == TransientCacheBudgetMiB) return "临时资源缓存预算（MiB）";
            if (id == TransientTextureReuse) return "复用临时纹理内存";
            if (id == LodEnabled) return "启用网格细节层级（LOD）";
            if (id == LodMaxPixelError) return "LOD 最大像素误差";
            if (id == AoEnabled) return "启用环境光遮蔽（AO）";
            if (id == AoTechnique) return "环境光遮蔽算法";
            if (id == AoNormalBias) return "AO 法线偏移";
            if (id == AoSampleCount) return "SSAO 采样数";
            if (id == RenderSettingIds::CloudQuality) return "体积云画质";
            if (id == RenderSettingIds::CloudRenderScale) return "云分辨率比例（每维）";
            if (id == RenderSettingIds::CloudMaxSteps) return "主射线最大步数";
            if (id == RenderSettingIds::CloudLightSteps) return "光照采样上限";
            if (id == RenderSettingIds::CloudHistoryWeight) return "历史权重";
            if (id == RenderSettingIds::CloudDetailDistance) return "细节衰减距离（m）";
            if (id == RenderSettingIds::CloudTemporal) return "独立时域重建";
            if (id == RenderSettingIds::CloudDebugView) return "云调试视图";
            if (id == AoFastUpsample) return "AO 快速上采样";
            if (id == AoRadius) return "AO 作用半径";
            if (id == AoIntensity) return "AO 强度";
            if (id == AoSliceCount) return "GTAO 切片数";
            if (id == AoStepsPerSide) return "GTAO 单侧步进数";
            if (id == SsrEnabled) return "启用屏幕空间反射（SSR）";
            if (id == SsrGuideCache) return "SSR 引导数据缓存";
            if (id == ReflectionProbesEnabled) return "启用局部反射探针";
            if (id == SsrMaximumDistance) return "SSR 最大追踪距离";
            if (id == SsrThickness) return "SSR 表面厚度";
            if (id == SsrRoughnessCutoff) return "SSR 粗糙度上限";
            if (id == SsrEdgeFade) return "SSR 屏幕边缘淡出";
            if (id == SsrNormalBias) return "SSR 法线偏移";
            if (id == SsrStepCount) return "SSR 步进次数";
            if (id == SsrBinaryRefinementSteps) return "SSR 二分细化步数";
            if (id == TaaOptimizedHistoryGather) return "TAA 历史采样优化";
            if (id == GiSvgfOptimizedKernels) return "SVGF 降噪内核优化";
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
            if (id == GiSsgiBinaryRefinementSteps) return "二分细化步数（兼容项，解析求交不使用）";
            if (id == GiSsgiMaximumRayDistance) return "最大射线距离";
            if (id == GiSsgiThickness) return "厚度";
            if (id == GiSsgiEdgeFade) return "边缘淡出";
            if (id == GiSsgiTemporalWeight) return "时间累积权重";
            if (id == PostProcessExposure) return "默认曝光（EV100）";
            if (id == BloomIntensity) return "泛光强度（Bloom）";
            if (id == BloomThreshold) return "泛光亮度阈值";
            if (id == AutoExposure) return "启用自动曝光";
            if (id == ExposureKey) return "自动曝光目标灰度";
            if (id == MinimumExposure) return "自动曝光最小 EV100";
            if (id == MaximumExposure) return "自动曝光最大 EV100";
            if (id == ExposureAdaptationSpeed) return "自动曝光适应速度";
            if (id == ColorContrast) return "对比度";
            if (id == ColorSaturation) return "饱和度";
            if (id == ColorTemperature) return "冷暖色调";
            if (id == ShadowsEnabled) return "启用动态阴影";
            if (id == GiDebugView) return "GI 调试视图";
            if (id == GiDirectOnly) return "仅直接光照";
            if (id == PbrDebugMode) return "PBR 调试模式";
            if (id == RenderGraphValidation) return "RenderGraph 验证";
            if (id == ShadowCascadeDebug) return "阴影级联覆盖";
            if (id == CaptureSupersampleScale) return "捕获超采样倍率";
            return nullptr;
        }

        [[nodiscard]] const char* SettingLabel(const RenderSettingDescriptor& descriptor)
        {
            if (const char* label = ChineseLabel(descriptor.PropertyId))
                return label;
            return descriptor.EnglishFallback.empty()
                ? descriptor.PropertyId.c_str() : descriptor.EnglishFallback.c_str();
        }

        [[nodiscard]] const char* OptionLabel(const std::string& option)
        {
            if (option == "Off" || option == "None") return "关闭";
            if (option == "ScreenSpaceGI") return "SSGI（屏幕空间）";
            if (option == "HardwareRTGI") return "硬件光追全局光照";
            if (option == "ExternalViewportRenderer") return "外部视口渲染器";
            if (option == "Forward") return "前向渲染（Forward）";
            if (option == "Deferred") return "延迟渲染（Deferred）";
            if (option == "Low") return "低";
            if (option == "Balanced") return "均衡";
            if (option == "High") return "高";
            if (option == "Custom") return "自定义";
            return option.c_str();
        }

        [[nodiscard]] std::string DisplayValue(const Json& value)
        {
            if (value.is_boolean()) return value.get<bool>() ? "已开启" : "已关闭";
            if (value.is_string()) return OptionLabel(value.get<std::string>());
            if (value.is_number_float())
            {
                char text[48];
                std::snprintf(text, sizeof(text), "%.4g", value.get<double>());
                return text;
            }
            return value.dump();
        }

        [[nodiscard]] const char* SettingHelp(std::string_view id)
        {
            if (id == RenderSettingIds::TransientCacheBudgetMiB)
                return "0 保持兼容的不限制策略。非零预算在 GPU 完成后回收闲置缓存；不会释放在途资源，也不是显卡总显存上限。";
            if (id == RenderSettingIds::CloudQuality) return "预设覆盖分辨率与采样预算；场景还需启用环境组件中的云。";
            if (id == RenderSettingIds::CloudRenderScale) return "仅自定义生效。0.5 表示宽高各一半、像素数四分之一；不改变游戏分辨率。";
            if (id == RenderSettingIds::CloudMaxSteps) return "仅自定义生效，16 至 96 步；透射按实际步长积分。";
            if (id == RenderSettingIds::CloudLightSteps) return "仅自定义生效，每个有效密度点最多 0 至 6 次光照采样。";
            if (id == RenderSettingIds::CloudHistoryWeight) return "0 至 0.95，配合深度拒绝与邻域约束。增大可能造成拖尾。";
            if (id == RenderSettingIds::CloudDetailDistance) return "远距离逐渐关闭细节侵蚀，单位为世界米。";
            if (id == RenderSettingIds::CloudTemporal) return "独立于整屏抗锯齿；关闭时显示本帧积分。";
            if (id == RenderSettingIds::CloudDebugView) return "0 正常合成；1 不透明度；2 散射；3 低分辨率网格。";

            using namespace RenderSettingIds;
            if (id == GiTechnique) return "在这里选择 SSGI 即可启用屏幕空间间接光照；修改后点击底部的应用按钮。";
            if (id == GiRenderScale) return "GI 内部计算分辨率比例；较低更省性能，较高细节更清晰。";
            if (id == GiSsgiRayCount) return "每像素追踪的射线数量；增加可减少噪点，但会提高开销。";
            if (id == PostProcessExposure) return "场景或相机没有覆盖时使用的 EV100；增大 1 档使画面亮度减半，晴天室外可从 14 开始。";
            if (id == BloomIntensity) return "亮部向周围扩散的强度；设为 0 关闭泛光，大于 0 开启。";
            if (id == BloomThreshold) return "仅超过该亮度的区域产生泛光；降低阈值会扩大泛光范围。";
            if (id == AutoExposure) return "根据画面亮度自动调整曝光；下方参数控制目标亮度、范围和速度。";
            if (id == ExposureKey) return "自动曝光的目标中灰亮度；增大后画面整体更亮。";
            if (id == MinimumExposure) return "自动曝光允许的最低 EV100，对应最亮的曝光，应小于或等于最大 EV100。";
            if (id == MaximumExposure) return "自动曝光允许的最高 EV100，对应最暗的曝光，应大于或等于最小 EV100。";
            if (id == ExposureAdaptationSpeed) return "每秒适应亮度变化的速率；数值越大，明暗切换越快。";
            if (id == ColorContrast) return "1 为原始对比度；增大可加强明暗差异。";
            if (id == ColorSaturation) return "1 为原始色彩；0 为黑白，增大让颜色更鲜艳。";
            if (id == ColorTemperature) return "0 为中性；负值偏冷，正值偏暖。";
            return nullptr;
        }

        // Measure in final pixels so wrapped action rows also follow font/DPI changes.
        void SameLineIfFits(const char* label)
        {
            const float width = ImGui::CalcTextSize(label).x +
                ImGui::GetStyle().FramePadding.x * 2.0f;
            const float right = ImGui::GetItemRectMax().x +
                ImGui::GetStyle().ItemSpacing.x + width;
            if (right <= ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x)
                ImGuiWrapper::SameLine(0.0f);
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
            if (group == "PostProcess.Bloom") return "泛光（Bloom）";
            if (group == "PostProcess.AutoExposure") return "自动曝光";
            if (group == "PostProcess.Color") return "色彩调整";
            if (group == "Pipeline") return "渲染管线";
            if (group == "Visibility") return "可见性与绘制优化";
            if (group == "Memory") return "显存优化";
            if (group == "Presentation") return "画面呈现";
            if (group == "Clouds") return "体积云";
            if (group == "AmbientOcclusion") return "环境光遮蔽（AO）";
            if (group == "Reflections") return "反射（SSR / 探针）";
            if (group == "Shadows") return "阴影";
            if (group == "Diagnostics") return "渲染诊断";
            if (group == "Capture") return "捕获";
            if (group == "Engine") return "引擎";
            if (group == "Rendering") return "渲染";
            return nullptr;
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
#if WITH_EDITOR
        if (Application::GetInstance().GetRunOptions().ExitAfterFrames > 0)
        {
            if (const char* capturePage = std::getenv("HE_EDITOR_SETTINGS_CAPTURE"))
            {
                const std::string_view requested(capturePage);
                mSelectedPage = requested == "Engine.Input" || requested == "Engine.Navigation"
                    ? std::string(requested) : "Rendering.Pipeline";
            }
        }
#endif
    }

    void ProjectSettingsPanel::OnImGuiRender()
    {
        if (IsOpen())
        {
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            const ImVec2 preferred = ImGuiWrapper::S(1080.0f, 760.0f);
            const float margin = ImGuiWrapper::S(24.0f);
            // WorkSize is already in final pixels. Keep the first opening on screen.
            ImGui::SetNextWindowSize(ImVec2(
                std::min(preferred.x, viewport->WorkSize.x - margin),
                std::min(preferred.y, viewport->WorkSize.y - margin)), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        }
        BasePanel::OnImGuiRender();
    }

    void ProjectSettingsPanel::Refresh(bool bDiscardPending)
    {
        mBaseSnapshot = RenderSettingsService::GetInstance().GetSnapshot();
        mSnapshot = mEditedView.empty() ? mBaseSnapshot : ResolveRenderViewSettings(mBaseSnapshot,
            Json::object(), mBaseSnapshot.Requested.ViewOverrides.value(mEditedView, Json::object()));
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
        if (descriptor.PropertyId == RenderSettingIds::ViewOverrides) return false;
        if (descriptor.PropertyId.starts_with("Rendering.Clouds.") &&
            descriptor.PropertyId != RenderSettingIds::CloudQuality)
        {
            if (GetDisplayedValue(RenderSettingIds::CloudQuality) == "Off") return false;
            const Json features = GetDisplayedValue(RenderSettingIds::PipelineFeatures);
            if (!features.is_array() || !std::ranges::any_of(features, [](const Json& feature) {
                return feature.value("type", "") == "builtin.cloud" && feature.value("enabled", false);
            })) return false;
        }
        if (!mEditedView.empty() && (!descriptor.bPersistedRenderSetting ||
            !SupportsLayer(descriptor, SettingsLayer::Camera))) return false;
        if (!bShowAdvanced && descriptor.bAdvanced)
            return false;
        if (mSearch[0] != '\0')
        {
            const std::string needle = LowerAscii(mSearch.data());
            const std::string haystack = LowerAscii(
                descriptor.PropertyId + " " + descriptor.EnglishFallback + " " +
                SettingLabel(descriptor));
            return haystack.find(needle) != std::string::npos;
        }
        return IsDescriptorOnSelectedPage(descriptor);
    }

    bool ProjectSettingsPanel::IsDescriptorOnSelectedPage(
        const RenderSettingDescriptor& descriptor) const
    {
        const std::string_view id = descriptor.PropertyId;
        if (id.starts_with("Rendering.Pipeline.")) return false;
        if (mSelectedPage == "All")
            return true;
        if (mSelectedPage == "Engine.General")
            return id == RenderSettingIds::Backend;
        if (mSelectedPage == "Engine.Display")
            return id.starts_with("Rendering.Presentation.");
        if (mSelectedPage == "Rendering.Pipeline")
            return descriptor.Subcategory == "Pipeline" ||
                descriptor.Subcategory == "Visibility" || descriptor.Subcategory == "Memory";
        if (mSelectedPage == "Rendering.AO") return id.starts_with("Rendering.AO.");
        if (mSelectedPage == "Rendering.Reflections") return id.starts_with("Rendering.Reflections.");
        if (mSelectedPage == "Rendering.AntiAliasing")
            return id.starts_with("Rendering.AntiAliasing.");
        if (mSelectedPage == "Rendering.GI.General")
        {
            return id == RenderSettingIds::GiTechnique ||
                id == RenderSettingIds::GiUnsupportedFallback ||
                id == RenderSettingIds::GiRenderScale;
        }
        if (mSelectedPage == "Rendering.GI.ScreenSpace")
            return id == RenderSettingIds::GiTechnique || id.starts_with("Rendering.GI.SSGI.");
        if (mSelectedPage == "Rendering.GI.Hardware")
        {
            return id == RenderSettingIds::GiTechnique || id == RenderSettingIds::GiSamplesPerPixel ||
                id == RenderSettingIds::GiMaxBounces;
        }
        if (mSelectedPage == "Rendering.GI.Denoising")
        {
            return id == RenderSettingIds::GiDenoiser ||
                id == RenderSettingIds::GiAtrousIterations || id == RenderSettingIds::GiSvgfOptimizedKernels ||
                id == RenderSettingIds::GiBilateralUpsample;
        }
        if (mSelectedPage == "Rendering.Clouds")
            return id.starts_with("Rendering.Clouds.");
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
        if (mSelectedPage == "Rendering.Pipeline") return "渲染 / 管线与性能";
        if (mSelectedPage == "Rendering.AO") return "渲染 / 环境光遮蔽（AO）";
        if (mSelectedPage == "Rendering.Reflections") return "渲染 / 反射（SSR / 探针）";
        if (mSelectedPage == "Rendering.AntiAliasing") return "渲染 / 抗锯齿";
        if (mSelectedPage == "Rendering.GI.General") return "渲染 / 全局光照 / 通用";
        if (mSelectedPage == "Rendering.GI.ScreenSpace") return "渲染 / 全局光照 / 屏幕空间 GI";
        if (mSelectedPage == "Rendering.GI.Hardware") return "渲染 / 全局光照 / 硬件光追 GI";
        if (mSelectedPage == "Rendering.GI.Denoising") return "渲染 / 全局光照 / 降噪";
        if (mSelectedPage == "Rendering.Clouds") return "渲染 / 体积云";
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
            return "查看全部设置。修改后需要应用；每项下方显示当前实际生效值与来源。";
        if (mSelectedPage == "Rendering.Pipeline") return "选择渲染路径，调整遮挡剔除、绘制合批、LOD 和显存复用。";
        if (mSelectedPage == "Rendering.AO") return "先开启环境光遮蔽，再选择 GTAO 或 SSAO，调整接触暗部的半径与强度。";
        if (mSelectedPage == "Rendering.Reflections") return "分别开启屏幕空间反射（SSR）和局部反射探针；下方参数控制 SSR 追踪质量。";
        if (mSelectedPage == "Engine.General")
            return "选择项目请求的渲染后端；不可用请求会保留并由能力解析器回退。";
        if (mSelectedPage == "Engine.Display")
            return "控制窗口呈现行为；命令行覆盖会显示为更高优先级来源。";
        if (mSelectedPage == "Rendering.AntiAliasing")
            return "配置实时抗锯齿模式和各算法参数，不支持的选项会提前禁用。";
        if (mSelectedPage == "Rendering.GI.General")
            return "选择全局光照技术、回退策略和内部渲染比例。";
        if (mSelectedPage == "Rendering.GI.ScreenSpace")
            return "开启步骤：全局光照技术选择 SSGI → 选择底部保存范围 → 点击应用。只调整射线参数不会自动开启 SSGI。";
        if (mSelectedPage == "Rendering.GI.Hardware")
            return "配置硬件光追 GI 的采样与反弹预算。";
        if (mSelectedPage == "Rendering.GI.Denoising")
            return "配置可用降噪器、空间过滤次数和上采样方式。";
        if (mSelectedPage == "Rendering.PostProcess")
            return "设置曝光、泛光和色彩。场景后期处理组件或相机的覆盖值优先于这里的项目默认值。SSGI 位于“分类与搜索”的全局光照分类。";
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
        selectable("  输入动作", "Engine.Input");
        selectable("  导航烘焙", "Engine.Navigation");
        ImGui::Dummy(ImGuiWrapper::S(0.0f, 6.0f));
        ImGui::TextDisabled("渲染");
        selectable("  体积云", "Rendering.Clouds");
        selectable("  后期处理", "Rendering.PostProcess");
        selectable("  管线与性能", "Rendering.Pipeline");
        selectable("  抗锯齿", "Rendering.AntiAliasing");
        selectable("  环境光遮蔽（AO）", "Rendering.AO");
        selectable("  反射（SSR / 探针）", "Rendering.Reflections");
        if (ImGui::TreeNodeEx("GlobalIlluminationSettingsTree",
                ImGuiTreeNodeFlags_DefaultOpen |
                    ImGuiTreeNodeFlags_SpanAvailWidth,
                "  全局光照"))
        {
            selectable("    通用", "Rendering.GI.General");
            selectable("  SSGI（屏幕空间）", "Rendering.GI.ScreenSpace");
            selectable("    硬件光追 GI", "Rendering.GI.Hardware");
            selectable("    降噪", "Rendering.GI.Denoising");
            ImGui::TreePop();
        }
        selectable("  阴影", "Rendering.Shadows");
        selectable("  诊断与捕获", "Rendering.Diagnostics");
        ImGui::Dummy(ImGuiWrapper::S(0.0f, 6.0f));
        selectable("全部设置", "All");

    }

    void ProjectSettingsPanel::DrawSetting(
        const RenderSettingDescriptor& descriptor)
    {
        const std::string& id = descriptor.PropertyId;
        Json value = GetDisplayedValue(id);
        Json effectiveValues = RenderSettingsToFlatValues(mSnapshot.Effective);
        effectiveValues.update(RenderDebugSettingsToFlatValues(mSnapshot.Debug));
        effectiveValues.update(RenderCaptureSettingsToFlatValues(mSnapshot.Capture));
        const Json effective = effectiveValues.value(id, value);
        const bool bPendingReset = mPendingPatch.contains(id) &&
            mPendingPatch[id].is_null();
        bool bChanged = false;

        ImGui::PushID(id.c_str());
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextWrapped("%s", SettingLabel(descriptor));
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s\n%s",
                descriptor.EnglishFallback.c_str(), id.c_str());
        }
        if (const char* help = SettingHelp(id))
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", help);
            ImGui::PopStyleColor();
        }

        if (ImGui::TableGetColumnCount() > 1)
            ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (bPendingReset)
        {
            ImGui::TextDisabled("应用后恢复继承");
        }
        else if (descriptor.Type == RenderSettingValueType::Boolean)
        {
            bool current = value.is_boolean() && value.get<bool>();
            bChanged = ImGui::Checkbox(current ? "已开启##Value" : "已关闭##Value", &current);
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
            if (ImGui::BeginCombo("##Value", OptionLabel(current)))
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
                    if (ImGui::Selectable(OptionLabel(option), option == current) &&
                        availability.bAvailable)
                    {
                        current = option;
                        bChanged = true;
                    }
                    if (!availability.bAvailable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("%s", availability.Reason.c_str());
                    ImGui::EndDisabled();
                }
                ImGui::EndCombo();
            }
            value = current;
        }

        if (bChanged)
        {
            mPendingPatch[id] = value;
            mLastMessage.clear();
        }

        const SettingsLayer source = mSnapshot.EffectiveSources.contains(id)
            ? mSnapshot.EffectiveSources.at(id) : SettingsLayer::EmergencyFallback;
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("设置层解析值：%s · 来源：%s",
            DisplayValue(effective).c_str(), ChineseLayer(source));
        ImGui::PopStyleColor();
        if (mPendingPatch.contains(id))
            ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "待应用");
        if (!mPendingPatch.contains(id) && effective != value)
            ImGui::TextWrapped("设置值与解析值不同，请检查覆盖来源或功能回退提示。");
        const auto availability = mSnapshot.Availability.find(id);
        if (availability != mSnapshot.Availability.end() &&
            !availability->second.bAvailable)
        {
            ImGui::TextWrapped("回退：%s",
                availability->second.Reason.c_str());
        }

        if (bShowAdvanced || descriptor.bRestartRequired)
            ImGui::TextWrapped("应用影响：%s", ChineseImpact(descriptor.Impact));
        if (bShowAdvanced)
        {
            if (ImGui::SmallButton("恢复继承"))
            {
                mPendingPatch[id] = nullptr;
                mLastMessage.clear();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("应用后移除所选保存范围的覆盖值，使用其他层的设置。");
        }
        ImGui::PopID();
    }

    void ProjectSettingsPanel::QueueCurrentPageReset()
    {
        mLastMessage.clear();
        const Json layerValues = RenderSettingsService::GetInstance().GetLayerValues(mTargetLayer);
        const Json values = mEditedView.empty() ? layerValues :
            layerValues.value(std::string(RenderSettingIds::ViewOverrides), Json::object()).value(mEditedView, Json::object());
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

    void ProjectSettingsPanel::DrawPipelineProfiles()
    {
        ImGui::TextUnformatted("管线配置资产");
        ImGui::TextWrapped("已应用配置：%s · 设置层解析管线：%s", mSnapshot.Requested.PipelineProfile.c_str(),
            mSnapshot.Effective.PipelineType.c_str());
        ImGui::TextWrapped("选择配置进入草稿；使用窗口底部的应用按钮发布到所选范围。保存资产会立即写入项目。");
        const Json pipelineValue = GetDisplayedValue(RenderSettingIds::PipelineType);
        const std::string selectedType = pipelineValue.is_string() ? pipelineValue.get<std::string>() : "realtime";
        if (ImGui::BeginCombo("管线实现", selectedType.c_str()))
        {
            for (const auto& type : EnumerateRenderPipelineTypes())
                if (ImGui::Selectable(type.c_str(), type == selectedType))
                    mPendingPatch[RenderSettingIds::PipelineType] = type;
            ImGui::EndCombo();
        }
        if (!IsRenderPipelineRegistered(selectedType))
            ImGui::TextWrapped("此请求的管线插件未注册；应用时将回退到兼容实时管线，原始请求保留。");
        if (ImGui::Button("打开离线渲染面板"))
            EditorLayer::GetPanel<OfflineRendererPanel>()->Open();
        ImGui::TextWrapped("离线路径追踪使用现有独立会话：在视口模式中选择 Path Tracing，然后在离线渲染面板控制采样、预算、AOV、暂停和输出。实时配置不替代离线会话设置。");
        const auto queueProfile = [&](const PipelineProfile& profile)
        {
            const Json patch = BuildPipelineProfilePatch(profile);
            for (const auto& [id, value] : patch.items()) mPendingPatch[id] = value;
            mLastMessage = "配置已载入草稿，等待应用。";
        };
        const auto attempt = [&](auto action)
        {
            try { action(); }
            catch (const std::exception& exception) { mLastMessage = std::string("配置资产操作失败：") + exception.what(); }
        };
        std::vector<std::string> profileNames;
        attempt([&] { profileNames = ListPipelineProfiles(); });
        {
            if (ImGui::BeginCombo("选择配置", mSelectedProfileAsset.empty() ? "选择内建或项目配置" : mSelectedProfileAsset.c_str()))
            {
                for (const auto* name : { "Default", "Low Cost", "High Quality" })
                    if (ImGui::Selectable(name))
                    {
                        attempt([&] { queueProfile(MakeBuiltinPipelineProfile(name)); });
                        mSelectedProfileAsset.clear();
                    }
                for (const auto& name : profileNames)
                    if (ImGui::Selectable(name.c_str(), name == mSelectedProfileAsset))
                    {
                        attempt([&] {
                            queueProfile(LoadPipelineProfile(name));
                            mSelectedProfileAsset = name;
                            std::snprintf(mProfileName.data(), mProfileName.size(), "%s", name.c_str());
                        });
                    }
                ImGui::EndCombo();
            }
            ImGui::InputText("资产名称", mProfileName.data(), mProfileName.size());
            const auto capture = [&]()
            {
                auto profile = CapturePipelineProfile(mProfileName.data(), mSnapshot.Requested);
                if (!mSelectedProfileAsset.empty())
                {
                    const auto old = LoadPipelineProfile(mSelectedProfileAsset);
                    for (const auto& [id, value] : old.Values.items())
                        if (!FindRenderSettingDescriptor(id)) profile.Values[id] = value;
                }
                return profile;
            };
            ImGui::BeginDisabled(mProfileName[0] == '\0' || !mPendingPatch.empty());
            if (ImGui::Button("从当前配置创建 / 复制"))
            {
                attempt([&] {
                    auto profile = capture();
                    SavePipelineProfile(profile);
                    mSelectedProfileAsset = profile.Name;
                    mLastMessage = "配置资产已保存。";
                });
            }
            SameLineIfFits("保存当前配置到资产");
            ImGui::BeginDisabled(mSelectedProfileAsset.empty() || mSelectedProfileAsset != mProfileName.data());
            if (ImGui::Button("保存当前配置到资产"))
            {
                attempt([&] {
                    SavePipelineProfile(capture(), true);
                    mLastMessage = "配置资产已更新。";
                });
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            ImGui::BeginDisabled(mSelectedProfileAsset.empty() || mProfileName[0] == '\0' ||
                mSelectedProfileAsset == mProfileName.data() || !mPendingPatch.empty());
            if (ImGui::Button("重命名所选资产"))
            {
                attempt([&] {
                    RenamePipelineProfile(mSelectedProfileAsset, mProfileName.data());
                    auto profile = LoadPipelineProfile(mProfileName.data());
                    mSelectedProfileAsset = profile.Name;
                    queueProfile(profile);
                    mLastMessage = "资产已重命名；应用草稿以更新当前配置名称。";
                });
            }
            ImGui::EndDisabled();
            if (!mPendingPatch.empty()) ImGui::TextDisabled("应用或取消草稿后可保存资产。");
            const Json displayed = GetDisplayedValue(RenderSettingIds::PipelineFeatures);
            Json features = displayed.is_array() ? displayed : Json::array();
            ImGui::Separator();
            ImGui::TextUnformatted("渲染功能");
            ImGui::TextWrapped("功能按依赖阶段执行。描边参与场景合成；后处理控制泛光和颜色调整，显示输出由管线维护。");
            const auto registered = EnumerateRenderFeatures();
            if (ImGui::BeginCombo("添加功能", "选择已注册功能"))
            {
                for (const auto& descriptor : registered)
                    if (ImGui::Selectable(descriptor.Type.c_str()))
                    {
                        std::string instance;
                        size_t suffix = features.size();
                        do { instance = descriptor.Type + "." + std::to_string(++suffix); }
                        while (std::ranges::any_of(features, [&](const Json& item) { return item.value("instance", "") == instance; }));
                        features.push_back({ { "type", descriptor.Type }, { "version", descriptor.Version },
                            { "instance", instance }, { "enabled", true }, { "parameters", Json::object() } });
                        mPendingPatch[RenderSettingIds::PipelineFeatures] = features;
                    }
                ImGui::EndCombo();
            }
            if (features.empty())
            {
                ImGui::TextDisabled("当前未添加可选功能。");
                if (ImGui::Button("添加默认功能组合"))
                    mPendingPatch[RenderSettingIds::PipelineFeatures] = DefaultRenderFeatureConfigurations();
            }
            else
            {
                for (size_t i = 0; i < features.size(); ++i)
                {
                    auto& feature = features[i];
                    ImGui::PushID(static_cast<int>(i));
                    bool enabled = feature.value("enabled", true);
                    const std::string type = feature.value("type", "");
                    const char* label = type == "builtin.outline" ? "描边" : type == "builtin.cloud" ? "体积云" : type == "builtin.postprocess" ? "后处理" : type.c_str();
                    const auto registeredType = std::ranges::find(registered, type, &RenderFeatureDescriptor::Type);
                    const bool supported = registeredType != registered.end() && feature.value("version", 0u) == registeredType->Version;
                    ImGui::BeginDisabled(!supported);
                    if (ImGui::Checkbox(label, &enabled))
                    {
                        feature["enabled"] = enabled;
                        mPendingPatch[RenderSettingIds::PipelineFeatures] = features;
                    }
                    ImGui::EndDisabled();
                    if (!supported) ImGui::TextWrapped("缺少插件或配置版本不兼容；请求参数保留，实际未启用。");
                    else ImGui::TextDisabled("阶段：%s", registeredType->Stage == RenderFeatureStage::TemporalComposite ? "时间重建后合成" :
                        registeredType->Stage == RenderFeatureStage::SceneOverlay ? "场景合成" :
                        registeredType->Stage == RenderFeatureStage::HdrPostProcess ? "HDR 后处理" : "显示颜色");
                    if (i > 0 && supported)
                    {
                        const auto previousType = std::ranges::find(registered, features[i - 1].value("type", ""), &RenderFeatureDescriptor::Type);
                        const bool sameStage = previousType != registered.end() && previousType->Stage == registeredType->Stage;
                        ImGui::BeginDisabled(!sameStage);
                        if (ImGui::SmallButton("同阶段前移"))
                        {
                            std::swap(features[i], features[i - 1]);
                            mPendingPatch[RenderSettingIds::PipelineFeatures] = features;
                        }
                        ImGui::EndDisabled();
                    }
                    if (ImGui::SmallButton("移除"))
                    {
                        features.erase(features.begin() + i);
                        mPendingPatch[RenderSettingIds::PipelineFeatures] = features;
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }
                if (ImGui::Button("恢复默认功能组合")) mPendingPatch[RenderSettingIds::PipelineFeatures] = MakeEmergencyFallbackRenderSettings().PipelineFeatures;
            }
            ImGui::Separator();
        }
    }

    void ProjectSettingsPanel::DrawSettings()
    {
        if (mSelectedPage == "Engine.Input") { DrawInputSettings(); return; }
        if (mSelectedPage == "Engine.Navigation") { DrawNavigationSettings(); return; }
        ImGui::BeginDisabled(!mPendingPatch.empty());
        const char* scopeLabel = mEditedView.empty() ? "默认配置" : mEditedView == "main" ? "主游戏视图" : mEditedView == "editor" ? "编辑器场景视图" : "资源预览";
        if (ImGui::BeginCombo("编辑视图", scopeLabel))
        {
            for (const auto* view : { "", "main", "editor", "preview" })
            {
                const char* label = view[0] == '\0' ? "默认配置" : std::string_view(view) == "main" ? "主游戏视图" : std::string_view(view) == "editor" ? "编辑器场景视图" : "资源预览";
                if (ImGui::Selectable(label, mEditedView == view)) { mEditedView = view; Refresh(true); }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::TextWrapped("解析值来自已应用设置和所选视图预设，未合并场景或相机组件覆盖；此处不显示实时画面状态。待应用修改为草稿。");
        if (!mEditedView.empty()) ImGui::TextWrapped("正在编辑该视图的独立覆盖；项目 / 用户 / 会话保存范围仍由下方控制。设备设置由默认配置管理。");
        if (mSelectedPage == "Rendering.Pipeline") DrawPipelineProfiles();
        if (mSelectedPage == "Rendering.Clouds")
        {
            const Json features = GetDisplayedValue(RenderSettingIds::PipelineFeatures);
            const bool cloudActive = features.is_array() && std::ranges::any_of(features, [](const Json& feature) {
                return feature.value("type", "") == "builtin.cloud" && feature.value("enabled", false);
            });
            if (!cloudActive) ImGui::TextWrapped("当前功能清单未启用体积云，因此本页参数暂不影响画面。请在管线页添加或启用体积云；现有清单保持不变。");
        }
        ImGui::TextUnformatted(GetSelectedPageTitle());
        ImGui::TextWrapped("%s", GetSelectedPageDescription());
        ImGui::Dummy(ImGuiWrapper::S(0.0f, 4.0f));
        ImGui::Checkbox("显示高级参数", &bShowAdvanced);
        SameLineIfFits(mSearch[0] != '\0' ? "恢复搜索结果的继承" : "恢复本页继承");
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
            ImGui::TextWrapped("设置层解析的全局光照：%s",
                DisplayValue(RenderSettingsToFlatValues(mSnapshot.Effective).at(
                    std::string(RenderSettingIds::GiTechnique))).c_str());
            if (mSnapshot.Debug.bGiDirectOnly)
                ImGui::TextWrapped("“仅直接光照”调试已开启，间接光照不会显示。请在诊断与捕获中关闭该选项并应用。");
            ImGui::TextWrapped("质量预设：%s（调整质量不会切换全局光照技术）", OptionLabel(std::string(ToString(preset))));
            bool bFirstPreset = true;
            for (GlobalIlluminationQualityPreset option : {
                     GlobalIlluminationQualityPreset::Low,
                     GlobalIlluminationQualityPreset::Balanced,
                     GlobalIlluminationQualityPreset::High })
            {
                const std::string label = OptionLabel(std::string(ToString(option)));
                if (!bFirstPreset) SameLineIfFits(label.c_str());
                bFirstPreset = false;
                if (ImGui::Button(label.c_str()))
                {
                    mPendingPatch.update(
                        GetGlobalIlluminationQualityPresetPatch(
                            option, preview.GlobalIllumination.Technique));
                    mLastMessage.clear();
                }
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
                    ImGui::TextWrapped("%s", value.c_str());
                };
                summaryRow("显示适配器",
                    mSnapshot.Capabilities.Render.AdapterName.empty()
                        ? std::string("尚未探测")
                        : mSnapshot.Capabilities.Render.AdapterName);
                summaryRow("设置层解析后端",
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
        ImGui::TextWrapped("%zu 项设置 · 修改后点击底部应用",
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
            ImGui::TextWrapped("当前分类和过滤条件下没有可显示的设置；可尝试开启高级参数或修改搜索词。");
            return;
        }

        for (const auto& [groupName, descriptors] : groups)
        {
            ImGui::PushID(groupName.c_str());
            ImGui::Dummy(ImGuiWrapper::S(0.0f, 8.0f));
            const char* groupLabel = ChineseGroupLabel(groupName);
            ImGui::TextWrapped("%s", groupLabel ? groupLabel : groupName.c_str());
            ImGui::Separator();
            const bool bCompact = ImGui::GetContentRegionAvail().x < ImGuiWrapper::S(600.0f);
            if (ImGui::BeginTable("##SettingsGroupV2", bCompact ? 1 : 2,
                    ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_BordersInnerH |
                    ImGuiTableFlags_NoSavedSettings |
                    ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("设置",
                    ImGuiTableColumnFlags_WidthStretch, 1.0f);
                if (!bCompact)
                    ImGui::TableSetupColumn("设置值 / 当前状态",
                        ImGuiTableColumnFlags_WidthStretch, 1.25f);
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
            .BaseRevision = mBaseSnapshot.Revision,
            .BaseHash = mBaseSnapshot.Hash,
            .Patch = Json::object(),
        };
        for (const auto& [id, value] : mPendingPatch.items())
        {
            const RenderSettingDescriptor* descriptor =
                FindRenderSettingDescriptor(id);
            if (descriptor && SupportsLayer(*descriptor, layer))
                draft.Patch[id] = value;
        }
        if (draft.Patch.empty())
        {
            mLastMessage = "当前保存范围不支持这些修改，请切换保存范围。";
            return;
        }
        const Json appliedPatch = draft.Patch;
        if (!mEditedView.empty())
        {
            const Json layerValues = RenderSettingsService::GetInstance().GetLayerValues(layer);
            Json views = layerValues.value(std::string(RenderSettingIds::ViewOverrides), Json::object());
            Json view = views.value(mEditedView, Json::object());
            for (const auto& [id, value] : draft.Patch.items())
            {
                const auto* descriptor = FindRenderSettingDescriptor(id);
                if (!descriptor || !descriptor->bPersistedRenderSetting || !SupportsLayer(*descriptor, SettingsLayer::Camera)) continue;
                if (value.is_null()) view.erase(id); else view[id] = value;
            }
            views[mEditedView] = view;
            draft.Patch = { { RenderSettingIds::ViewOverrides, views } };
        }
        const RenderSettingsApplyResult result =
            RenderSettingsService::GetInstance().ApplyDraft(draft, bPersist);
        mLastMessage = result.bSuccess
            ? (result.ChangePlan.bProcessRestartRequired
                ? "设置已保存；图形 API 将在重启 Editor/Game 后切换。"
                : layer == SettingsLayer::Session ? "已应用到本次会话，关闭程序后失效。"
                : layer == SettingsLayer::Project ? "已应用并保存到项目。"
                : "已应用并保存到我的设置。")
            : "应用失败：" + result.ErrorCode + " · " + result.Message;
        if (result.bSuccess)
        {
            for (const auto& [id, value] : appliedPatch.items())
            {
                (void)value;
                mPendingPatch.erase(id);
            }
            if (!mPendingPatch.empty())
                mLastMessage += " 部分修改不支持当前保存范围，已保留，请切换范围后应用。";
        }
        Refresh(false);
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
        const char* layerNames[] = { "项目（共享默认值）", "我的设置（本机）", "本次会话（临时试用）" };
        int target = mTargetLayer == SettingsLayer::Project ? 0
            : mTargetLayer == SettingsLayer::User ? 1 : 2;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("保存范围");
        SameLineIfFits("本次会话（临时试用）  ");
        ImGui::SetNextItemWidth(std::min(ImGuiWrapper::S(230.0f), ImGui::GetContentRegionAvail().x));
        if (ImGui::Combo("##TargetSettingsLayer", &target, layerNames,
                static_cast<int>(std::size(layerNames))))
        {
            mTargetLayer = target == 0 ? SettingsLayer::Project
                : target == 1 ? SettingsLayer::User : SettingsLayer::Session;
        }
        const char* applyLabel = mTargetLayer == SettingsLayer::Session
            ? "应用并试用" : "应用并保存";
        SameLineIfFits(applyLabel);
        ImGui::BeginDisabled(mPendingPatch.empty());
        if (ImGui::Button(applyLabel))
            ApplyPending(mTargetLayer, mTargetLayer != SettingsLayer::Session);
        SameLineIfFits("放弃修改");
        if (ImGui::Button("放弃修改"))
        {
            Refresh(true);
            mLastMessage.clear();
        }
        ImGui::EndDisabled();
        SameLineIfFits("刷新解析值");
        if (ImGui::Button("刷新解析值"))
        {
            Refresh(false);
            mLastMessage = "已刷新设置层解析值，待应用修改仍保留。";
        }

        ImGui::TextWrapped("项目保存为共享默认值；我的设置仅保存在本机；本次会话关闭后失效。会话、命令行、场景或相机覆盖可能使生效值不同。应用会处理所有页面的待修改项。");
        if (!mLastMessage.empty())
            ImGui::TextWrapped("%s", mLastMessage.c_str());
        if (!mPendingPatch.empty())
            ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f),
                "有 %zu 个待应用字段。", mPendingPatch.size());
        else if (mLastMessage.empty())
            ImGui::TextDisabled("没有待应用修改。");

        if (bShowAdvanced && ImGui::TreeNode("恢复全部设置"))
        {
            ImGui::TextWrapped("清除项目、我的设置和本次会话的全部覆盖，恢复引擎默认值。");
            if (ImGui::Button("清除全部覆盖"))
            {
                const RenderSettingsApplyResult result =
                    RenderSettingsService::GetInstance().ResetToEngineDefault(
                        mBaseSnapshot.Revision, mBaseSnapshot.Hash);
                mLastMessage = result.bSuccess ? "所有可写层已清除，已恢复引擎默认值。"
                    : "恢复失败：" + result.Message;
                Refresh(result.bSuccess);
            }
            ImGui::TreePop();
        }
    }

    void ProjectSettingsPanel::DrawInputSettings()
    {
        const auto project = ConfigManager::GetInstance().GetRootFolder();
        const auto path = project / "Config/Input.json";
        auto reload = [&]() {
            InputActions loaded;
            if (std::filesystem::exists(path)) loaded.Load(path);
            mInputDraft = loaded.GetMap(); mInputMessage.clear();
        };
        if (mInputProject != project.generic_string())
        {
            mInputProject = project.generic_string(); mInputDraft = InputActions::Defaults();
            try { reload(); } catch (const std::exception& error) { mInputMessage = error.what(); }
        }
        ImGui::TextUnformatted("输入动作 / Config/Input.json");
        ImGui::TextWrapped("草稿保存前会完整校验。键码使用 GLFW 编号（W=87、Space=32）；手柄使用引擎 GamepadCodes 枚举。Axis 为输出分量 0/1，Scale 可反转方向。");
        if (!mInputMessage.empty()) ImGui::TextWrapped("%s", mInputMessage.c_str());
        ImGui::BeginDisabled(!ModeManager::IsEditState());
        if (ImGui::Button("保存项目输入"))
        {
            try
            {
                InputActions validated; validated.Configure(mInputDraft); validated.Save(path);
                InputActions::Get().BeginSession(mInputDraft); mInputMessage = "已保存；下次 Play 使用此配置。";
            }
            catch (const std::exception& error) { mInputMessage = error.what(); }
        }
        ImGui::SameLine();
        if (ImGui::Button("重新读取"))
            try { reload(); } catch (const std::exception& error) { mInputMessage = error.what(); }
        ImGui::SameLine();
        if (ImGui::Button("默认配置草稿")) mInputDraft = InputActions::Defaults();
        auto editText = [](const char* label, std::string& value) {
            std::array<char, 129> buffer{};
            std::copy_n(value.data(), std::min(value.size(), buffer.size() - 1), buffer.data());
            if (ImGui::InputText(label, buffer.data(), buffer.size())) value = buffer.data();
        };
        auto choice = [](const char* label, std::string& value, std::initializer_list<const char*> choices) {
            if (ImGui::BeginCombo(label, value.c_str()))
            {
                for (const char* option : choices) if (ImGui::Selectable(option, value == option)) value = option;
                ImGui::EndCombo();
            }
        };
        for (size_t i = 0; i < mInputDraft.Actions.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            auto& action = mInputDraft.Actions[i];
            const bool open = ImGui::TreeNode("Action", "%s [%s]", action.Name.c_str(), action.Context.c_str());
            bool remove = false;
            if (open)
            {
                editText("Name", action.Name); editText("Context", action.Context);
                choice("Type", action.Type, {"Button", "Axis", "Vector2"});
                ImGui::DragFloat("Dead zone", &action.DeadZone, .01f, 0, .99f);
                ImGui::DragFloat("Hold seconds", &action.HoldSeconds, .01f, .01f, 60);
                ImGui::DragFloat("Double tap seconds", &action.DoubleTapSeconds, .01f, .01f, 5);
                for (size_t j = 0; j < action.Bindings.size(); ++j)
                {
                    ImGui::PushID(static_cast<int>(j));
                    auto& binding = action.Bindings[j];
                    ImGui::Separator();
                    choice("Device", binding.Device, {"Key", "MouseButton", "GamepadButton", "GamepadAxis", "PointerX", "PointerY"});
                    ImGui::InputInt("Code", &binding.Code); ImGui::InputInt("Axis", &binding.Axis);
                    ImGui::DragFloat("Scale", &binding.Scale, .1f, -100, 100);
                    const bool removeBinding = ImGui::Button("Remove binding");
                    ImGui::PopID();
                    if (removeBinding) { action.Bindings.erase(action.Bindings.begin() + j); break; }
                }
                if (action.Bindings.size() < 32 && ImGui::Button("Add binding")) action.Bindings.push_back({"Key", 32, 0, 1});
                remove = ImGui::Button("Remove action");
                ImGui::TreePop();
            }
            ImGui::PopID();
            if (remove) { mInputDraft.Actions.erase(mInputDraft.Actions.begin() + i); break; }
        }
        if (mInputDraft.Actions.size() < 256 && ImGui::Button("Add action"))
        {
            InputActionRes action; action.Name = "NewAction"; mInputDraft.Actions.push_back(std::move(action));
        }
        ImGui::EndDisabled();
    }

    void ProjectSettingsPanel::DrawNavigationSettings()
    {
        const auto project = ConfigManager::GetInstance().GetRootFolder();
        if (mNavigationProject != project.generic_string())
        {
            mNavigationProject = project.generic_string(); mNavigationDraft = {};
            try { mNavigationDraft = NavigationService::LoadProjectSettings(); }
            catch (const std::exception& error) { mNavigationMessage = error.what(); }
        }
        ImGui::TextUnformatted("导航烘焙 / Config/Navigation.json");
        ImGui::TextWrapped("按世界空间静态网格烘焙。空规则保留 NavGeometry / NavGround 约定；自定义规则按顺序匹配实体标签前缀。区域 63 禁止通行，其余区域代价至少为 1。动态障碍需要重新烘焙。");
        if (!mNavigationMessage.empty()) ImGui::TextWrapped("%s", mNavigationMessage.c_str());
        ImGui::BeginDisabled(!ModeManager::IsEditState());
        ImGui::DragFloat("Cell size", &mNavigationDraft.CellSize, .01f, .01f, 10);
        ImGui::DragFloat("Cell height", &mNavigationDraft.CellHeight, .01f, .01f, 10);
        ImGui::DragFloat("Agent height", &mNavigationDraft.AgentHeight, .05f, .1f, 100);
        ImGui::DragFloat("Agent radius", &mNavigationDraft.AgentRadius, .05f, 0, 100);
        ImGui::DragFloat("Max climb", &mNavigationDraft.AgentMaxClimb, .05f, 0, 100);
        ImGui::DragFloat("Max slope", &mNavigationDraft.AgentMaxSlope, 1, 0, 89);
        ImGui::InputInt("Grid cell budget", &mNavigationDraft.MaxGridCells);
        for (size_t i = 0; i < mNavigationDraft.Surfaces.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            auto& surface = mNavigationDraft.Surfaces[i];
            std::array<char, 129> prefix{};
            std::copy_n(surface.TagPrefix.data(), std::min(surface.TagPrefix.size(), prefix.size() - 1), prefix.data());
            if (ImGui::InputText("Tag prefix", prefix.data(), prefix.size())) surface.TagPrefix = prefix.data();
            ImGui::InputInt("Area", &surface.Area);
            ImGui::Checkbox("Use static mesh (off: flat proxy)", &surface.UseMesh);
            bool remove = ImGui::Button("Remove surface rule");
            ImGui::Separator(); ImGui::PopID();
            if (remove) { mNavigationDraft.Surfaces.erase(mNavigationDraft.Surfaces.begin() + i); break; }
        }
        if (mNavigationDraft.Surfaces.size() < 128 && ImGui::Button("Add surface rule"))
        { NavigationSurfaceRes surface; surface.TagPrefix = "NavGeometry"; mNavigationDraft.Surfaces.push_back(surface); }
        if (ImGui::TreeNode("Area costs"))
        {
            if (mNavigationDraft.AreaCosts.empty()) mNavigationDraft.AreaCosts.assign(64, 1);
            for (int area = 0; area < 63; ++area)
            {
                ImGui::PushID(area);
                ImGui::DragFloat(("Area " + std::to_string(area)).c_str(), &mNavigationDraft.AreaCosts[area], .1f, 1, 1000000);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        if (ImGui::Button("保存导航配置"))
        {
            try
            {
                NavigationService::ValidateSettings(mNavigationDraft);
                Utils::SaveJsonToFile(Json(mNavigationDraft), project / "Config/Navigation.json");
                mNavigationMessage = "配置已保存；已有网格需重新烘焙。";
            }
            catch (const std::exception& error) { mNavigationMessage = error.what(); }
        }
#if HE_ENABLE_NAVIGATION
        auto scene = mParent->GetPanel<SceneHierarchyPanel>()->GetContext();
        auto* navigation = scene ? scene->TryGetSystem<NavPathDebugSystem>() : nullptr;
        ImGui::BeginDisabled(!navigation);
        if (ImGui::Button("烘焙当前场景预览"))
        {
            if (navigation->Rebuild(mNavigationDraft))
                mNavigationMessage = "烘焙成功，导航预览使用运行时相同的路径查询。";
            else mNavigationMessage = navigation->GetNavigation().GetLastError();
        }
        bool visible = navigation && navigation->IsDebugVisible();
        if (ImGui::Checkbox("显示导航三角形（最多 20000 个）", &visible)) navigation->SetDebugVisible(visible);
        if (navigation && !navigation->GetNavigation().GetLastError().empty())
            ImGui::TextWrapped("%s", navigation->GetNavigation().GetLastError().c_str());
        ImGui::EndDisabled();
#else
        ImGui::TextWrapped("当前构建未启用导航模块；可编辑配置，无法烘焙。" );
#endif
        ImGui::EndDisabled();
    }

    void ProjectSettingsPanel::OnImGuiRenderImpl()
    {
        if (!bInitialized)
            Refresh(true);
        else if (mPendingPatch.empty())
            Refresh(false);
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float footerHeight = std::min(ImGuiWrapper::S(120.0f), available.y * 0.45f);
        ImGui::BeginChild("##ProjectSettingsBody", ImVec2(0.0f,
            std::max(1.0f, available.y - footerHeight - ImGui::GetStyle().ItemSpacing.y)), false);
        if (available.x < ImGuiWrapper::S(800.0f))
        {
            if (ImGui::Button("分类与搜索")) ImGui::OpenPopup("##SettingsNavigationPopup");
            ImGuiWrapper::SetNextWindowSize(ImVec2(260.0f, 520.0f), ImGuiCond_Appearing);
            if (ImGui::BeginPopup("##SettingsNavigationPopup"))
            {
                const std::string previousPage = mSelectedPage;
                DrawNavigation();
                if (previousPage != mSelectedPage) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            DrawSettings();
        }
        else if (ImGui::BeginTable("##ProjectSettingsLayoutV2", 2,
                ImGuiTableFlags_NoSavedSettings |
                ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_SizingFixedFit,
                ImGui::GetContentRegionAvail()))
        {
            ImGui::TableSetupColumn("##ProjectSettingsNavigation",
                ImGuiTableColumnFlags_WidthFixed,
                ImGuiWrapper::S(220.0f));
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
                ImVec2(0.0f, 0.0f), false);
            DrawSettings();
            ImGui::EndChild();
            ImGui::EndTable();
        }
        ImGui::EndChild();
        ImGui::BeginChild("##ProjectSettingsFooter", ImVec2(0.0f, 0.0f), false);
        if (mSelectedPage == "Engine.Input") ImGui::TextWrapped("输入动作仅在上方保存。Play 期间编辑被禁用；运行时重绑定可通过 InputActions.Save 写入玩家存档。");
        else if (mSelectedPage == "Engine.Navigation") ImGui::TextWrapped("导航配置在上方保存；场景预览网格与游戏场景互相隔离。NavStart / NavEnd 实体可显示路径，修改几何后重新烘焙。");
        else DrawFooter();
        ImGui::EndChild();
    }
}
