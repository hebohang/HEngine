#pragma once

#include "BasePanel.h"

#include <imgui/imgui.h>

// System includes
#include <ctype.h>          // toupper
#include <limits.h>         // INT_MIN, INT_MAX
#include <math.h>           // sqrtf, powf, cosf, sinf, floorf, ceilf
#include <stdio.h>          // vsnprintf, sscanf, printf
#include <stdlib.h>         // NULL, malloc, free, atoi
#if defined(_MSC_VER) && _MSC_VER <= 1500 // MSVC 2008 or earlier
#include <stddef.h>         // intptr_t
#else
#include <stdint.h>         // intptr_t
#endif

#include <pybind11/embed.h>

namespace HEngine
{
	// from imgui_demo.cpp ExampleAppConsole
	class OutputLogPanel : public BasePanel
	{
		REGISTER_PANEL(OutputLogPanel)
	public:
		OutputLogPanel(EditorLayer* parent, std::string panelName, bool* pOpen);
		virtual ~OutputLogPanel();

		void ClearLog();

		void OnImGuiRenderImpl();

		void AddLog(const char* fmt, ...) IM_FMTARGS(2);

		void ExecCommand(const char* command_line);
		void ExecPython(const char* command_line);
		int TextEditCallback(ImGuiInputTextCallbackData* data);

		void ShutDown();
	private:
		char                  InputBuf[256];
		ImVector<char*>       Items;
		ImVector<const char*> Commands;
		ImVector<char*>       History;
		int                   HistoryPos;    // -1: new line, 0..History.Size-1 browsing history.
		ImGuiTextFilter       Filter;
		std::string           mVisibleLogTextCache;
		bool                  bVisibleLogTextCacheDirty = true;
		bool                  ScrollToBottom;
		int					  ConsoleMode = 0;

		// Portable helpers
		static int   Stricmp(const char* s1, const char* s2) { int d; while ((d = toupper(*s2) - toupper(*s1)) == 0 && *s1) { s1++; s2++; } return d; }
		static int   Strnicmp(const char* s1, const char* s2, int n) { int d = 0; while (n > 0 && (d = toupper(*s2) - toupper(*s1)) == 0 && *s1) { s1++; s2++; n--; } return d; }
		static char* Strdup(const char* s) { IM_ASSERT(s); size_t len = strlen(s) + 1; void* buf = malloc(len); IM_ASSERT(buf); return (char*)memcpy(buf, (const void*)s, len); }
		static void  Strtrim(char* s) { char* str_end = s + strlen(s); while (str_end > s && str_end[-1] == ' ') str_end--; *str_end = 0; }
	};
}
