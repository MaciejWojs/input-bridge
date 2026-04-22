#include "../platform_input.hpp"
#include <windows.h>
#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <array>

class PlatformInputWin : public IPlatformInput {
    private:
    std::vector<INPUT> m_winInputs;
    std::jthread m_detectionThread;
    std::mutex m_detectedMutex;
    std::vector<InputEvent> m_detectedEvents;
    std::array<bool, 256> m_prevKeyStates = {};
    int32_t m_prevMouseX = 0;
    int32_t m_prevMouseY = 0;

    public:
    PlatformInputWin() {
        // Reserve memory up front to prevent frequent allocations on every flush
        m_winInputs.reserve(1024);
    }

    ~PlatformInputWin() {
        StopInputDetection();
    }

    bool Initialize(std::string& error_msg) override {
        // Windows input injection generally does not require session authorization
        return true;
    }

    bool StartInputDetection() override {
        if (m_detectionThread.joinable()) {
            return true;
        }

        POINT cursorPos;
        if (!GetCursorPos(&cursorPos)) {
            return false;
        }

        m_prevMouseX = cursorPos.x;
        m_prevMouseY = cursorPos.y;

        for (int vk = 0; vk < 256; ++vk) {
            m_prevKeyStates[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
        }

        m_detectionThread = std::jthread([this](std::stop_token stopToken) {
            this->DetectionLoop(stopToken);
        });

        return true;
    }

    void StopInputDetection() override {
        if (!m_detectionThread.joinable()) {
            return;
        }
        m_detectionThread.request_stop();
        m_detectionThread.join();
    }

    std::vector<InputEvent> DrainDetectedInputEvents() override {
        std::vector<InputEvent> drained;
        {
            std::lock_guard<std::mutex> lock(m_detectedMutex);
            drained.swap(m_detectedEvents);
        }
        return drained;
    }

    void DetectionLoop(std::stop_token stopToken) {
        while (!stopToken.stop_requested()) {
            Sleep(10);

            POINT pos;
            if (GetCursorPos(&pos)) {
                int32_t deltaX = pos.x - m_prevMouseX;
                int32_t deltaY = pos.y - m_prevMouseY;
                if (deltaX != 0 || deltaY != 0) {
                    std::lock_guard<std::mutex> lock(m_detectedMutex);
                    MouseMoveRelative move;
                    move.x = deltaX;
                    move.y = deltaY;
                    m_detectedEvents.push_back(move);
                }
                m_prevMouseX = pos.x;
                m_prevMouseY = pos.y;
            }

            std::vector<InputEvent> events;
            events.reserve(16);
            for (int vk = 1; vk < 256; ++vk) {
                bool isDown = (GetAsyncKeyState(vk) & 0x8000) != 0;
                if (isDown != m_prevKeyStates[vk]) {
                    ::KeyPress keyEvent;
                    keyEvent.keyCode = vk;
                    keyEvent.down = isDown;
                    keyEvent.routedTo = InputRoute::Keyboard;
                    events.push_back(keyEvent);
                    m_prevKeyStates[vk] = isDown;
                }
            }

            if (!events.empty()) {
                std::lock_guard<std::mutex> lock(m_detectedMutex);
                m_detectedEvents.insert(m_detectedEvents.end(), events.begin(), events.end());
            }
        }
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

    void AppendUnicodeInputSequence(uint32_t charCode, std::vector<INPUT>& outputs) {
        auto append = [&](WORD scan, DWORD flags) {
            INPUT input = { 0 };
            input.type = INPUT_KEYBOARD;
            input.ki.wScan = scan;
            input.ki.dwFlags = flags;
            outputs.push_back(input);
            };

        if (charCode <= 0xFFFF) {
            append(static_cast<WORD>(charCode), KEYEVENTF_UNICODE);
            append(static_cast<WORD>(charCode), KEYEVENTF_UNICODE | KEYEVENTF_KEYUP);
            return;
        }

        uint32_t scalar = charCode - 0x10000;
        WORD highSurrogate = static_cast<WORD>(0xD800 + ((scalar >> 10) & 0x3FF));
        WORD lowSurrogate = static_cast<WORD>(0xDC00 + (scalar & 0x3FF));

        append(highSurrogate, KEYEVENTF_UNICODE);
        append(highSurrogate, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP);
        append(lowSurrogate, KEYEVENTF_UNICODE);
        append(lowSurrogate, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP);
    }

    void TypeCharacter(uint32_t charCode) override {
        Log("PlatformInputWin: TypeCharacter charCode=" + std::to_string(charCode));

        std::vector<INPUT> inputs;
        inputs.reserve(charCode > 0xFFFF ? 4 : 2);
        AppendUnicodeInputSequence(charCode, inputs);
        SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    }

    void ExecuteEvents(const std::vector<InputEvent>& events) override {
        Log("PlatformInputWin: Execute batch of " + std::to_string(events.size()) + " events");

        m_winInputs.clear();

        for (const auto& ev : events) {
            std::vector<INPUT> eventInputs;
            eventInputs.reserve(4);

            std::visit([&](auto&& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, struct MouseMoveRelative>) {
                    INPUT input = { 0 };
                    input.type = INPUT_MOUSE;
                    input.mi.dx = e.x;
                    input.mi.dy = e.y;
                    input.mi.dwFlags = MOUSEEVENTF_MOVE;
                    eventInputs.push_back(input);
                } else if constexpr (std::is_same_v<T, struct MouseMoveAbsolute>) {
                    INPUT input = { 0 };
                    input.type = INPUT_MOUSE;
                    input.mi.dx = (e.x * 65535) / (GetSystemMetrics(SM_CXSCREEN) - 1);
                    input.mi.dy = (e.y * 65535) / (GetSystemMetrics(SM_CYSCREEN) - 1);
                    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
                    eventInputs.push_back(input);
                } else if constexpr (std::is_same_v<T, struct MouseClick>) {
                    INPUT input = { 0 };
                    input.type = INPUT_MOUSE;
                    if (e.button == 0)      input.mi.dwFlags = e.down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
                    else if (e.button == 1) input.mi.dwFlags = e.down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
                    else if (e.button == 2) input.mi.dwFlags = e.down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
                    eventInputs.push_back(input);
                } else if constexpr (std::is_same_v<T, struct KeyPress>) {
                    INPUT input = { 0 };
                    input.type = INPUT_KEYBOARD;
                    input.ki.wVk = static_cast<WORD>(e.keyCode);
                    input.ki.wScan = MapVirtualKey(input.ki.wVk, MAPVK_VK_TO_VSC);
                    input.ki.dwFlags = KEYEVENTF_SCANCODE;
                    if (!e.down) {
                        input.ki.dwFlags |= KEYEVENTF_KEYUP;
                    }
                    eventInputs.push_back(input);
                } else if constexpr (std::is_same_v<T, struct MouseScroll>) {
                    INPUT input = { 0 };
                    input.type = INPUT_MOUSE;
                    input.mi.mouseData = e.delta;
                    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
                    eventInputs.push_back(input);
                } else if constexpr (std::is_same_v<T, struct TypeCharacter>) {
                    AppendUnicodeInputSequence(e.charCode, eventInputs);
                }
                }, ev);

            m_winInputs.insert(m_winInputs.end(), eventInputs.begin(), eventInputs.end());
        }

        if (!m_winInputs.empty()) {
            SendInput(static_cast<UINT>(m_winInputs.size()), m_winInputs.data(), sizeof(INPUT));
        }
    }
};
