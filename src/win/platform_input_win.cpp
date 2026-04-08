#include "../platform_input.hpp"
#include <windows.h>
#include <iostream>
#include <string>

class PlatformInputWin : public IPlatformInput {
    private:
    std::vector<INPUT> m_winInputs;

    public:
    PlatformInputWin() {
        // Reserve memory up front to prevent frequent allocations on every flush
        m_winInputs.reserve(1024);
    }

    void MoveMouseRelative(int32_t x, int32_t y) override {
        Log("PlatformInputWin: MoveMouseRelative x=" + std::to_string(x) + " y=" + std::to_string(y));

        INPUT input = { 0 };
        input.type = INPUT_MOUSE;
        input.mi.dx = x;
        input.mi.dy = y;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &input, sizeof(INPUT));
    }

    void MoveMouseAbsolute(int32_t x, int32_t y) override {
        Log("PlatformInputWin: MoveMouseAbsolute x=" + std::to_string(x) + " y=" + std::to_string(y));

        INPUT input = { 0 };
        input.type = INPUT_MOUSE;
        input.mi.dx = (x * 65535) / (GetSystemMetrics(SM_CXSCREEN) - 1);
        input.mi.dy = (y * 65535) / (GetSystemMetrics(SM_CYSCREEN) - 1);
        input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        SendInput(1, &input, sizeof(INPUT));
    }

    void MouseClick(int32_t button, bool down) override {
        INPUT input = { 0 };
        input.type = INPUT_MOUSE;

        switch (button) {
        case 0:
            input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
            break;
        case 1:
            input.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
            break;
        case 2:
            input.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
            break;
        default: return;
        }
        SendInput(1, &input, sizeof(INPUT));
    }

    void KeyPress(int32_t keyCode, bool down) override {
        INPUT input = { 0 };
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = static_cast<WORD>(keyCode);
        input.ki.wScan = static_cast<WORD>(MapVirtualKey(input.ki.wVk, MAPVK_VK_TO_VSC));

        input.ki.dwFlags = KEYEVENTF_SCANCODE;

        // Extended keys (like arrows, Home, End) require the KEYEVENTF_EXTENDEDKEY flag
        if (keyCode >= 33 && keyCode <= 46) {
            input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        }

        if (!down) {
            input.ki.dwFlags |= KEYEVENTF_KEYUP;
        }
        SendInput(1, &input, sizeof(INPUT));
    }

    void ScrollMouse(int32_t delta) override {
        Log("PlatformInputWin: ScrollMouse delta=" + std::to_string(delta));

        INPUT input = { 0 };
        input.type = INPUT_MOUSE;
        input.mi.mouseData = delta;
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        SendInput(1, &input, sizeof(INPUT));
    }

    void TypeCharacter(char16_t charCode) override {
        Log("PlatformInputWin: TypeCharacter charCode=" + std::to_string(charCode));

        INPUT inputs[2] = { 0 };
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wScan = static_cast<WORD>(charCode);
        inputs[0].ki.dwFlags = KEYEVENTF_UNICODE;

        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wScan = static_cast<WORD>(charCode);
        inputs[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;

        SendInput(2, inputs, sizeof(INPUT));
    }

    void ExecuteEvents(const std::vector<InputEvent>& events) override {
        Log("PlatformInputWin: Execute batch of " + std::to_string(events.size()) + " events");

        m_winInputs.clear();

        for (const auto& ev : events) {
            INPUT input = { 0 };
            bool hasSecond = false;
            INPUT input2 = { 0 };

            std::visit([&input, &hasSecond, &input2](auto&& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, struct MouseMoveRelative>) {
                    input.type = INPUT_MOUSE;
                    input.mi.dx = e.x;
                    input.mi.dy = e.y;
                    input.mi.dwFlags = MOUSEEVENTF_MOVE;
                } else if constexpr (std::is_same_v<T, struct MouseMoveAbsolute>) {
                    input.type = INPUT_MOUSE;
                    input.mi.dx = (e.x * 65535) / (GetSystemMetrics(SM_CXSCREEN) - 1);
                    input.mi.dy = (e.y * 65535) / (GetSystemMetrics(SM_CYSCREEN) - 1);
                    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
                } else if constexpr (std::is_same_v<T, struct MouseClick>) {
                    input.type = INPUT_MOUSE;
                    if (e.button == 0)      input.mi.dwFlags = e.down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
                    else if (e.button == 1) input.mi.dwFlags = e.down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
                    else if (e.button == 2) input.mi.dwFlags = e.down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
                } else if constexpr (std::is_same_v<T, struct KeyPress>) {
                    input.type = INPUT_KEYBOARD;
                    input.ki.wVk = static_cast<WORD>(e.keyCode);
                    input.ki.wScan = MapVirtualKey(input.ki.wVk, MAPVK_VK_TO_VSC);
                    input.ki.dwFlags = KEYEVENTF_SCANCODE;
                    if (!e.down) {
                        input.ki.dwFlags |= KEYEVENTF_KEYUP;
                    }
                } else if constexpr (std::is_same_v<T, struct MouseScroll>) {
                    input.type = INPUT_MOUSE;
                    input.mi.mouseData = e.delta;
                    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
                } else if constexpr (std::is_same_v<T, struct TypeCharacter>) {
                    input.type = INPUT_KEYBOARD;
                    input.ki.wScan = static_cast<WORD>(e.charCode);
                    input.ki.dwFlags = KEYEVENTF_UNICODE;
                    
                    input2 = input;
                    input2.ki.dwFlags |= KEYEVENTF_KEYUP;
                    hasSecond = true;
                }
                }, ev);

            m_winInputs.push_back(input);
            if (hasSecond) {
                m_winInputs.push_back(input2);
            }
        }

        if (!m_winInputs.empty()) {
            SendInput(static_cast<UINT>(m_winInputs.size()), m_winInputs.data(), sizeof(INPUT));
        }
    }
};

std::unique_ptr<IPlatformInput> CreatePlatformInput() {
    return std::make_unique<PlatformInputWin>();
}
