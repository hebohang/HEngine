#include "Hepch.h"

#include "Editor/Panels/RenderGraphDiagnosticsPanel.h"

#include "Editor/ImGuiWrapper/ImGuiWrapper.h"
#include "Runtime/Core/AppFramework/Application.h"
#include "Runtime/Graphics/Renderer/RenderPipeline.h"
#include "Runtime/Graphics/RHI/RenderDevice.h"

#include <imgui/imgui.h>

#include <algorithm>
#include <cctype>

namespace HEngine
{
    namespace
    {
        [[nodiscard]] const char* QueueName(RenderGraphQueue queue) noexcept
        {
            switch (queue)
            {
            case RenderGraphQueue::Graphics: return "图形";
            case RenderGraphQueue::Compute: return "计算";
            case RenderGraphQueue::Copy: return "复制";
            }
            return "未知";
        }

        [[nodiscard]] const char* ResourceKindName(
            RenderGraphDiagnosticResourceKind kind) noexcept
        {
            switch (kind)
            {
            case RenderGraphDiagnosticResourceKind::Texture: return "纹理";
            case RenderGraphDiagnosticResourceKind::Buffer: return "缓冲区";
            case RenderGraphDiagnosticResourceKind::AccelerationStructure:
                return "加速结构";
            }
            return "未知";
        }

        [[nodiscard]] const char* LifetimeName(
            RenderGraphResourceLifetime lifetime) noexcept
        {
            switch (lifetime)
            {
            case RenderGraphResourceLifetime::Imported: return "导入";
            case RenderGraphResourceLifetime::Persistent: return "持久";
            case RenderGraphResourceLifetime::Transient: return "瞬时";
            }
            return "未知";
        }

        [[nodiscard]] const char* AccessName(RenderGraphAccess access) noexcept
        {
            switch (access)
            {
            case RenderGraphAccess::Read: return "读";
            case RenderGraphAccess::Write: return "写";
            case RenderGraphAccess::ReadWrite: return "读写";
            }
            return "未知";
        }

        [[nodiscard]] bool ContainsInsensitive(
            std::string_view text,
            std::string_view needle)
        {
            if (needle.empty())
                return true;
            std::string normalizedText(text);
            std::string normalizedNeedle(needle);
            const auto lower = [](unsigned char value)
            {
                return static_cast<char>(std::tolower(value));
            };
            std::ranges::transform(normalizedText, normalizedText.begin(), lower);
            std::ranges::transform(normalizedNeedle,
                normalizedNeedle.begin(), lower);
            return normalizedText.find(normalizedNeedle) != std::string::npos;
        }
    }

    RenderGraphDiagnosticsPanel::RenderGraphDiagnosticsPanel(
        EditorLayer* parent,
        std::string panelName,
        bool* pOpen)
        : BasePanel(parent, std::move(panelName), pOpen)
    {
    }

    void RenderGraphDiagnosticsPanel::Refresh()
    {
        const Application& application = Application::GetInstance();
        mSnapshot = application.GetRenderGraphDiagnosticSnapshot();
        mCapturedFrame = application.GetFrameNumber();
        if (mSelectedPass >= mSnapshot.Passes.size())
            mSelectedPass = InvalidRenderGraphIndex;
    }

    void RenderGraphDiagnosticsPanel::DrawPassList()
    {
        ImGui::TextUnformatted("执行阶段");
        ImGui::Separator();
        for (const RenderGraphDiagnosticPassSnapshot& pass : mSnapshot.Passes)
        {
            if (!bShowCulled && pass.bCulled)
                continue;
            if (!ContainsInsensitive(pass.Name, mSearch.data()))
                continue;
            const bool bSelected = mSelectedPass == pass.PassId;
            ImGui::PushID(static_cast<int>(pass.PassId));
            if (pass.bCulled)
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
            const std::string label = pass.Name + "  [" +
                QueueName(pass.Queue) + "]";
            if (ImGui::Selectable(label.c_str(), bSelected))
                mSelectedPass = pass.PassId;
            if (pass.bCulled)
                ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }

    void RenderGraphDiagnosticsPanel::DrawPassDetails()
    {
        if (mSelectedPass == InvalidRenderGraphIndex ||
            mSelectedPass >= mSnapshot.Passes.size())
        {
            ImGui::TextDisabled("选择一个 Pass 查看只读依赖与资源访问。");
            return;
        }
        const RenderGraphDiagnosticPassSnapshot& pass =
            mSnapshot.Passes[mSelectedPass];
        ImGui::Text("%s", pass.Name.c_str());
        ImGui::TextDisabled("PassId %u · 调度提示 %s · 执行序号 %s",
            pass.PassId, QueueName(pass.Queue),
            pass.ExecutionIndex == InvalidRenderGraphIndex
                ? "已裁剪" : std::to_string(pass.ExecutionIndex).c_str());
        ImGui::TextDisabled("实际录制队列：%s", pass.bHasRecordedCommands
            ? QueueName(pass.RecordedQueue) : "尚未录制");
        if (pass.FeatureInstanceId.empty())
            ImGui::TextDisabled("管线内建阶段：顺序受输入依赖约束。");
        else
            ImGui::TextWrapped("渲染功能：%s（%s）", pass.FeatureInstanceId.c_str(), pass.FeatureTypeId.c_str());
        ImGui::Separator();
        if (ImGui::BeginTable("##PassAccesses", 4,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
        {
            ImGui::TableSetupColumn("访问");
            ImGui::TableSetupColumn("资源类型");
            ImGui::TableSetupColumn("资源 ID");
            ImGui::TableSetupColumn("用途位");
            ImGui::TableHeadersRow();
            for (const RenderGraphDiagnosticAccessSnapshot& access :
                mSnapshot.Accesses)
            {
                if (access.PassId != pass.PassId)
                    continue;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(AccessName(access.Access));
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(ResourceKindName(access.ResourceKind));
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%u", access.ResourceId);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("0x%08X", static_cast<std::uint32_t>(access.Usage));
            }
            ImGui::EndTable();
        }
    }

    void RenderGraphDiagnosticsPanel::DrawResources()
    {
        if (!ImGui::BeginTable("##GraphResources", 7,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY))
            return;
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed,
            ImGuiWrapper::S(48.0f));
        ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthFixed,
            ImGuiWrapper::S(72.0f));
        ImGui::TableSetupColumn("名称", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("生命周期");
        ImGui::TableSetupColumn("首次/末次");
        ImGui::TableSetupColumn("尺寸/字节");
        ImGui::TableSetupColumn("格式");
        ImGui::TableHeadersRow();
        for (const RenderGraphDiagnosticResourceSnapshot& resource :
            mSnapshot.Resources)
        {
            if (!ContainsInsensitive(resource.Name, mSearch.data()))
                continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%u", resource.ResourceId);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(ResourceKindName(resource.Kind));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(resource.Name.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(LifetimeName(resource.Lifetime));
            ImGui::TableSetColumnIndex(4);
            if (resource.PassInterval.IsUsed())
                ImGui::Text("%u / %u", resource.PassInterval.FirstUse,
                    resource.PassInterval.LastUse);
            else
                ImGui::TextDisabled("未使用");
            ImGui::TableSetColumnIndex(5);
            if (resource.Kind == RenderGraphDiagnosticResourceKind::Texture)
                ImGui::Text("%ux%u x%u", resource.Width,
                    resource.Height, resource.SampleCount);
            else if (resource.Kind == RenderGraphDiagnosticResourceKind::Buffer)
                ImGui::Text("%llu B",
                    static_cast<unsigned long long>(resource.ByteSize));
            else
                ImGui::TextUnformatted("—");
            ImGui::TableSetColumnIndex(6);
            ImGui::TextUnformatted(resource.FormatName.c_str());
        }
        ImGui::EndTable();
    }

    void RenderGraphDiagnosticsPanel::DrawDependencies()
    {
        for (const RenderGraphDiagnosticDependencySnapshot& dependency :
            mSnapshot.Dependencies)
        {
            const char* prerequisite = dependency.PrerequisitePassId <
                    mSnapshot.Passes.size()
                ? mSnapshot.Passes[dependency.PrerequisitePassId].Name.c_str()
                : "<invalid>";
            const char* dependent = dependency.DependentPassId <
                    mSnapshot.Passes.size()
                ? mSnapshot.Passes[dependency.DependentPassId].Name.c_str()
                : "<invalid>";
            ImGui::BulletText("%s  →  %s%s", prerequisite, dependent,
                dependency.bExplicit ? "  （显式）" : "");
        }
    }

    void RenderGraphDiagnosticsPanel::OnImGuiRenderImpl()
    {
        if (bAutoRefresh || mCapturedFrame == 0)
            Refresh();
        ImGui::Text("只读快照 · 帧 %llu · %ux%u",
            static_cast<unsigned long long>(mCapturedFrame),
            mSnapshot.Width, mSnapshot.Height);
        ImGuiWrapper::SameLine(0.0f);
        ImGui::TextColored(mSnapshot.bCompiled
                ? ImVec4(0.25f, 0.85f, 0.4f, 1.0f)
                : ImVec4(1.0f, 0.4f, 0.25f, 1.0f),
            mSnapshot.bCompiled ? "已编译" : "未编译");
        ImGuiWrapper::SameLine(0.0f);
        ImGui::Checkbox("自动刷新", &bAutoRefresh);
        ImGuiWrapper::SameLine(0.0f);
        if (ImGui::Button("刷新快照"))
            Refresh();
        ImGuiWrapper::SameLine(0.0f);
        ImGui::Checkbox("显示已裁剪", &bShowCulled);
        ImGui::SetNextItemWidth(ImGuiWrapper::S(280.0f));
        ImGui::InputTextWithHint("##GraphSearch", "筛选 Pass/资源…",
            mSearch.data(), mSearch.size());
        ImGui::TextDisabled("Pass %zu · Resource %zu · Access %zu · Dependency %zu",
            mSnapshot.Passes.size(), mSnapshot.Resources.size(),
            mSnapshot.Accesses.size(), mSnapshot.Dependencies.size());
        const auto& stats = Application::GetInstance().GetMainSceneStats();
        constexpr double mib = 1024.0 * 1024.0;
        ImGui::TextDisabled("实时名义内存：活动/保留 %.1f MiB · 空闲缓存 %.1f MiB · 退役 %.1f MiB · 已回收 %.1f MiB",
            (stats.RenderGraphPersistentBytes + stats.RenderGraphTransientReservedBytes) / mib,
            stats.RenderGraphCachedTransientBytes / mib, stats.RenderGraphRetiredResourceBytes / mib,
            stats.RenderGraphTrimmedTransientBytes / mib);
        const auto budget = Application::GetInstance().GetRenderMemoryBudget();
        if (budget.bAvailable)
            ImGui::TextDisabled("系统显存：本进程 %.1f MiB / 预算 %.1f MiB",
                budget.LocalUsageBytes / mib, budget.LocalBudgetBytes / mib);
        else
            ImGui::TextDisabled("系统显存预算：当前后端未提供；名义资源字节不代表实际显存占用。");
        for (const RenderGraphDiagnosticCompileErrorSnapshot& error :
            mSnapshot.CompileErrors)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                "%s", error.Message.c_str());
        }

        if (ImGuiWrapper::BeginResizableSplit("##RenderGraphSplit",
                1.15f, 2.4f))
        {
            DrawPassList();
            ImGui::TableSetColumnIndex(1);
            DrawPassDetails();
            ImGuiWrapper::EndResizableSplit();
        }
        if (ImGui::BeginTabBar("##RenderGraphTabs"))
        {
            if (ImGui::BeginTabItem("资源生命周期"))
            {
                DrawResources();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("依赖关系"))
            {
                DrawDependencies();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::TextDisabled(
            "诊断器只消费结构化纯值快照；这里不能编辑节点或改变渲染管线。");
    }
}
