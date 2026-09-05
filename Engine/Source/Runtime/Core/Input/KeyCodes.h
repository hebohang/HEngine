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

    // refer to https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes
    constexpr uint16_t HKeyCodeToWinCode(KeyCode code)
    {
        switch (code)
        {
        case HEngine::KeyCode::Space:			return VK_SPACE;
        case HEngine::KeyCode::Apostrophe:      return VK_OEM_7;
        case HEngine::KeyCode::Comma:           return VK_OEM_COMMA;
        case HEngine::KeyCode::Minus:           return VK_OEM_MINUS;
        case HEngine::KeyCode::Period:          return VK_OEM_2;
        case HEngine::KeyCode::Slash:           return 0x30;
        case HEngine::KeyCode::D0:              return 0x30;
        case HEngine::KeyCode::D1:              return 0x31;
        case HEngine::KeyCode::D2:              return 0x32;
        case HEngine::KeyCode::D3:              return 0x33;
        case HEngine::KeyCode::D4:              return 0x34;
        case HEngine::KeyCode::D5:              return 0x35;
        case HEngine::KeyCode::D6:              return 0x36;
        case HEngine::KeyCode::D7:              return 0x37;
        case HEngine::KeyCode::D8:              return 0x38;
        case HEngine::KeyCode::D9:              return 0x39;
        case HEngine::KeyCode::Semicolon:       return VK_OEM_1;
        case HEngine::KeyCode::Equal:           return VK_OEM_PLUS;
        case HEngine::KeyCode::A:               return 0x41;
        case HEngine::KeyCode::B:               return 0x42;
        case HEngine::KeyCode::C:               return 0x43;
        case HEngine::KeyCode::D:               return 0x44;
        case HEngine::KeyCode::E:               return 0x45;
        case HEngine::KeyCode::F:               return 0x46;
        case HEngine::KeyCode::G:               return 0x47;
        case HEngine::KeyCode::H:               return 0x48;
        case HEngine::KeyCode::I:               return 0x49;
        case HEngine::KeyCode::J:               return 0x4A;
        case HEngine::KeyCode::K:               return 0x4B;
        case HEngine::KeyCode::L:               return 0x4C;
        case HEngine::KeyCode::M:               return 0x4D;
        case HEngine::KeyCode::N:               return 0x4E;
        case HEngine::KeyCode::O:               return 0x4F;
        case HEngine::KeyCode::P:               return 0x50;
        case HEngine::KeyCode::Q:               return 0x51;
        case HEngine::KeyCode::R:               return 0x52;
        case HEngine::KeyCode::S:               return 0x53;
        case HEngine::KeyCode::T:               return 0x54;
        case HEngine::KeyCode::U:               return 0x55;
        case HEngine::KeyCode::V:               return 0x56;
        case HEngine::KeyCode::W:               return 0x57;
        case HEngine::KeyCode::X:               return 0x58;
        case HEngine::KeyCode::Y:               return 0x59;
        case HEngine::KeyCode::Z:               return 0x5A;
        case HEngine::KeyCode::LeftBracket:     return VK_OEM_4;
        case HEngine::KeyCode::Backslash:       return VK_OEM_5;
        case HEngine::KeyCode::RightBracket:    return VK_OEM_6;
        case HEngine::KeyCode::GraveAccent:     return VK_OEM_3;
        case HEngine::KeyCode::World1:          return VK_SPACE;
        case HEngine::KeyCode::World2:          return VK_SPACE;
        case HEngine::KeyCode::Escape:          return VK_SPACE;
        case HEngine::KeyCode::Enter:           return VK_RETURN;
        case HEngine::KeyCode::Tab:             return VK_TAB;
        case HEngine::KeyCode::Backspace:       return VK_BACK;
        case HEngine::KeyCode::Insert:          return VK_INSERT;
        case HEngine::KeyCode::Delete:          return VK_DELETE;
        case HEngine::KeyCode::Right:           return VK_RIGHT;
        case HEngine::KeyCode::Left:            return VK_LEFT;
        case HEngine::KeyCode::Down:            return VK_DOWN;
        case HEngine::KeyCode::Up:              return VK_UP;
        case HEngine::KeyCode::PageUp:          return VK_PRIOR;
        case HEngine::KeyCode::PageDown:        return VK_NEXT;
        case HEngine::KeyCode::Home:            return VK_HOME;
        case HEngine::KeyCode::End:             return VK_END;
        case HEngine::KeyCode::CapsLock:        return VK_CAPITAL;
        case HEngine::KeyCode::ScrollLock:      return VK_SCROLL;
        case HEngine::KeyCode::NumLock:         return VK_NUMLOCK;
        case HEngine::KeyCode::PrintScreen:     return VK_SNAPSHOT;
        case HEngine::KeyCode::Pause:           return VK_PAUSE;
        case HEngine::KeyCode::F1:              return VK_F1;
        case HEngine::KeyCode::F2:              return VK_F2;
        case HEngine::KeyCode::F3:              return VK_F3;
        case HEngine::KeyCode::F4:              return VK_F4;
        case HEngine::KeyCode::F5:              return VK_F5;
        case HEngine::KeyCode::F6:              return VK_F6;
        case HEngine::KeyCode::F7:              return VK_F7;
        case HEngine::KeyCode::F8:              return VK_F8;
        case HEngine::KeyCode::F9:              return VK_F9;
        case HEngine::KeyCode::F10:             return VK_F10;
        case HEngine::KeyCode::F11:             return VK_F11;
        case HEngine::KeyCode::F12:             return VK_F12;
        case HEngine::KeyCode::F13:             return VK_F13;
        case HEngine::KeyCode::F14:             return VK_F14;
        case HEngine::KeyCode::F15:             return VK_F15;
        case HEngine::KeyCode::F16:             return VK_F16;
        case HEngine::KeyCode::F17:             return VK_F17;
        case HEngine::KeyCode::F18:             return VK_F18;
        case HEngine::KeyCode::F19:             return VK_F19;
        case HEngine::KeyCode::F20:             return VK_F20;
        case HEngine::KeyCode::F21:             return VK_F21;
        case HEngine::KeyCode::F22:             return VK_F22;
        case HEngine::KeyCode::F23:             return VK_F23;
        case HEngine::KeyCode::F24:             return VK_F24;
        case HEngine::KeyCode::F25:             return 0x88;
        case HEngine::KeyCode::KP0:             return VK_NUMPAD0;
        case HEngine::KeyCode::KP1:             return VK_NUMPAD1;
        case HEngine::KeyCode::KP2:             return VK_NUMPAD2;
        case HEngine::KeyCode::KP3:             return VK_NUMPAD3;
        case HEngine::KeyCode::KP4:             return VK_NUMPAD4;
        case HEngine::KeyCode::KP5:             return VK_NUMPAD5;
        case HEngine::KeyCode::KP6:             return VK_NUMPAD6;
        case HEngine::KeyCode::KP7:             return VK_NUMPAD7;
        case HEngine::KeyCode::KP8:             return VK_NUMPAD8;
        case HEngine::KeyCode::KP9:             return VK_NUMPAD9;
        case HEngine::KeyCode::KPDecimal:       return VK_DECIMAL;
        case HEngine::KeyCode::KPDivide:        return VK_DIVIDE;
        case HEngine::KeyCode::KPMultiply:      return VK_MULTIPLY;
        case HEngine::KeyCode::KPSubtract:      return VK_SUBTRACT;
        case HEngine::KeyCode::KPAdd:           return VK_ADD;
        case HEngine::KeyCode::KPEnter:         return VK_SPACE;
        case HEngine::KeyCode::KPEqual:         return VK_SPACE;
        case HEngine::KeyCode::LeftShift:       return VK_LSHIFT;
        case HEngine::KeyCode::LeftControl:     return VK_LCONTROL;
        case HEngine::KeyCode::LeftAlt:         return VK_LMENU;
        case HEngine::KeyCode::LeftSuper:       return VK_SPACE;
        case HEngine::KeyCode::RightShift:      return VK_RSHIFT;
        case HEngine::KeyCode::RightControl:    return VK_RCONTROL;
        case HEngine::KeyCode::RightAlt:        return VK_RMENU;
        case HEngine::KeyCode::RightSuper:      return VK_SPACE;
        case HEngine::KeyCode::Menu:            return VK_MENU;
        default:
            HE_CORE_ERROR("Unknown Key!");
            return 0;
        }
        return 0;
    }

    constexpr KeyCode WinCodeToHKeyCode(uint16_t code)
    {
        switch (code)
        {
        case VK_SPACE:	             return HEngine::KeyCode::Space;
        case VK_OEM_7:               return HEngine::KeyCode::Apostrophe;
        case VK_OEM_COMMA:           return HEngine::KeyCode::Comma;
        case VK_OEM_MINUS:           return HEngine::KeyCode::Minus;
        case VK_OEM_2:               return HEngine::KeyCode::Period;
        case 0x30:                   return HEngine::KeyCode::D0;
        case 0x31:                   return HEngine::KeyCode::D1;         
        case 0x32:                   return HEngine::KeyCode::D2;         
        case 0x33:                   return HEngine::KeyCode::D3;         
        case 0x34:                   return HEngine::KeyCode::D4;         
        case 0x35:                   return HEngine::KeyCode::D5;         
        case 0x36:                   return HEngine::KeyCode::D6;         
        case 0x37:                   return HEngine::KeyCode::D7;         
        case 0x38:                   return HEngine::KeyCode::D8;         
        case 0x39:                   return HEngine::KeyCode::D9;         
        case VK_OEM_1:               return HEngine::KeyCode::Semicolon;
        case VK_OEM_PLUS:            return HEngine::KeyCode::Equal;
        case 0x41:                   return HEngine::KeyCode::A;
        case 0x42:                   return HEngine::KeyCode::B;
        case 0x43:                   return HEngine::KeyCode::C;
        case 0x44:                   return HEngine::KeyCode::D;
        case 0x45:                   return HEngine::KeyCode::E;
        case 0x46:                   return HEngine::KeyCode::F;
        case 0x47:                   return HEngine::KeyCode::G;
        case 0x48:                   return HEngine::KeyCode::H;
        case 0x49:                   return HEngine::KeyCode::I;
        case 0x4A:                   return HEngine::KeyCode::J;
        case 0x4B:                   return HEngine::KeyCode::K;
        case 0x4C:                   return HEngine::KeyCode::L;
        case 0x4D:                   return HEngine::KeyCode::M;
        case 0x4E:                   return HEngine::KeyCode::N;
        case 0x4F:                   return HEngine::KeyCode::O;
        case 0x50:                   return HEngine::KeyCode::P;
        case 0x51:                   return HEngine::KeyCode::Q;
        case 0x52:                   return HEngine::KeyCode::R;
        case 0x53:                   return HEngine::KeyCode::S;
        case 0x54:                   return HEngine::KeyCode::T;
        case 0x55:                   return HEngine::KeyCode::U;
        case 0x56:                   return HEngine::KeyCode::V;
        case 0x57:                   return HEngine::KeyCode::W;
        case 0x58:                   return HEngine::KeyCode::X;
        case 0x59:                   return HEngine::KeyCode::Y;
        case 0x5A:                   return HEngine::KeyCode::Z;
        case VK_OEM_4:               return HEngine::KeyCode::LeftBracket;
        case VK_OEM_5:               return HEngine::KeyCode::Backslash;
        case VK_OEM_6:               return HEngine::KeyCode::RightBracket;
        case VK_OEM_3:               return HEngine::KeyCode::GraveAccent;
        case VK_RETURN:              return HEngine::KeyCode::Enter;
        case VK_TAB:                 return HEngine::KeyCode::Tab;
        case VK_BACK:                return HEngine::KeyCode::Backspace;
        case VK_INSERT:              return HEngine::KeyCode::Insert;
        case VK_DELETE:              return HEngine::KeyCode::Delete;
        case VK_RIGHT:               return HEngine::KeyCode::Right;
        case VK_LEFT:                return HEngine::KeyCode::Left ;
        case VK_DOWN:                return HEngine::KeyCode::Down ;
        case VK_UP:                  return HEngine::KeyCode::Up;
        case VK_PRIOR:               return HEngine::KeyCode::PageUp;
        case VK_NEXT:                return HEngine::KeyCode::PageDown;
        case VK_HOME:                return HEngine::KeyCode::Home;
        case VK_END:                 return HEngine::KeyCode::End;
        case VK_CAPITAL:             return HEngine::KeyCode::CapsLock;
        case VK_SCROLL:              return HEngine::KeyCode::ScrollLock;
        case VK_NUMLOCK:             return HEngine::KeyCode::NumLock;
        case VK_SNAPSHOT:            return HEngine::KeyCode::PrintScreen;
        case VK_PAUSE:               return HEngine::KeyCode::Pause;
        case VK_F1:                  return HEngine::KeyCode::F1 ;
        case VK_F2:                  return HEngine::KeyCode::F2 ;
        case VK_F3:                  return HEngine::KeyCode::F3 ;
        case VK_F4:                  return HEngine::KeyCode::F4 ;
        case VK_F5:                  return HEngine::KeyCode::F5 ;
        case VK_F6:                  return HEngine::KeyCode::F6 ;
        case VK_F7:                  return HEngine::KeyCode::F7 ;
        case VK_F8:                  return HEngine::KeyCode::F8 ;
        case VK_F9:                  return HEngine::KeyCode::F9 ;
        case VK_F10:                 return HEngine::KeyCode::F10;
        case VK_F11:                 return HEngine::KeyCode::F11;
        case VK_F12:                 return HEngine::KeyCode::F12;
        case VK_F13:                 return HEngine::KeyCode::F13;
        case VK_F14:                 return HEngine::KeyCode::F14;
        case VK_F15:                 return HEngine::KeyCode::F15;
        case VK_F16:                 return HEngine::KeyCode::F16;
        case VK_F17:                 return HEngine::KeyCode::F17;
        case VK_F18:                 return HEngine::KeyCode::F18;
        case VK_F19:                 return HEngine::KeyCode::F19;
        case VK_F20:                 return HEngine::KeyCode::F20;
        case VK_F21:                 return HEngine::KeyCode::F21;
        case VK_F22:                 return HEngine::KeyCode::F22;
        case VK_F23:                 return HEngine::KeyCode::F23;
        case VK_F24:                 return HEngine::KeyCode::F24;
        case 0x88:                   return HEngine::KeyCode::F25;
        case VK_NUMPAD0:             return HEngine::KeyCode::KP0;
        case VK_NUMPAD1:             return HEngine::KeyCode::KP1;
        case VK_NUMPAD2:             return HEngine::KeyCode::KP2;
        case VK_NUMPAD3:             return HEngine::KeyCode::KP3;
        case VK_NUMPAD4:             return HEngine::KeyCode::KP4;
        case VK_NUMPAD5:             return HEngine::KeyCode::KP5;
        case VK_NUMPAD6:             return HEngine::KeyCode::KP6;
        case VK_NUMPAD7:             return HEngine::KeyCode::KP7;
        case VK_NUMPAD8:             return HEngine::KeyCode::KP8;
        case VK_NUMPAD9:             return HEngine::KeyCode::KP9;
        case VK_DECIMAL:             return HEngine::KeyCode::KPDecimal;
        case VK_DIVIDE:              return HEngine::KeyCode::KPDivide;
        case VK_MULTIPLY:            return HEngine::KeyCode::KPMultiply;
        case VK_SUBTRACT:            return HEngine::KeyCode::KPSubtract;
        case VK_ADD:                 return HEngine::KeyCode::KPAdd;
        case VK_LSHIFT:              return HEngine::KeyCode::LeftShift;
        case VK_LCONTROL:            return HEngine::KeyCode::LeftControl;
        case VK_LMENU:               return HEngine::KeyCode::LeftAlt;
        case VK_RSHIFT:              return HEngine::KeyCode::RightShift;
        case VK_RCONTROL:            return HEngine::KeyCode::RightControl;
        case VK_RMENU:               return HEngine::KeyCode::RightAlt;
        case VK_MENU:                return HEngine::KeyCode::Menu;
        default:
            HE_CORE_ERROR("Unknown Key!");
            return HEngine::KeyCode::Space;
        }
        return HEngine::KeyCode::Space;
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