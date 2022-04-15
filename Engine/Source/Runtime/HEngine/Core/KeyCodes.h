#pragma once

namespace HEngine
{
	typedef enum class KeyCode : uint16_t
	{
		// From glfw3.h
		Space = 32,
		Apostrophe = 39, /* ' */
		Comma = 44, /* , */
		Minus = 45, /* - */
		Period = 46, /* . */
		Slash = 47, /* / */

		D0 = 48, /* 0 */
		D1 = 49, /* 1 */
		D2 = 50, /* 2 */
		D3 = 51, /* 3 */
		D4 = 52, /* 4 */
		D5 = 53, /* 5 */
		D6 = 54, /* 6 */
		D7 = 55, /* 7 */
		D8 = 56, /* 8 */
		D9 = 57, /* 9 */

		Semicolon = 59, /* ; */
		Equal = 61, /* = */

		A = 65,
		B = 66,
		C = 67,
		D = 68,
		E = 69,
		F = 70,
		G = 71,
		H = 72,
		I = 73,
		J = 74,
		K = 75,
		L = 76,
		M = 77,
		N = 78,
		O = 79,
		P = 80,
		Q = 81,
		R = 82,
		S = 83,
		T = 84,
		U = 85,
		V = 86,
		W = 87,
		X = 88,
		Y = 89,
		Z = 90,

		LeftBracket = 91,  /* [ */
		Backslash = 92,  /* \ */
		RightBracket = 93,  /* ] */
		GraveAccent = 96,  /* ` */

		World1 = 161, /* non-US #1 */
		World2 = 162, /* non-US #2 */

		/* Function keys */
		Escape = 256,
		Enter = 257,
		Tab = 258,
		Backspace = 259,
		Insert = 260,
		Delete = 261,
		Right = 262,
		Left = 263,
		Down = 264,
		Up = 265,
		PageUp = 266,
		PageDown = 267,
		Home = 268,
		End = 269,
		CapsLock = 280,
		ScrollLock = 281,
		NumLock = 282,
		PrintScreen = 283,
		Pause = 284,
		F1 = 290,
		F2 = 291,
		F3 = 292,
		F4 = 293,
		F5 = 294,
		F6 = 295,
		F7 = 296,
		F8 = 297,
		F9 = 298,
		F10 = 299,
		F11 = 300,
		F12 = 301,
		F13 = 302,
		F14 = 303,
		F15 = 304,
		F16 = 305,
		F17 = 306,
		F18 = 307,
		F19 = 308,
		F20 = 309,
		F21 = 310,
		F22 = 311,
		F23 = 312,
		F24 = 313,
		F25 = 314,

		/* Keypad */
		KP0 = 320,
		KP1 = 321,
		KP2 = 322,
		KP3 = 323,
		KP4 = 324,
		KP5 = 325,
		KP6 = 326,
		KP7 = 327,
		KP8 = 328,
		KP9 = 329,
		KPDecimal = 330,
		KPDivide = 331,
		KPMultiply = 332,
		KPSubtract = 333,
		KPAdd = 334,
		KPEnter = 335,
		KPEqual = 336,

		LeftShift = 340,
		LeftControl = 341,
		LeftAlt = 342,
		LeftSuper = 343,
		RightShift = 344,
		RightControl = 345,
		RightAlt = 346,
		RightSuper = 347,
		Menu = 348
	} Key;

	inline std::ostream& operator<<(std::ostream& os, KeyCode keyCode)
	{
		os << static_cast<int32_t>(keyCode);
		return os;
	}
}

// From glfw3.h
#define HE_KEY_SPACE           ::HEngine::Key::Space
#define HE_KEY_APOSTROPHE      ::HEngine::Key::Apostrophe    /* ' */
#define HE_KEY_COMMA           ::HEngine::Key::Comma         /* , */
#define HE_KEY_MINUS           ::HEngine::Key::Minus         /* - */
#define HE_KEY_PERIOD          ::HEngine::Key::Period        /* . */
#define HE_KEY_SLASH           ::HEngine::Key::Slash         /* / */
#define HE_KEY_0               ::HEngine::Key::D0
#define HE_KEY_1               ::HEngine::Key::D1
#define HE_KEY_2               ::HEngine::Key::D2
#define HE_KEY_3               ::HEngine::Key::D3
#define HE_KEY_4               ::HEngine::Key::D4
#define HE_KEY_5               ::HEngine::Key::D5
#define HE_KEY_6               ::HEngine::Key::D6
#define HE_KEY_7               ::HEngine::Key::D7
#define HE_KEY_8               ::HEngine::Key::D8
#define HE_KEY_9               ::HEngine::Key::D9
#define HE_KEY_SEMICOLON       ::HEngine::Key::Semicolon     /* ; */
#define HE_KEY_EQUAL           ::HEngine::Key::Equal         /* = */
#define HE_KEY_A               ::HEngine::Key::A
#define HE_KEY_B               ::HEngine::Key::B
#define HE_KEY_C               ::HEngine::Key::C
#define HE_KEY_D               ::HEngine::Key::D
#define HE_KEY_E               ::HEngine::Key::E
#define HE_KEY_F               ::HEngine::Key::F
#define HE_KEY_G               ::HEngine::Key::G
#define HE_KEY_H               ::HEngine::Key::H
#define HE_KEY_I               ::HEngine::Key::I
#define HE_KEY_J               ::HEngine::Key::J
#define HE_KEY_K               ::HEngine::Key::K
#define HE_KEY_L               ::HEngine::Key::L
#define HE_KEY_M               ::HEngine::Key::M
#define HE_KEY_N               ::HEngine::Key::N
#define HE_KEY_O               ::HEngine::Key::O
#define HE_KEY_P               ::HEngine::Key::P
#define HE_KEY_Q               ::HEngine::Key::Q
#define HE_KEY_R               ::HEngine::Key::R
#define HE_KEY_S               ::HEngine::Key::S
#define HE_KEY_T               ::HEngine::Key::T
#define HE_KEY_U               ::HEngine::Key::U
#define HE_KEY_V               ::HEngine::Key::V
#define HE_KEY_W               ::HEngine::Key::W
#define HE_KEY_X               ::HEngine::Key::X
#define HE_KEY_Y               ::HEngine::Key::Y
#define HE_KEY_Z               ::HEngine::Key::Z
#define HE_KEY_LEFT_BRACKET    ::HEngine::Key::LeftBracket   /* [ */
#define HE_KEY_BACKSLASH       ::HEngine::Key::Backslash     /* \ */
#define HE_KEY_RIGHT_BRACKET   ::HEngine::Key::RightBracket  /* ] */
#define HE_KEY_GRAVE_ACCENT    ::HEngine::Key::GraveAccent   /* ` */
#define HE_KEY_WORLD_1         ::HEngine::Key::World1        /* non-US #1 */
#define HE_KEY_WORLD_2         ::HEngine::Key::World2        /* non-US #2 */

/* Function keys */
#define HE_KEY_ESCAPE          ::HEngine::Key::Escape
#define HE_KEY_ENTER           ::HEngine::Key::Enter
#define HE_KEY_TAB             ::HEngine::Key::Tab
#define HE_KEY_BACKSPACE       ::HEngine::Key::Backspace
#define HE_KEY_INSERT          ::HEngine::Key::Insert
#define HE_KEY_DELETE          ::HEngine::Key::Delete
#define HE_KEY_RIGHT           ::HEngine::Key::Right
#define HE_KEY_LEFT            ::HEngine::Key::Left
#define HE_KEY_DOWN            ::HEngine::Key::Down
#define HE_KEY_UP              ::HEngine::Key::Up
#define HE_KEY_PAGE_UP         ::HEngine::Key::PageUp
#define HE_KEY_PAGE_DOWN       ::HEngine::Key::PageDown
#define HE_KEY_HOME            ::HEngine::Key::Home
#define HE_KEY_END             ::HEngine::Key::End
#define HE_KEY_CAPS_LOCK       ::HEngine::Key::CapsLock
#define HE_KEY_SCROLL_LOCK     ::HEngine::Key::ScrollLock
#define HE_KEY_NUM_LOCK        ::HEngine::Key::NumLock
#define HE_KEY_PRINT_SCREEN    ::HEngine::Key::PrintScreen
#define HE_KEY_PAUSE           ::HEngine::Key::Pause
#define HE_KEY_F1              ::HEngine::Key::F1
#define HE_KEY_F2              ::HEngine::Key::F2
#define HE_KEY_F3              ::HEngine::Key::F3
#define HE_KEY_F4              ::HEngine::Key::F4
#define HE_KEY_F5              ::HEngine::Key::F5
#define HE_KEY_F6              ::HEngine::Key::F6
#define HE_KEY_F7              ::HEngine::Key::F7
#define HE_KEY_F8              ::HEngine::Key::F8
#define HE_KEY_F9              ::HEngine::Key::F9
#define HE_KEY_F10             ::HEngine::Key::F10
#define HE_KEY_F11             ::HEngine::Key::F11
#define HE_KEY_F12             ::HEngine::Key::F12
#define HE_KEY_F13             ::HEngine::Key::F13
#define HE_KEY_F14             ::HEngine::Key::F14
#define HE_KEY_F15             ::HEngine::Key::F15
#define HE_KEY_F16             ::HEngine::Key::F16
#define HE_KEY_F17             ::HEngine::Key::F17
#define HE_KEY_F18             ::HEngine::Key::F18
#define HE_KEY_F19             ::HEngine::Key::F19
#define HE_KEY_F20             ::HEngine::Key::F20
#define HE_KEY_F21             ::HEngine::Key::F21
#define HE_KEY_F22             ::HEngine::Key::F22
#define HE_KEY_F23             ::HEngine::Key::F23
#define HE_KEY_F24             ::HEngine::Key::F24
#define HE_KEY_F25             ::HEngine::Key::F25

/* Keypad */
#define HE_KEY_KP_0            ::HEngine::Key::KP0
#define HE_KEY_KP_1            ::HEngine::Key::KP1
#define HE_KEY_KP_2            ::HEngine::Key::KP2
#define HE_KEY_KP_3            ::HEngine::Key::KP3
#define HE_KEY_KP_4            ::HEngine::Key::KP4
#define HE_KEY_KP_5            ::HEngine::Key::KP5
#define HE_KEY_KP_6            ::HEngine::Key::KP6
#define HE_KEY_KP_7            ::HEngine::Key::KP7
#define HE_KEY_KP_8            ::HEngine::Key::KP8
#define HE_KEY_KP_9            ::HEngine::Key::KP9
#define HE_KEY_KP_DECIMAL      ::HEngine::Key::KPDecimal
#define HE_KEY_KP_DIVIDE       ::HEngine::Key::KPDivide
#define HE_KEY_KP_MULTIPLY     ::HEngine::Key::KPMultiply
#define HE_KEY_KP_SUBTRACT     ::HEngine::Key::KPSubtract
#define HE_KEY_KP_ADD          ::HEngine::Key::KPAdd
#define HE_KEY_KP_ENTER        ::HEngine::Key::KPEnter
#define HE_KEY_KP_EQUAL        ::HEngine::Key::KPEqual

#define HE_KEY_LEFT_SHIFT      ::HEngine::Key::LeftShift
#define HE_KEY_LEFT_CONTROL    ::HEngine::Key::LeftControl
#define HE_KEY_LEFT_ALT        ::HEngine::Key::LeftAlt
#define HE_KEY_LEFT_SUPER      ::HEngine::Key::LeftSuper
#define HE_KEY_RIGHT_SHIFT     ::HEngine::Key::RightShift
#define HE_KEY_RIGHT_CONTROL   ::HEngine::Key::RightControl
#define HE_KEY_RIGHT_ALT       ::HEngine::Key::RightAlt
#define HE_KEY_RIGHT_SUPER     ::HEngine::Key::RightSuper
#define HE_KEY_MENU            ::HEngine::Key::Menu