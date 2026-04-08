#include "platform_input.hpp"
#include <iostream>

class PlatformInputStub : public IPlatformInput {
    public:
    void MoveMouse(int32_t x, int32_t y) override {
        // NotImplemented
    }

    void MouseClick(int32_t button, bool down) override {
        // NotImplemented
    }

    void KeyPress(int32_t keyCode, bool down) override {
        // NotImplemented
    }

    void ScrollMouse(int32_t delta) override {
        // NotImplemented
    }
};

std::unique_ptr<IPlatformInput> CreatePlatformInput() {
    return std::make_unique<PlatformInputStub>();
}
