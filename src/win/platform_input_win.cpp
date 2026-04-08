#include "../platform_input.hpp"
#include <windows.h>
#include <iostream>
#include <string>

class PlatformInputWin : public IPlatformInput {
    public:
    void MoveMouse(int32_t x, int32_t y) override {
        Log("PlatformInputWin: MoveMouse x=" + std::to_string(x) + " y=" + std::to_string(y));

        INPUT input = { 0 };
        input.type = INPUT_MOUSE;
        input.mi.dx = x;
        input.mi.dy = y;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &input, sizeof(INPUT));
    }

    void MouseClick(int32_t button, bool down) override {
        Log("PlatformInputWin: MouseClick button=" + std::to_string(button) + " down=" + (down ? "true" : "false"));

        INPUT input = { 0 };
        input.type = INPUT_MOUSE;
        if (button == 0) {
            input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        } else if (button == 1) {
            input.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        } else if (button == 2) {
            input.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        }
        SendInput(1, &input, sizeof(INPUT));
    }

    void KeyPress(int32_t keyCode, bool down) override {
        Log("PlatformInputWin: KeyPress keyCode=" + std::to_string(keyCode) + " down=" + (down ? "true" : "false"));

        INPUT input = { 0 };
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = static_cast<WORD>(keyCode);
        if (!down) {
            input.ki.dwFlags = KEYEVENTF_KEYUP;
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
};

std::unique_ptr<IPlatformInput> CreatePlatformInput() {
    return std::make_unique<PlatformInputWin>();
}
