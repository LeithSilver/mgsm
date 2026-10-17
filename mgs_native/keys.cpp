#include "keys.h"

#include <SDL3/SDL_scancode.h>
#include <cstdio>

int VKFromScancode(int sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return 'A' + (sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return '1' + (sc - SDL_SCANCODE_1);
    if (sc == SDL_SCANCODE_0) return '0';
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return 0x70 + (sc - SDL_SCANCODE_F1);
    if (sc >= SDL_SCANCODE_F13 && sc <= SDL_SCANCODE_F24) return 0x7C + (sc - SDL_SCANCODE_F13);
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return VK_NUMPAD0 + 1 + (sc - SDL_SCANCODE_KP_1);
    switch (sc) {
        case SDL_SCANCODE_KP_0: return VK_NUMPAD0;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return VK_RETURN;   // Windows reports both as Enter
        case SDL_SCANCODE_ESCAPE: return VK_ESCAPE;
        case SDL_SCANCODE_BACKSPACE: return VK_BACK;
        case SDL_SCANCODE_TAB: return VK_TAB;
        case SDL_SCANCODE_SPACE: return VK_SPACE;
        case SDL_SCANCODE_MINUS: return VK_OEM_MINUS;
        case SDL_SCANCODE_EQUALS: return VK_OEM_PLUS;
        case SDL_SCANCODE_LEFTBRACKET: return VK_OEM_4;
        case SDL_SCANCODE_RIGHTBRACKET: return VK_OEM_6;
        case SDL_SCANCODE_BACKSLASH: return VK_OEM_5;
        case SDL_SCANCODE_SEMICOLON: return VK_OEM_1;
        case SDL_SCANCODE_APOSTROPHE: return VK_OEM_7;
        case SDL_SCANCODE_GRAVE: return VK_OEM_3;
        case SDL_SCANCODE_COMMA: return VK_OEM_COMMA;
        case SDL_SCANCODE_PERIOD: return VK_OEM_PERIOD;
        case SDL_SCANCODE_SLASH: return VK_OEM_2;
        case SDL_SCANCODE_NONUSBACKSLASH: return VK_OEM_102;
        case SDL_SCANCODE_CAPSLOCK: return VK_CAPITAL;
        case SDL_SCANCODE_PRINTSCREEN: return VK_SNAPSHOT;
        case SDL_SCANCODE_SCROLLLOCK: return VK_SCROLL;
        case SDL_SCANCODE_PAUSE: return VK_PAUSE;
        case SDL_SCANCODE_INSERT: return VK_INSERT;
        case SDL_SCANCODE_HOME: return VK_HOME;
        case SDL_SCANCODE_PAGEUP: return VK_PRIOR;
        case SDL_SCANCODE_DELETE: return VK_DELETE;
        case SDL_SCANCODE_END: return VK_END;
        case SDL_SCANCODE_PAGEDOWN: return VK_NEXT;
        case SDL_SCANCODE_RIGHT: return VK_RIGHT;
        case SDL_SCANCODE_LEFT: return VK_LEFT;
        case SDL_SCANCODE_DOWN: return VK_DOWN;
        case SDL_SCANCODE_UP: return VK_UP;
        case SDL_SCANCODE_NUMLOCKCLEAR: return VK_NUMLOCK;
        case SDL_SCANCODE_KP_DIVIDE: return VK_DIVIDE;
        case SDL_SCANCODE_KP_MULTIPLY: return VK_MULTIPLY;
        case SDL_SCANCODE_KP_MINUS: return VK_SUBTRACT;
        case SDL_SCANCODE_KP_PLUS: return VK_ADD;
        case SDL_SCANCODE_KP_PERIOD: return VK_DECIMAL;
        case SDL_SCANCODE_APPLICATION: return VK_APPS;
        case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return VK_CONTROL;     // Windows keyboard messages don't
        case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return VK_SHIFT;     // tell left and right apart either
        case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return VK_MENU;
        case SDL_SCANCODE_LGUI: return VK_LWIN;
        case SDL_SCANCODE_RGUI: return VK_RWIN;
        default: return 0;
    }
}

std::string VKName(int vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, static_cast<char>(vk));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "Num " + std::to_string(vk - VK_NUMPAD0);
    if (vk >= 0x70 && vk <= 0x87) return "F" + std::to_string(vk - 0x70 + 1);
    switch (vk) {
        case VK_BACK: return "Backspace"; case VK_TAB: return "Tab"; case VK_RETURN: return "Enter";
        case VK_SHIFT: return "Shift"; case VK_CONTROL: return "Ctrl"; case VK_MENU: return "Alt";
        case VK_PAUSE: return "Pause"; case VK_CAPITAL: return "Caps Lock"; case VK_ESCAPE: return "Esc";
        case VK_SPACE: return "Space"; case VK_PRIOR: return "Page Up"; case VK_NEXT: return "Page Down";
        case VK_END: return "End"; case VK_HOME: return "Home"; case VK_LEFT: return "Left"; case VK_UP: return "Up";
        case VK_RIGHT: return "Right"; case VK_DOWN: return "Down"; case VK_SNAPSHOT: return "Print Screen";
        case VK_INSERT: return "Insert"; case VK_DELETE: return "Delete"; case VK_LWIN: return "Left Win";
        case VK_RWIN: return "Right Win"; case VK_APPS: return "Menu";
        case VK_MULTIPLY: return "Num *"; case VK_ADD: return "Num +"; case VK_SUBTRACT: return "Num -";
        case VK_DECIMAL: return "Num ."; case VK_DIVIDE: return "Num /"; case VK_NUMLOCK: return "Num Lock";
        case VK_SCROLL: return "Scroll Lock";
        case VK_OEM_1: return ";"; case VK_OEM_PLUS: return "="; case VK_OEM_COMMA: return ",";
        case VK_OEM_MINUS: return "-"; case VK_OEM_PERIOD: return "."; case VK_OEM_2: return "/";
        case VK_OEM_3: return "`"; case VK_OEM_4: return "["; case VK_OEM_5: return "\\"; case VK_OEM_6: return "]";
        case VK_OEM_7: return "'"; case VK_OEM_102: return "\\ (ISO)";
        default: break;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "VK 0x%02X", vk);
    return buf;
}
