#include "../platform_input.hpp"
#include "../key_translator.hpp"

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

// Fix for X11 macro pollution
#undef KeyPress
#undef KeyRelease

#include <unistd.h>
#include <dlfcn.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>
#include <map>

using xkb_utf32_to_keysym_t = uint32_t(*)(uint32_t);
static void* xkb_handle = nullptr;
static xkb_utf32_to_keysym_t xkb_utf32_to_keysym_func = nullptr;
static bool xkb_common_initialized = false;

static void InitOptionalXkbCommon() {
    if (xkb_common_initialized) return;
    xkb_common_initialized = true;

    xkb_handle = dlopen("libxkbcommon.so.0", RTLD_LAZY | RTLD_LOCAL);
    if (!xkb_handle) {
        xkb_handle = dlopen("libxkbcommon.so", RTLD_LAZY | RTLD_LOCAL);
    }

    if (xkb_handle) {
        xkb_utf32_to_keysym_func = reinterpret_cast<xkb_utf32_to_keysym_t>(dlsym(xkb_handle, "xkb_utf32_to_keysym"));
    }
}

namespace {

KeySym EvdevToX11KeySym(int32_t evdev) {
    switch (evdev) {
        case 30: return XK_a;
        case 48: return XK_b;
        case 46: return XK_c;
        case 32: return XK_d;
        case 18: return XK_e;
        case 33: return XK_f;
        case 34: return XK_g;
        case 35: return XK_h;
        case 23: return XK_i;
        case 36: return XK_j;
        case 37: return XK_k;
        case 38: return XK_l;
        case 50: return XK_m;
        case 49: return XK_n;
        case 24: return XK_o;
        case 25: return XK_p;
        case 16: return XK_q;
        case 19: return XK_r;
        case 31: return XK_s;
        case 20: return XK_t;
        case 22: return XK_u;
        case 47: return XK_v;
        case 17: return XK_w;
        case 45: return XK_x;
        case 21: return XK_y;
        case 44: return XK_z;

        case 11: return XK_0;
        case 2: return XK_1;
        case 3: return XK_2;
        case 4: return XK_3;
        case 5: return XK_4;
        case 6: return XK_5;
        case 7: return XK_6;
        case 8: return XK_7;
        case 9: return XK_8;
        case 10: return XK_9;

        case 28: return XK_Return;
        case 1: return XK_Escape;
        case 14: return XK_BackSpace;
        case 15: return XK_Tab;
        case 57: return XK_space;

        case 105: return XK_Left;
        case 103: return XK_Up;
        case 106: return XK_Right;
        case 108: return XK_Down;

        case 42: return XK_Shift_L;
        case 54: return XK_Shift_R;
        case 29: return XK_Control_L;
        case 97: return XK_Control_R;
        case 56: return XK_Alt_L;
        case 100: return XK_Alt_R;
        default: return NoSymbol;
    }
}

KeySym CharToKeySym(char32_t cp, bool& needShift) {
    needShift = false;

    if (cp >= U'a' && cp <= U'z') {
        return static_cast<KeySym>(cp);
    }

    if (cp >= U'A' && cp <= U'Z') {
        needShift = true;
        return static_cast<KeySym>(cp + 32);
    }
    switch (cp) {
        case U' ': return XK_space;
        case U'\n': return XK_Return;
        case U'\r': return XK_Return;
        case U'\t': return XK_Tab;
        case U'!': needShift = true; return XK_1;
        case U'@': needShift = true; return XK_2;
        case U'#': needShift = true; return XK_3;
        case U'$': needShift = true; return XK_4;
        case U'%': needShift = true; return XK_5;
        case U'^': needShift = true; return XK_6;
        case U'&': needShift = true; return XK_7;
        case U'*': needShift = true; return XK_8;
        case U'(': needShift = true; return XK_9;
        case U')': needShift = true; return XK_0;
        case U'_': needShift = true; return XK_minus;
        case U'+': needShift = true; return XK_equal;
        case U'{': needShift = true; return XK_bracketleft;
        case U'}': needShift = true; return XK_bracketright;
        case U'|': needShift = true; return XK_backslash;
        case U':': needShift = true; return XK_semicolon;
        case U'"': needShift = true; return XK_apostrophe;
        case U'<': needShift = true; return XK_comma;
        case U'>': needShift = true; return XK_period;
        case U'?': needShift = true; return XK_slash;
        case U'~': needShift = true; return XK_grave;
        default: break;
    }

    if (cp <= 0x7f) {
        return static_cast<KeySym>(cp);
    }

    return static_cast<KeySym>(0x01000000u | cp);
}

} // namespace

class X11PlatformInput : public IPlatformInput {
private:
    Display* m_display = nullptr;
    std::vector<KeyCode> m_scratchCodes;
    std::vector<KeySym> m_scratchSyms;
    size_t m_scratchIndex = 0;
    bool m_scratchInitialized = false;

    void InitScratch() {
        if (m_scratchInitialized) return;
        m_scratchInitialized = true;

        int min_keycode = 0, max_keycode = 0;
        XDisplayKeycodes(m_display, &min_keycode, &max_keycode);

        for (KeyCode i = max_keycode; i >= min_keycode; --i) {
            int keysyms_per_keycode = 0;
            KeySym* ks = XGetKeyboardMapping(m_display, i, 1, &keysyms_per_keycode);
            bool empty = true;
            if (ks) {
                for (int k = 0; k < keysyms_per_keycode; ++k) {
                    if (ks[k] != NoSymbol) { empty = false; break; }
                }
                XFree(ks);
            }
            if (empty) {
                m_scratchCodes.push_back(i);
                m_scratchSyms.push_back(NoSymbol);
            }
        }
    }

    KeyCode GetScratchForSym(KeySym sym) {
        InitScratch();
        if (m_scratchCodes.empty()) return 0;

        // Re-use an existing scratch key if already mapped
        for (size_t i = 0; i < m_scratchCodes.size(); ++i) {
            if (m_scratchSyms[i] == sym) {
                return m_scratchCodes[i];
            }
        }

        // Allocate a scratch key from the rotating pool
        size_t idx = m_scratchIndex;
        m_scratchIndex = (m_scratchIndex + 1) % m_scratchCodes.size();

        KeyCode code = m_scratchCodes[idx];
        m_scratchSyms[idx] = sym;

        KeySym newSyms[2] = { sym, sym }; 
        XChangeKeyboardMapping(m_display, code, 2, newSyms, 1);
        XSync(m_display, False);

        // Allow target applications some time to process the mapping notify event
        usleep(1000 * 5); // 5ms

        return code;
    }

    KeyCode ResolveKeycodeFromEvdevInput(int32_t keyCode) const {
        int32_t evdevCode = 0;

        if ((keyCode & FLAG_RAW_MASK) == FLAG_RAW_LINUX) {
            evdevCode = keyCode & ~FLAG_RAW_MASK;
        } else if ((keyCode & FLAG_RAW_MASK) == FLAG_RAW_WINDOWS) {
            evdevCode = KeyTranslator::WindowsToLinux(keyCode & ~FLAG_RAW_MASK);
        } else {
            evdevCode = KeyTranslator::WindowsToLinux(keyCode);
        }

        if (evdevCode == 0) {
            return 0;
        }

        KeySym keySym = EvdevToX11KeySym(evdevCode);
        if (keySym == NoSymbol) {
            return 0;
        }

        return XKeysymToKeycode(m_display, keySym);
    }

    void ClickWheelButton(unsigned int button) {
        XTestFakeButtonEvent(m_display, button, True, CurrentTime);
        usleep(1000);
        XTestFakeButtonEvent(m_display, button, False, CurrentTime);
        usleep(1000);
    }

public:
    X11PlatformInput() {
        m_display = XOpenDisplay(nullptr);
        if (m_display == nullptr) {
            const char* displayEnv = std::getenv("DISPLAY");
            std::string details = displayEnv ? displayEnv : "(not set)";
            throw std::runtime_error("Failed to open X11 display. DISPLAY=" + details);
        }
    }

    ~X11PlatformInput() override {
        if (m_display != nullptr) {
            // Restore scratch variables
            if (m_scratchInitialized) {
                for (size_t i = 0; i < m_scratchCodes.size(); ++i) {
                    if (m_scratchSyms[i] != NoSymbol) {
                        KeySym emptySyms[2] = { NoSymbol, NoSymbol };
                        XChangeKeyboardMapping(m_display, m_scratchCodes[i], 2, emptySyms, 1);
                    }
                }
                if (!m_scratchCodes.empty()) {
                    XSync(m_display, False);
                }
            }

            XCloseDisplay(m_display);
            m_display = nullptr;
        }
    }

    bool Initialize(std::string& error_msg) override {
        (void)error_msg;
        return true;
    }

    void MoveMouseAbsolute(int32_t x, int32_t y) override {
        XTestFakeMotionEvent(m_display, -1, x, y, CurrentTime);
    }

    void MoveMouseRelative(int32_t x, int32_t y) override {
        XTestFakeRelativeMotionEvent(m_display, x, y, CurrentTime);
    }

    void MouseClick(int32_t button, bool down) override {
        unsigned int x11Button = 0;
        if (button == 0) {
            x11Button = 1;
        } else if (button == 1) {
            x11Button = 3;
        } else if (button == 2) {
            x11Button = 2;
        } else {
            fprintf(stderr, "[X11 WARN] Unsupported mouse button index: %d\n", button);
            return;
        }

        XTestFakeButtonEvent(m_display, x11Button, down ? True : False, CurrentTime);
    }

    void ScrollMouse(int32_t delta) override {
        if (delta == 0) {
            return;
        }

        const unsigned int wheelButton = (delta > 0) ? 4 : 5;
        int steps = std::abs(delta);
        for (int i = 0; i < steps; ++i) {
            ClickWheelButton(wheelButton);
        }
    }

    void KeyPress(int32_t keyCode, bool down) override {
        KeyCode x11Code = ResolveKeycodeFromEvdevInput(keyCode);
        if (x11Code == 0) {
            fprintf(stderr, "[X11 WARN] Unsupported key code: %d\n", keyCode);
            return;
        }

        XTestFakeKeyEvent(m_display, x11Code, down ? True : False, CurrentTime);
    }

    void TypeCharacter(uint32_t charCode) override {
        uint32_t codepoint = charCode;
        KeySym keySym = 0;

        // 1. Try mapping with libxkbcommon (if loaded)
        InitOptionalXkbCommon();
        if (xkb_utf32_to_keysym_func) {
            keySym = xkb_utf32_to_keysym_func(codepoint);
        }

        // 2. Fallback to the standard Unicode Keysym (if xkb fails)
        if (keySym == 0) {
            if (codepoint >= 0x20 && codepoint <= 0x7E) {
                keySym = codepoint; // ASCII
            } else if (codepoint > 0x7E) {
                keySym = codepoint | 0x01000000; // Standard Unicode prefix in X11
            } else {
                if (codepoint == 0x0D) keySym = 0xFF0D;      // Enter
                else if (codepoint == 0x08) keySym = 0xFF08; // Backspace
                else if (codepoint == 0x09) keySym = 0xFF09; // Tab
                else return;
            }
        }

        KeyCode keyCode = XKeysymToKeycode(m_display, keySym);
        bool needShift = false;
        bool mappedToScratch = false;

        int keysyms_per_keycode = 0;
        KeySym* syms = nullptr;
        if (keyCode != 0) {
            syms = XGetKeyboardMapping(m_display, keyCode, 1, &keysyms_per_keycode);
        }

        bool directMatch = false;
        if (syms) {
            if (keysyms_per_keycode > 0 && syms[0] == keySym) {
                directMatch = true;
            } else if (keysyms_per_keycode > 1 && syms[1] == keySym) {
                directMatch = true;
                needShift = true;
            }
            XFree(syms);
        }

        if (!directMatch) {
            // Kod przypisany w układzie (lub jego brak) wymagałby wciskania AltGr (Level 3) lub 
            // kombinacji grup. Aby tego uniknąć (co jest problematyczne z XTest),
            // przypiszemy znak bezpośrednio (jako bazę bez modyfikatorów) 
            // pod nieużywany, "pusty" klawisz systemowy na czas wciśnięcia.
            // Bierzemy klawisz z puli i nie zwracamy go aż do zamknięcia addonu, 
            // aby uniknąć błędów wyścigów w target aplikacji
            keyCode = GetScratchForSym(keySym);
            needShift = false; // Został przypisany wprost!
            
            if (keyCode == 0) {
                fprintf(stderr, "[X11 WARN] Brak pustego przycisku dla znaku U+%04X\n", codepoint);
                return;
            }
            mappedToScratch = true;
        }

        KeyCode shiftCode = 0;
        if (needShift) {
            shiftCode = XKeysymToKeycode(m_display, XK_Shift_L);
            if (shiftCode) {
                XTestFakeKeyEvent(m_display, shiftCode, True, CurrentTime);
                XSync(m_display, False);
            }
        }

        XTestFakeKeyEvent(m_display, keyCode, True, CurrentTime);
        XTestFakeKeyEvent(m_display, keyCode, False, CurrentTime);
        XSync(m_display, False);

        if (shiftCode) {
            XTestFakeKeyEvent(m_display, shiftCode, False, CurrentTime);
            XSync(m_display, False);
        }
    }

    void ExecuteEvents(const std::vector<InputEvent>& events) override {
        IPlatformInput::ExecuteEvents(events);
        XFlush(m_display);
        XSync(m_display, False);
    }
};