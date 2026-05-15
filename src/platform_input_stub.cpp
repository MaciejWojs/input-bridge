#include "platform_input.hpp"
#include <iostream>

class PlatformInputStub : public IPlatformInput {
    public:
    bool SetClipboardText(const std::string&) override {
        return false;
    }

    std::optional<std::string> GetClipboardText() override {
        return std::nullopt;
    }

    bool SetClipboardFiles(const std::vector<std::string>&) override {
        return false;
    }

    std::optional<std::vector<std::string>> GetClipboardFiles() override {
        return std::nullopt;
    }

    bool SetClipboardFilesRemote(const std::vector<ClipboardRemoteFileEntry>&) override {
        return false;
    }

    std::optional<std::vector<std::string>> GetClipboardFilesRemote() override {
        return std::nullopt;
    }

    std::vector<MonitorInfo> GetMonitors() override {
        return {};
    }

    void SetMonitors(const std::vector<MonitorInfo>&) override {
    }

    bool SetCurrentMonitor(int32_t, int32_t, int32_t) override {
        return false;
    }

    void MoveMouseRelative(int32_t x, int32_t y) override {
        // NotImplemented
    }

    void MoveMouseAbsolute(int32_t x, int32_t y) override {
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

    void TypeCharacter(uint32_t charCode) override {
        // NotImplemented
    }
};
