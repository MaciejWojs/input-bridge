#pragma once

#include <cstdint>
#include <string>

class LinuxUinputInjector {
public:
    LinuxUinputInjector() = default;
    ~LinuxUinputInjector();

    bool Initialize(std::string& error_msg);
    void Shutdown();

    bool IsReady() const;
    bool MoveRelative(int32_t x, int32_t y);
    bool MoveAbsolute(int32_t x, int32_t y, int32_t width, int32_t height);
    bool MouseClick(int32_t button, bool down);
    bool KeyPress(uint32_t evdev_code, bool down);
    bool Scroll(int32_t delta);

private:
    bool EmitEvent(uint16_t type, uint16_t code, int32_t value);
    bool EmitSync();

    int m_fd = -1;
};
