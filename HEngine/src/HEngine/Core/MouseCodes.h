#pragma once

namespace HEngine
{
	typedef enum class MouseCode : uint16_t
	{
		// From glfw3.h
		Button0 = 0,
		Button1 = 1,
		Button2 = 2,
		Button3 = 3,
		Button4 = 4,
		Button5 = 5,
		Button6 = 6,
		Button7 = 7,

		ButtonLast = Button7,
		ButtonLeft = Button0,
		ButtonRight = Button1,
		ButtonMiddle = Button2
	} Mouse;

	inline std::ostream& operator<<(std::ostream& os, MouseCode mouseCode)
	{
		os << static_cast<int32_t>(mouseCode);
		return os;
	}
}

#define HE_MOUSE_BUTTON_0      ::HEngine::Mouse::Button0
#define HE_MOUSE_BUTTON_1      ::HEngine::Mouse::Button1
#define HE_MOUSE_BUTTON_2      ::HEngine::Mouse::Button2
#define HE_MOUSE_BUTTON_3      ::HEngine::Mouse::Button3
#define HE_MOUSE_BUTTON_4      ::HEngine::Mouse::Button4
#define HE_MOUSE_BUTTON_5      ::HEngine::Mouse::Button5
#define HE_MOUSE_BUTTON_6      ::HEngine::Mouse::Button6
#define HE_MOUSE_BUTTON_7      ::HEngine::Mouse::Button7
#define HE_MOUSE_BUTTON_LAST   ::HEngine::Mouse::ButtonLast
#define HE_MOUSE_BUTTON_LEFT   ::HEngine::Mouse::ButtonLeft
#define HE_MOUSE_BUTTON_RIGHT  ::HEngine::Mouse::ButtonRight
#define HE_MOUSE_BUTTON_MIDDLE ::HEngine::Mouse::ButtonMiddle