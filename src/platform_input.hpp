#pragma once

#include <napi.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <functional>
#include <string>

class IPlatformInput {
    protected:
    std::function<void(const std::string&)> m_log;

    void Log(const std::string& msg) {
        if (m_log) {
            m_log(msg);
        }
    }

    public:
    virtual ~IPlatformInput() = default;

    void SetLogCallback(std::function<void(const std::string&)> cb) {
        m_log = cb;
    }

    virtual void MoveMouse(int32_t x, int32_t y) = 0;
    virtual void MouseClick(int32_t button, bool down) = 0;
    virtual void KeyPress(int32_t keyCode, bool down) = 0;
    virtual void ScrollMouse(int32_t delta) {} // opcjonalnie
};

std::unique_ptr<IPlatformInput> CreatePlatformInput();
