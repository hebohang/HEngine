#include "Editor/Log/OutputLog.h"
#include "Editor/EditorLayer.h"
#include "Editor/ImGuiWrapper/ImGuiWrapper.h"
#include "Runtime/Core/Log/Log.h"

#include <spdlog/spdlog.h>
#include <imgui/imgui_internal.h>

namespace HEngine
{
	OutputLog::OutputLog()
	{
		ClearLog();
		memset(InputBuf, 0, sizeof(InputBuf));
		HistoryPos = -1;

		// "CLASSIFY" is here to provide the test case where "C"+[tab] completes to "CL" and display multiple matches.
		Commands.push_back("HELP");
		Commands.push_back("HISTORY");
		Commands.push_back("CLEAR");
		Commands.push_back("CLASSIFY");
		ScrollToBottom = false;
	}

	OutputLog::~OutputLog()
	{
		ClearLog();
		for (int i = 0; i < History.Size; i++)
			free(History[i]);
	}

	void OutputLog::ClearLog()
	{
		for (int i = 0; i < Items.Size; i++)
			free(Items[i]);
		Items.clear();
	}

	void OutputLog::AddLog(const char* fmt, ...)
	{
		// FIXME-OPT
		char buf[1024];
		va_list args;
		va_start(args, fmt);
		vsnprintf(buf, IM_ARRAYSIZE(buf), fmt, args);
		buf[IM_ARRAYSIZE(buf) - 1] = 0;
		va_end(args);

		size_t len = strlen(buf);
		while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
			buf[--len] = 0;

		Items.push_back(Strdup(buf));
	}

	void OutputLog::OnImGuiRender(bool* pOpen)
	{
        // Filters
        static bool bShowInfo = true;
        static bool bShowTrace = true;
        static bool bShowWarning = true;
        static bool bShowError = true;

        // Settings
        static bool bAutoScroll = true;
        static bool bSelectableCopy = true;

		const std::string outputLogTitle = std::string(EditorLayer::GetText("Output Log")) + "###LegacyOutputLog";
		if (!ImGui::Begin(outputLogTitle.c_str(), pOpen))
		{
			ImGui::End();
			return;
		}

        bool copy_to_clipboard = false;

        // Options menu
		constexpr const char* filtersPopupId = "LegacyOutputLogFilters";
        if (ImGui::BeginPopup(filtersPopupId))
        {
			ImGui::Checkbox((std::string(EditorLayer::GetText("Info")) + "###LegacyFilterInfo").c_str(), &bShowInfo);
			ImGui::Checkbox((std::string(EditorLayer::GetText("Trace")) + "###LegacyFilterTrace").c_str(), &bShowTrace);
			ImGui::Checkbox((std::string(EditorLayer::GetText("Warning")) + "###LegacyFilterWarning").c_str(), &bShowWarning);
			ImGui::Checkbox((std::string(EditorLayer::GetText("Error")) + "###LegacyFilterError").c_str(), &bShowError);
            ImGui::EndPopup();
        }

        // Options, Filter
		const std::string filtersLabel = std::string(EditorLayer::GetText("Filters")) + "###LegacyOutputLogFiltersButton";
        if (ImGui::Button(filtersLabel.c_str()))
			ImGui::OpenPopup(filtersPopupId);
        ImGui::SameLine();

        ImGui::SetNextItemWidth(380.0);
		bool value_changed = ImGui::InputTextWithHint("##SearchLog", EditorLayer::GetText("Search Log"), Filter.InputBuf, IM_ARRAYSIZE(Filter.InputBuf));
        if (value_changed)
            Filter.Build();

		const char* settingsLabel = EditorLayer::GetText("Settings");
		constexpr const char* settingsPopupId = "LegacyOutputLogSettings";

        static float logFontScale = 1.0f;
		if (ImGui::BeginPopup(settingsPopupId))
        {
            ImGui::PushItemWidth(100.0f);

			ImGui::TextUnformatted(EditorLayer::GetText("Auto-scroll"));
            ImGuiWrapper::SameLine(100.0f);
            ImGui::Checkbox("##Auto-scroll", &bAutoScroll);

			ImGui::TextUnformatted(EditorLayer::GetText("Font Size"));
            ImGuiWrapper::SameLine(100.0f);
			ImGui::SliderFloat("##Font Size", &logFontScale, 0.5f, 1.5f, EditorLayer::GetText("Scale Format"));

			ImGui::TextUnformatted(EditorLayer::GetText("Text Select"));
            ImGuiWrapper::SameLine(100.0f);
            ImGui::Checkbox("##Text Select", &bSelectableCopy);

            ImGui::PopItemWidth();

            ImGui::EndPopup();
        }

        // TODO: add setting Icon
        const float settingsButtonWidth = ImGui::CalcTextSize(settingsLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - settingsButtonWidth);
		const std::string settingsButtonLabel = std::string(settingsLabel) + "###LegacyOutputLogSettingsButton";
		if (ImGui::Button(settingsButtonLabel.c_str()))
			ImGui::OpenPopup(settingsPopupId);

        ImGui::Separator();

		// Reserve enough left-over height for 1 separator + 1 input text
		const float footer_height_to_reserve = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
		ImGui::BeginChild("ScrollingRegion", ImVec2(0, -footer_height_to_reserve), false, ImGuiWindowFlags_HorizontalScrollbar);
		if (ImGui::BeginPopupContextWindow())
		{
			if (ImGui::Selectable(EditorLayer::GetText("Clear Log"))) ClearLog();
			if (ImGui::Selectable(EditorLayer::GetText("Copy"))) copy_to_clipboard = true;
            ImGui::EndPopup();
		}

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1)); // Tighten spacing
        auto IsVisibleLog = [&](const char* item)
        {
            if (!Filter.PassFilter(item))
                return false;

            if (strstr(item, "[info]") && !bShowInfo)
                return false;

            if (strstr(item, "[trace]") && !bShowTrace)
                return false;

            if (strstr(item, "[warning]") && !bShowWarning)
                return false;

            if (strstr(item, "[error]") && !bShowError)
                return false;

            return true;
        };

        if (bSelectableCopy)
        {
            std::string visibleLogText;
            visibleLogText.reserve(Items.Size * 64);
            for (int i = 0; i < Items.Size; i++)
            {
                const char* item = Items[i];
                if (!IsVisibleLog(item))
                    continue;

                visibleLogText += item;
                visibleLogText += '\n';
            }

            if (copy_to_clipboard)
                ImGui::SetClipboardText(visibleLogText.c_str());

            static char emptyLogBuffer[1] = { 0 };
            char* logBuffer = visibleLogText.empty() ? emptyLogBuffer : visibleLogText.data();

            ImGui::InputTextMultiline(
                "##OutputLogSelectable",
                logBuffer,
                visibleLogText.size() + 1,
                ImGui::GetContentRegionAvail(),
                ImGuiInputTextFlags_ReadOnly);

            if (ImGui::BeginPopupContextItem("OutputLogSelectableContext"))
            {
				if (ImGui::Selectable(EditorLayer::GetText("Clear Log")))
                    ClearLog();
				if (ImGui::Selectable(EditorLayer::GetText("Copy")))
                    ImGui::SetClipboardText(visibleLogText.c_str());
                ImGui::EndPopup();
            }
        }
        else
        {
            if (copy_to_clipboard)
                ImGui::LogToClipboard();

            for (int i = 0; i < Items.Size; i++)
            {
                const char* item = Items[i];
                if (!IsVisibleLog(item))
                    continue;

                ImVec4 color;
                bool has_color = false;
                if (strstr(item, "[info]"))
                {
                    color = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
                    has_color = true;
                }
                else if (strstr(item, "[trace]"))
                {
                    color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
                    has_color = true;
                }
                else if (strstr(item, "[warning]"))
                {
                    color = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
                    has_color = true;
                }
                else if (strstr(item, "[error]"))
                {
                    color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
                    has_color = true;
                }
                else if (strncmp(item, "# ", 2) == 0)
                {
                    color = ImVec4(1.0f, 0.8f, 0.6f, 1.0f);
                    has_color = true;
                }

                ImGui::PushFont(ImGui::GetFont(), ImGui::GetFont()->LegacySize * logFontScale);
                if (has_color)
                    ImGui::PushStyleColor(ImGuiCol_Text, color);
                ImGui::TextUnformatted(item);
                if (has_color)
                    ImGui::PopStyleColor();
                ImGui::PopFont();
            }

            if (copy_to_clipboard)
                ImGui::LogFinish();
        }

        if (ScrollToBottom || (bAutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()))
            ImGui::SetScrollHereY(1.0f);
        ScrollToBottom = false;

        ImGui::PopStyleVar();
        ImGui::EndChild();
        ImGui::Separator();

        // Command-line
        bool reclaim_focus = false;
        ImGuiInputTextFlags input_text_flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackHistory;
        auto TextEditCallbackStub = [](ImGuiInputTextCallbackData* data) {
            OutputLog* console = (OutputLog*)data->UserData;
            return console->TextEditCallback(data);
        };

        ImGui::SetNextItemWidth(100.0f);

        static ImGuiComboFlags flags = 0;
        //    flags &= ~ImGuiComboFlags_NoPreview;    
        //    flags &= ~ImGuiComboFlags_NoArrowButton;

		const char* items[] = {
			EditorLayer::GetText("Command"),
			EditorLayer::GetText("Python")
		};
		const char* item_current = items[ConsoleMode];
        if (ImGui::BeginCombo("##Console Mode", item_current, flags)) // The second parameter is the label previewed before opening the combo.
        {
            for (int n = 0; n < IM_ARRAYSIZE(items); n++)
            {
                bool is_selected = (ConsoleMode == n);
                if (ImGui::Selectable(items[n], is_selected))
                    ConsoleMode = n;
                    item_current = items[ConsoleMode];
                if (is_selected)
                    ImGui::SetItemDefaultFocus();   // Set the initial focus when opening the combo (scrolling + for keyboard navigation support in the upcoming navigation branch)
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetColumnWidth());

		if (ImGui::InputTextWithHint("##ConsoleInput", EditorLayer::GetText("Enter Console Command"), InputBuf, IM_ARRAYSIZE(InputBuf), input_text_flags, TextEditCallbackStub, (void*)this))
        {
            char* s = InputBuf;
            Strtrim(s);
            if (s[0])
            {
                AddLog("# %s\n", s);

                switch (ConsoleMode)
                {
                case 0:
                {
                    // Command Mode
                    ExecCommand(s);
                    //task = std::thread(&OutputLog::ExecCommand, this, std::string(s));
                    //bWithTask = true;
                    break;
                }
                case 1:
                {
                    // Python Mode
                    ExecPython(s);
                    break;
                }
                default:
                    break;
                }
            }
            strcpy(s, "");
            reclaim_focus = true;
        }

        // Auto-focus on window apparition
        ImGui::SetItemDefaultFocus();
        if (reclaim_focus)
            ImGui::SetKeyboardFocusHere(-1); // Auto focus previous widget

        ImGui::End();
	}

    void OutputLog::ExecCommand(const char* command_line)
    {
        // Insert into history. First find match and delete it so it can be pushed to the back.
        // This isn't trying to be smart or optimal.
        HistoryPos = -1;
        for (int i = History.Size - 1; i >= 0; i--)
            if (Stricmp(History[i], command_line) == 0)
            {
                free(History[i]);
                History.erase(History.begin() + i);
                break;
            }
        History.push_back(Strdup(command_line));

        // Process command
        if (Stricmp(command_line, "CLEAR") == 0)
        {
            ClearLog();
        }
        else if (Stricmp(command_line, "HELP") == 0)
        {
            AddLog("%s:", EditorLayer::GetText("Commands"));
            for (int i = 0; i < Commands.Size; i++)
                AddLog("- %s", Commands[i]);
        }
        else if (Stricmp(command_line, "HISTORY") == 0)
        {
            int first = History.Size - 10;
            for (int i = first > 0 ? first : 0; i < History.Size; i++)
                AddLog("%3d: %s\n", i, History[i]);
        }
        else if (Stricmp(command_line, "小何") == 0)
        {
            AddLog("好帅！");
        }
        else
        {
            AddLog("%s: '%s'\n", EditorLayer::GetText("Unknown command"), command_line);
        }

        // On command input, we scroll to bottom even if AutoScroll==false
        ScrollToBottom = true;
    }

    void OutputLog::ExecPython(const char* command_line)
    {
        try
        {
            pybind11::exec(command_line);
        }
        catch (pybind11::error_already_set& e)
        {
            AddLog("[error] %s \n", e.what());
        }
    }

    int OutputLog::TextEditCallback(ImGuiInputTextCallbackData* data)
    {
        //AddLog("cursor: %d, selection: %d-%d", data->CursorPos, data->SelectionStart, data->SelectionEnd);
        switch (data->EventFlag)
        {
        case ImGuiInputTextFlags_CallbackCompletion:
        {
            // Example of TEXT COMPLETION

            // Locate beginning of current word
            const char* word_end = data->Buf + data->CursorPos;
            const char* word_start = word_end;
            while (word_start > data->Buf)
            {
                const char c = word_start[-1];
                if (c == ' ' || c == '\t' || c == ',' || c == ';')
                    break;
                word_start--;
            }

            // Build a list of candidates
            ImVector<const char*> candidates;
            for (int i = 0; i < Commands.Size; i++)
                if (Strnicmp(Commands[i], word_start, (int)(word_end - word_start)) == 0)
                    candidates.push_back(Commands[i]);

            if (candidates.Size == 0)
            {
                // No match
                AddLog("%s \"%.*s\"!\n", EditorLayer::GetText("No match for"), (int)(word_end - word_start), word_start);
            }
            else if (candidates.Size == 1)
            {
                // Single match. Delete the beginning of the word and replace it entirely so we've got nice casing.
                data->DeleteChars((int)(word_start - data->Buf), (int)(word_end - word_start));
                data->InsertChars(data->CursorPos, candidates[0]);
                data->InsertChars(data->CursorPos, " ");
            }
            else
            {
                // Multiple matches. Complete as much as we can..
                // So inputing "C"+Tab will complete to "CL" then display "CLEAR" and "CLASSIFY" as matches.
                int match_len = (int)(word_end - word_start);
                for (;;)
                {
                    int c = 0;
                    bool all_candidates_matches = true;
                    for (int i = 0; i < candidates.Size && all_candidates_matches; i++)
                        if (i == 0)
                            c = toupper(candidates[i][match_len]);
                        else if (c == 0 || c != toupper(candidates[i][match_len]))
                            all_candidates_matches = false;
                    if (!all_candidates_matches)
                        break;
                    match_len++;
                }

                if (match_len > 0)
                {
                    data->DeleteChars((int)(word_start - data->Buf), (int)(word_end - word_start));
                    data->InsertChars(data->CursorPos, candidates[0], candidates[0] + match_len);
                }

                // List matches
                AddLog("%s:\n", EditorLayer::GetText("Possible matches"));
                for (int i = 0; i < candidates.Size; i++)
                    AddLog("- %s\n", candidates[i]);
            }

            break;
        }
        case ImGuiInputTextFlags_CallbackHistory:
        {
            // Example of HISTORY
            const int prev_history_pos = HistoryPos;
            if (data->EventKey == ImGuiKey_UpArrow)
            {
                if (HistoryPos == -1)
                    HistoryPos = History.Size - 1;
                else if (HistoryPos > 0)
                    HistoryPos--;
            }
            else if (data->EventKey == ImGuiKey_DownArrow)
            {
                if (HistoryPos != -1)
                    if (++HistoryPos >= History.Size)
                        HistoryPos = -1;
            }

            // A better implementation would preserve the data on the current input line along with cursor position.
            if (prev_history_pos != HistoryPos)
            {
                const char* history_str = (HistoryPos >= 0) ? History[HistoryPos] : "";
                data->DeleteChars(0, data->BufTextLen);
                data->InsertChars(0, history_str);
            }
        }
        }
        return 0;
    }

    void OutputLog::ShutDown()
    {
    }
}
