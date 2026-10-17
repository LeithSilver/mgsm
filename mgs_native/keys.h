#pragma once

#include <string>

// PC keys are Windows virtual-key codes on every platform: the ini bindings, the overlay, photo mode and the
// test scripts (MGS_KEY_SCRIPT) use them. SDL scancodes are translated with VKFromScancode. On Windows
// <windows.h> may already define the same names (with the same values); include it first if a file needs both.
#ifndef VK_BACK
#define VK_BACK      0x08
#define VK_TAB       0x09
#define VK_RETURN    0x0D
#define VK_SHIFT     0x10
#define VK_CONTROL   0x11
#define VK_MENU      0x12
#define VK_PAUSE     0x13
#define VK_CAPITAL   0x14
#define VK_ESCAPE    0x1B
#define VK_SPACE     0x20
#define VK_PRIOR     0x21
#define VK_NEXT      0x22
#define VK_END       0x23
#define VK_HOME      0x24
#define VK_LEFT      0x25
#define VK_UP        0x26
#define VK_RIGHT     0x27
#define VK_DOWN      0x28
#define VK_SNAPSHOT  0x2C
#define VK_INSERT    0x2D
#define VK_DELETE    0x2E
#define VK_LWIN      0x5B
#define VK_RWIN      0x5C
#define VK_APPS      0x5D
#define VK_NUMPAD0   0x60
#define VK_NUMPAD9   0x69
#define VK_MULTIPLY  0x6A
#define VK_ADD       0x6B
#define VK_SUBTRACT  0x6D
#define VK_DECIMAL   0x6E
#define VK_DIVIDE    0x6F
#define VK_F1        0x70
#define VK_F2        0x71
#define VK_F5        0x74
#define VK_F6        0x75
#define VK_F9        0x78
#define VK_F11       0x7A
#define VK_F12       0x7B
#define VK_NUMLOCK   0x90
#define VK_SCROLL    0x91
#define VK_OEM_1     0xBA   // ;
#define VK_OEM_PLUS  0xBB   // =
#define VK_OEM_COMMA 0xBC
#define VK_OEM_MINUS 0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2     0xBF   // /
#define VK_OEM_3     0xC0   // `
#define VK_OEM_4     0xDB   // [
#define VK_OEM_5     0xDC   // backslash
#define VK_OEM_6     0xDD   // ]
#define VK_OEM_7     0xDE   // '
#define VK_OEM_102   0xE2   // the extra key left of Z on ISO keyboards
#endif

// SDL_Scancode -> VK (0 = no equivalent). Position-based, as on a US layout.
int VKFromScancode(int sdl_scancode);
// Display name for the overlay ("Enter", "Num 5", "F1", "A", ...).
std::string VKName(int vk);
