#pragma once

#include "Runtime/Core/Input/KeyCodes.h"
#include "Runtime/Core/Log/Log.h"

#include <Windows.h>

namespace HEngine
{
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
