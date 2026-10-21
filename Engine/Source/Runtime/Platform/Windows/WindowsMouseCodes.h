#pragma once

#include "Runtime/Core/Input/MouseCodes.h"
#include <Windows.h>

namespace HEngine
{
	// refer to https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes
	constexpr uint16_t HMouseCodeToWin(MouseCode code)
	{
		switch (code)
		{
		case HEngine::MouseCode::Button0:    return VK_LBUTTON;
		case HEngine::MouseCode::Button1:    return VK_RBUTTON;
		case HEngine::MouseCode::Button2:    return VK_MBUTTON;
		case HEngine::MouseCode::Button3:    return VK_XBUTTON1;
		case HEngine::MouseCode::Button4:    return VK_XBUTTON2;
		case HEngine::MouseCode::Button5:    return 0x07;
		case HEngine::MouseCode::Button6:    return 0x07;
		case HEngine::MouseCode::Button7:    return 0x07;
		default:
			break;
		}
		return 0;
	}
}
