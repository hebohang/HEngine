#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace HEngine::Utils
{
	inline constexpr std::string_view CookedJsonMagicText = "HECJMPK1";
	inline constexpr std::array<std::uint8_t, 8> CookedJsonMagic = {
		'H', 'E', 'C', 'J', 'M', 'P', 'K', '1'
	};
}
