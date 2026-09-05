#pragma once

#include "Runtime/DevTools/Commands/DevCommandTypes.h"

#include <string>
#include <vector>

namespace HEngine
{
	class GMPanelModel final
	{
	public:
		[[nodiscard]] static std::string BuildCommandList(
			const Json& discovery,
			std::vector<std::string>& outCommandNames);
		[[nodiscard]] static std::string BuildCommandForm(
			const DevCommandDescription& description);
		[[nodiscard]] static std::string BuildHistory(const Json& history);

	private:
		[[nodiscard]] static std::string Escape(std::string_view value);
		[[nodiscard]] static std::string ScalarText(const Json& value);
	};
}
