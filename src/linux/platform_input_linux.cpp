#include "../platform_input.hpp"
#include <iostream>

class PlatformInputLinux : public IPlatformInput {
    public:
    void MoveMouseRelative(int32_t x, int32_t y) override {
        // Implement with uinput or X11/XTest or Wayland/ydotool
    }

    void MoveMouseAbsolute(int32_t x, int32_t y) override {
        // Implement with uinput or X11/XTest or Wayland/ydotool
    }

    void MouseClick(int32_t button, bool down) override {
        // Implement with uinput or X11/XTest or Wayland/ydotool
    }

    void KeyPress(int32_t keyCode, bool down) override {
        // Implement with uinput or X11/XTest or Wayland/ydotool
    }
};

std::unique_ptr<IPlatformInput> CreatePlatformInput() {
    return std::make_unique<PlatformInputLinux>();
}
