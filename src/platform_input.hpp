#pragma once

#include <napi.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <functional>
#include <string>
#include <vector>
#include <variant>
#include <type_traits>

enum class InputRoute {
    Unicode,
    Keyboard
};

struct MouseMoveRelative { int32_t x; int32_t y; };
struct MouseMoveAbsolute { int32_t x; int32_t y; };
struct MouseClick { int32_t button; bool down; };
struct KeyPress { int32_t keyCode; bool down; InputRoute routedTo = InputRoute::Keyboard; };
struct MouseScroll { int32_t delta; };
struct TypeCharacter { uint32_t charCode; InputRoute routedTo = InputRoute::Unicode; };

using InputEvent = std::variant<MouseMoveRelative, MouseMoveAbsolute, MouseClick, KeyPress, MouseScroll, TypeCharacter>;

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

    virtual bool Initialize(std::string& error_msg) { return true; }

    // Linux RemoteDesktop transport controls.
    // Supported modes are backend-specific. Linux portal backend supports: "notify" and "eis".
    virtual bool SetInputMode(const std::string& mode, std::string& error_msg) {
        (void)mode;
        error_msg = "Input mode is not supported on this platform";
        return false;
    }

    virtual std::string GetInputMode() const {
        return "notify";
    }

    virtual bool ConnectToEIS(std::string& error_msg) {
        error_msg = "EIS is not supported on this platform";
        return false;
    }

    virtual void DisconnectEIS() {}

    virtual bool IsEISConnected() const {
        return false;
    }

    // Clipboard API
    // Sets text to clipboard. Returns true on success.
    virtual bool SetClipboardText(const std::string& text) = 0;
    // Gets text from clipboard. Returns std::nullopt if unavailable.
    virtual std::optional<std::string> GetClipboardText() = 0;
    // Sets file paths to clipboard (CF_HDROP). Returns true on success.
    virtual bool SetClipboardFiles(const std::vector<std::string>& filePaths) = 0;
    // Gets file paths from clipboard (CF_HDROP). Returns std::nullopt if unavailable.
    virtual std::optional<std::vector<std::string>> GetClipboardFiles() = 0;

    // Sets files into a remote-capable clipboard format, including file descriptor
    // and stream contents. This is required for Remote Desktop/redirected clipboard
    // file transfer.
    virtual bool SetClipboardFilesRemote(const std::vector<std::string>& filePaths) = 0;
    // Gets file descriptors from a remote-capable clipboard format. Returns file names
    // if available, or std::nullopt otherwise.
    virtual std::optional<std::vector<std::string>> GetClipboardFilesRemote() = 0;

    using ClipboardChangeCallback = std::function<void(const std::string& type, const std::vector<std::string>& files, const std::string& text)>;
    virtual void SetClipboardChangeCallback(ClipboardChangeCallback cb) {
        (void)cb;
    }

    using InputEventCallback = std::function<void(const InputEvent& ev)>;
    /**
     * Sets a callback that will be invoked for incoming input events (push)
     * coming from the platform (e.g. captured keyboard/mouse events).
     * Default implementation is a no-op.
     */
    virtual void SetInputEventCallback(InputEventCallback cb) {
        (void)cb;
    }

    void SetLogCallback(std::function<void(const std::string&)> cb) {
        m_log = cb;
    }

    virtual void MoveMouseRelative(int32_t x, int32_t y) = 0;
    virtual void MoveMouseAbsolute(int32_t x, int32_t y) = 0;
    virtual void MouseClick(int32_t button, bool down) = 0;
    virtual void KeyPress(int32_t keyCode, bool down) = 0;
    virtual void ScrollMouse(int32_t delta) {} // optional
    virtual void TypeCharacter(uint32_t charCode) {} // optional

    // Default implementation that executes a batch of events in order. Platform-specific backends can override this for optimization.
    virtual void ExecuteEvents(const std::vector<InputEvent>& events) {
        for (const auto& event : events) {
            std::visit([this](auto&& ev) {
                using T = std::decay_t<decltype(ev)>;
                if constexpr (std::is_same_v<T, struct MouseMoveRelative>) {
                    MoveMouseRelative(ev.x, ev.y);
                } else if constexpr (std::is_same_v<T, struct MouseMoveAbsolute>) {
                    MoveMouseAbsolute(ev.x, ev.y);
                } else if constexpr (std::is_same_v<T, struct MouseClick>) {
                    this->MouseClick(ev.button, ev.down);
                } else if constexpr (std::is_same_v<T, struct KeyPress>) {
                    this->KeyPress(ev.keyCode, ev.down);
                } else if constexpr (std::is_same_v<T, struct MouseScroll>) {
                    ScrollMouse(ev.delta);
                } else if constexpr (std::is_same_v<T, struct TypeCharacter>) {
                    this->TypeCharacter(ev.charCode);
                }
                }, event);
        }
    }
};

class InputQueue {
    private:
    std::vector<InputEvent> m_events;
    std::unique_ptr<IPlatformInput> m_platformInput;
    bool m_optimizeEnabled = true;

    public:
    InputQueue(std::unique_ptr<IPlatformInput> platformInput)
        : m_platformInput(std::move(platformInput)) {
    }

    void QueueMouseMoveRelative(int32_t x, int32_t y) {
        m_events.push_back(MouseMoveRelative{ x, y });
    }

    void QueueMouseMoveAbsolute(int32_t x, int32_t y) {
        m_events.push_back(MouseMoveAbsolute{ x, y });
    }

    void QueueMouseClick(int32_t button, bool down) {
        m_events.push_back(MouseClick{ button, down });
    }

    void QueueKeyPress(int32_t keyCode, bool down) {
        m_events.push_back(KeyPress{ keyCode, down, InputRoute::Keyboard });
    }

    void QueueScrollMouse(int32_t delta) {
        m_events.push_back(MouseScroll{ delta });
    }

    void QueueTypeCharacter(uint32_t charCode) {
        m_events.push_back(TypeCharacter{ charCode, InputRoute::Unicode });
    }

    private:

    void OptimizeMouseMovesInternal(int distanceThreshold, bool isAbsolute) {
        if (!m_optimizeEnabled || m_events.empty()) return;

        std::vector<InputEvent> optimized;
        optimized.reserve(m_events.size());

        int sqThreshold = distanceThreshold * distanceThreshold;

        for (size_t i = 0; i < m_events.size(); ++i) {
            bool isTargetType = isAbsolute ? std::holds_alternative<MouseMoveAbsolute>(m_events[i]) : std::holds_alternative<MouseMoveRelative>(m_events[i]);

            if (!isTargetType) {
                optimized.push_back(m_events[i]);
                continue;
            }

            // Find the start and end of a sequence of target events
            size_t seqStart = i;
            size_t seqEnd = i;
            while (seqEnd + 1 < m_events.size() &&
                (isAbsolute ? std::holds_alternative<MouseMoveAbsolute>(m_events[seqEnd + 1]) : std::holds_alternative<MouseMoveRelative>(m_events[seqEnd + 1]))) {
                seqEnd++;
            }

            // If there is only one point in the sequence, just add it
            if (seqStart == seqEnd) {
                optimized.push_back(m_events[seqStart]);
                continue;
            }

            // Add the first point of the sequence unchanged (or as a starting point for accumulation)
            optimized.push_back(m_events[seqStart]);

            if (isAbsolute) {
                auto startEv = std::get<MouseMoveAbsolute>(m_events[seqStart]);
                int lastKeptX = startEv.x;
                int lastKeptY = startEv.y;
                size_t lastKeptIndex = seqStart;

                for (size_t j = seqStart + 1; j < seqEnd; ++j) {
                    auto ev = std::get<MouseMoveAbsolute>(m_events[j]);
                    int distSq = (ev.x - lastKeptX) * (ev.x - lastKeptX) +
                        (ev.y - lastKeptY) * (ev.y - lastKeptY);

                    // Add early exit rule for extremely dense queues
                    if (distSq >= sqThreshold || j - lastKeptIndex >= 50) {
                        optimized.push_back(ev);
                        lastKeptX = ev.x;
                        lastKeptY = ev.y;
                        lastKeptIndex = j;
                    }
                }

                // Always add the last point to ensure the mouse reaches the exact target
                auto lastEv = std::get<MouseMoveAbsolute>(m_events[seqEnd]);
                if (lastKeptX != lastEv.x || lastKeptY != lastEv.y) {
                    optimized.push_back(lastEv);
                }
            } else {
                // For relative movements ("dx", "dy") we need to sum skipped pixels
                int accumX = 0;
                int accumY = 0;
                size_t lastKeptIndex = seqStart;

                for (size_t j = seqStart + 1; j < seqEnd; ++j) {
                    auto ev = std::get<MouseMoveRelative>(m_events[j]);
                    accumX += ev.x;
                    accumY += ev.y;

                    // Add early exit rule for extremely dense queues
                    if (accumX * accumX + accumY * accumY >= sqThreshold || j - lastKeptIndex >= 50) {
                        MouseMoveRelative keptEv{ accumX, accumY };
                        optimized.push_back(keptEv);
                        accumX = 0;
                        accumY = 0;
                        lastKeptIndex = j;
                    }
                }

                auto lastEv = std::get<MouseMoveRelative>(m_events[seqEnd]);
                accumX += lastEv.x;
                accumY += lastEv.y;

                // The final point emits all remaining accumulated "distance"
                if (accumX != 0 || accumY != 0) {
                    MouseMoveRelative finalEv{ accumX, accumY };
                    optimized.push_back(finalEv);
                }
            }

            i = seqEnd; // Jump the loop index
        } // End of outer loop (for size_t i = 0;)

        m_events = std::move(optimized);
    } // End of OptimizeMouseMovesInternal

    public:
    bool ToggleOptimization() {
        m_optimizeEnabled = !m_optimizeEnabled;
        return m_optimizeEnabled;
    }

    void OptimizeMouseMovesRelative(int distanceThreshold) {
        OptimizeMouseMovesInternal(distanceThreshold, false);
    }

    void OptimizeMouseMovesAbsolute(int distanceThreshold) {
        OptimizeMouseMovesInternal(distanceThreshold, true);
    }

    void Flush() {
        if (!m_events.empty() && m_platformInput) {
            m_platformInput->ExecuteEvents(m_events);
            m_events.clear();
        }
    }

    IPlatformInput* GetPlatform() { return m_platformInput.get(); }
};

std::unique_ptr<IPlatformInput> CreatePlatformInput();