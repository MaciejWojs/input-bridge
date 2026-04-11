#pragma once

#include <cstdint>

// Flagi maskujące platformowe 'RAW' sygnały wejściowe kodów klawiszy (32-bit int)
constexpr int32_t FLAG_RAW_WINDOWS = 0x10000000;
constexpr int32_t FLAG_RAW_LINUX   = 0x20000000;
constexpr int32_t FLAG_RAW_MASK    = 0xF0000000;

class KeyTranslator {
public:
    // Konwertuje kody Windows Virtual Key (VK) na kody Linux Evdev (Wayland/X11)
    static int32_t WindowsToLinux(int32_t vk) {
        switch (vk) {
            // Litery A-Z
            case 0x41: return 30; // A
            case 0x42: return 48; // B
            case 0x43: return 46; // C
            case 0x44: return 32; // D
            case 0x45: return 18; // E
            case 0x46: return 33; // F
            case 0x47: return 34; // G
            case 0x48: return 35; // H
            case 0x49: return 23; // I
            case 0x4A: return 36; // J
            case 0x4B: return 37; // K
            case 0x4C: return 38; // L
            case 0x4D: return 50; // M
            case 0x4E: return 49; // N
            case 0x4F: return 24; // O
            case 0x50: return 25; // P
            case 0x51: return 16; // Q
            case 0x52: return 19; // R
            case 0x53: return 31; // S
            case 0x54: return 20; // T
            case 0x55: return 22; // U
            case 0x56: return 47; // V
            case 0x57: return 17; // W
            case 0x58: return 45; // X
            case 0x59: return 21; // Y
            case 0x5A: return 44; // Z
            
            // Cyfry 0-9
            case 0x30: return 11; // 0
            case 0x31: return 2;  // 1
            case 0x32: return 3;  // 2
            case 0x33: return 4;  // 3
            case 0x34: return 5;  // 4
            case 0x35: return 6;  // 5
            case 0x36: return 7;  // 6
            case 0x37: return 8;  // 7
            case 0x38: return 9;  // 8
            case 0x39: return 10; // 9

            // Funkcyjne F1-F12
            case 0x70: return 59; // F1
            case 0x71: return 60; // F2
            case 0x72: return 61; // F3
            case 0x73: return 62; // F4
            case 0x74: return 63; // F5
            case 0x75: return 64; // F6
            case 0x76: return 65; // F7
            case 0x77: return 66; // F8
            case 0x78: return 67; // F9
            case 0x79: return 68; // F10
            case 0x7A: return 87; // F11
            case 0x7B: return 88; // F12

            // Numpad
            case 0x60: return 82; // Numpad 0
            case 0x61: return 79; // Numpad 1
            case 0x62: return 80; // Numpad 2
            case 0x63: return 81; // Numpad 3
            case 0x64: return 75; // Numpad 4
            case 0x65: return 76; // Numpad 5
            case 0x66: return 77; // Numpad 6
            case 0x67: return 71; // Numpad 7
            case 0x68: return 72; // Numpad 8
            case 0x69: return 73; // Numpad 9
            case 0x6A: return 55; // Numpad *
            case 0x6B: return 78; // Numpad +
            case 0x6D: return 74; // Numpad -
            case 0x6E: return 83; // Numpad .
            case 0x6F: return 98; // Numpad /
            case 0x90: return 69; // Num Lock

            // Znaki punktowe i specjalne (US QWERTY Layout)
            case 0xBA: return 39; // ; :
            case 0xBB: return 13; // = +
            case 0xBC: return 51; // , <
            case 0xBD: return 12; // - _
            case 0xBE: return 52; // . >
            case 0xBF: return 53; // / ?
            case 0xC0: return 41; // ` ~
            case 0xDB: return 26; // [ {
            case 0xDC: return 43; // \ |
            case 0xDD: return 27; // ] }
            case 0xDE: return 40; // ' "

            // Modyfikatory
            case 0x10: 
            case 0xA0: return 42; // LSHIFT
            case 0xA1: return 54; // RSHIFT
            case 0x11: 
            case 0xA2: return 29; // LCTRL
            case 0xA3: return 97; // RCTRL
            case 0x12: 
            case 0xA4: return 56; // LALT
            case 0xA5: return 100; // RALT
            case 0x5B: return 125; // LWIN / LMETA
            case 0x5C: return 126; // RWIN / RMETA

            // Nawigacyjne
            case 0x24: return 102; // HOME
            case 0x23: return 107; // END
            case 0x21: return 104; // PAGEUP
            case 0x22: return 109; // PAGEDOWN
            case 0x2D: return 110; // INSERT
            case 0x2E: return 111; // DELETE

            // Znaki kontrolne/Specjalne
            case 0x0D: return 28; // ENTER
            case 0x1B: return 1;  // ESCAPE
            case 0x08: return 14; // BACKSPACE
            case 0x09: return 15; // TAB
            case 0x20: return 57; // SPACE
            case 0x14: return 58; // CAPS LOCK

            // Strzałki
            case 0x25: return 105; // LEFT
            case 0x26: return 103; // UP
            case 0x27: return 106; // RIGHT
            case 0x28: return 108; // DOWN

            // Media
            case 0xAD: return 113; // VOLUME MUTE
            case 0xAE: return 114; // VOLUME DOWN
            case 0xAF: return 115; // VOLUME UP
            case 0xB3: return 164; // PLAY/PAUSE
            case 0xB0: return 163; // NEXT TRACK
            case 0xB1: return 165; // PREV TRACK

            default: return 0; // Nieznany kod klawisza
        }
    }

    // Konwertuje kody Linux Evdev (Wayland/X11) na Windows Virtual Key (VK)
    static int32_t LinuxToWindows(int32_t evdev) {
        switch (evdev) {
            // Litery A-Z
            case 30: return 0x41; // A
            case 48: return 0x42; // B
            case 46: return 0x43; // C
            case 32: return 0x44; // D
            case 18: return 0x45; // E
            case 33: return 0x46; // F
            case 34: return 0x47; // G
            case 35: return 0x48; // H
            case 23: return 0x49; // I
            case 36: return 0x4A; // J
            case 37: return 0x4B; // K
            case 38: return 0x4C; // L
            case 50: return 0x4D; // M
            case 49: return 0x4E; // N
            case 24: return 0x4F; // O
            case 25: return 0x50; // P
            case 16: return 0x51; // Q
            case 19: return 0x52; // R
            case 31: return 0x53; // S
            case 20: return 0x54; // T
            case 22: return 0x55; // U
            case 47: return 0x56; // V
            case 17: return 0x57; // W
            case 45: return 0x58; // X
            case 21: return 0x59; // Y
            case 44: return 0x5A; // Z
            
            // Cyfry
            case 11: return 0x30; // 0
            case 2:  return 0x31; // 1
            case 3:  return 0x32; // 2
            case 4:  return 0x33; // 3
            case 5:  return 0x34; // 4
            case 6:  return 0x35; // 5
            case 7:  return 0x36; // 6
            case 8:  return 0x37; // 7
            case 9:  return 0x38; // 8
            case 10: return 0x39; // 9

            // Funkcyjne
            case 59: return 0x70; // F1
            case 60: return 0x71; // F2
            case 61: return 0x72; // F3
            case 62: return 0x73; // F4
            case 63: return 0x74; // F5
            case 64: return 0x75; // F6
            case 65: return 0x76; // F7
            case 66: return 0x77; // F8
            case 67: return 0x78; // F9
            case 68: return 0x79; // F10
            case 87: return 0x7A; // F11
            case 88: return 0x7B; // F12

            // Numpad
            case 82: return 0x60; // Numpad 0
            case 79: return 0x61; // Numpad 1
            case 80: return 0x62; // Numpad 2
            case 81: return 0x63; // Numpad 3
            case 75: return 0x64; // Numpad 4
            case 76: return 0x65; // Numpad 5
            case 77: return 0x66; // Numpad 6
            case 71: return 0x67; // Numpad 7
            case 72: return 0x68; // Numpad 8
            case 73: return 0x69; // Numpad 9
            case 55: return 0x6A; // Numpad *
            case 78: return 0x6B; // Numpad +
            case 74: return 0x6D; // Numpad -
            case 83: return 0x6E; // Numpad .
            case 98: return 0x6F; // Numpad /
            case 69: return 0x90; // Num Lock

            // Znaki punktowe
            case 39: return 0xBA; // ; :
            case 13: return 0xBB; // = +
            case 51: return 0xBC; // , <
            case 12: return 0xBD; // - _
            case 52: return 0xBE; // . >
            case 53: return 0xBF; // / ?
            case 41: return 0xC0; // ` ~
            case 26: return 0xDB; // [ {
            case 43: return 0xDC; // \ |
            case 27: return 0xDD; // ] }
            case 40: return 0xDE; // ' "

            // Modyfikatory
            case 42: return 0xA0; // KEY_LEFTSHIFT -> VK_LSHIFT
            case 54: return 0xA1; // KEY_RIGHTSHIFT -> VK_RSHIFT
            case 29: return 0xA2; // KEY_LEFTCTRL -> VK_LCONTROL
            case 97: return 0xA3; // KEY_RIGHTCTRL -> VK_RCONTROL
            case 56: return 0xA4; // KEY_LEFTALT -> VK_LMENU
            case 100: return 0xA5; // KEY_RIGHTALT -> VK_RMENU
            case 125: return 0x5B; // KEY_LEFTMETA -> VK_LWIN
            case 126: return 0x5C; // KEY_RIGHTMETA -> VK_RWIN

            // Nawigacyjne
            case 102: return 0x24; // HOME
            case 107: return 0x23; // END
            case 104: return 0x21; // PAGEUP
            case 109: return 0x22; // PAGEDOWN
            case 110: return 0x2D; // INSERT
            case 111: return 0x2E; // DELETE

            // Znaki kontrolne/Specjalne
            case 28: return 0x0D; // KEY_ENTER -> VK_RETURN
            case 1:  return 0x1B; // KEY_ESC -> VK_ESCAPE
            case 14: return 0x08; // KEY_BACKSPACE -> VK_BACK
            case 15: return 0x09; // KEY_TAB -> VK_TAB
            case 57: return 0x20; // KEY_SPACE -> VK_SPACE
            case 58: return 0x14; // KEY_CAPSLOCK -> VK_CAPITAL

            // Strzałki
            case 105: return 0x25; // KEY_LEFT -> VK_LEFT
            case 103: return 0x26; // KEY_UP -> VK_UP
            case 106: return 0x27; // KEY_RIGHT -> VK_RIGHT
            case 108: return 0x28; // KEY_DOWN -> VK_DOWN

            // Media
            case 113: return 0xAD; // MUTE
            case 114: return 0xAE; // VOL DOWN
            case 115: return 0xAF; // VOL UP
            case 164: return 0xB3; // PLAY/PAUSE
            case 163: return 0xB0; // NEXT
            case 165: return 0xB1; // PREV

            default: return 0;
        }
    }
};