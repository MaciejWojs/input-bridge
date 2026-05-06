#include "../platform_input.hpp"
#include "../key_translator.hpp"
#include <iostream>
#include <string>
#include <sstream>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <unistd.h>
#include <thread>
#include <future>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <map>
#include <functional>
#include <dlfcn.h>
#include <poll.h>

#if defined(__has_include)
#  if __has_include(<libei.h>)
#    include <libei.h>
#    define INPUT_BRIDGE_HAS_LIBEI 1
#  elif __has_include(<libei/libei.h>)
#    include <libei/libei.h>
#    define INPUT_BRIDGE_HAS_LIBEI 1
#  else
#    define INPUT_BRIDGE_HAS_LIBEI 0
#  endif
#else
#  define INPUT_BRIDGE_HAS_LIBEI 0
#endif

#include <algorithm>

class PlatformInputLinux : public IPlatformInput {

    enum class InputMode {
        Notify,
        EIS
    };

    InputMode input_mode = InputMode::Notify;
    int eis_fd = -1;
    std::atomic<bool> eis_connected{ false };

    bool SetInputMode(const std::string& mode, std::string& error_msg) override {
        if (mode == "notify") {
            DisconnectEIS();
            input_mode = InputMode::Notify;
            return true;
        }
        if (mode == "eis") {
            input_mode = InputMode::EIS;
            return true;
        }

        error_msg = "Unsupported mode. Use 'notify' or 'eis'.";
        return false;
    }

    std::string GetInputMode() const override {
        return input_mode == InputMode::EIS ? "eis" : "notify";
    }

    bool ConnectToEIS(std::string& error_msg) override {
#if !INPUT_BRIDGE_HAS_LIBEI
        error_msg = "EIS/libei support is not available in this build. Input will stay on notify mode.";
        return false;
#else
        if (!is_session_ready.load(std::memory_order_relaxed)) {
            error_msg = "Session is not ready. Call init() and wait for portal authorization first.";
            return false;
        }

        if (HasEISDevice()) {
            eis_connected.store(true, std::memory_order_relaxed);
            return true;
        }

        const std::string current_session = GetSessionHandle();
        if (current_session.empty()) {
            error_msg = "RemoteDesktop session handle is empty.";
            return false;
        }

        if (m_eis.context || m_eis.device || m_eis.seat) {
            ReleaseEISState();
        }

        GVariantBuilder options_builder;
        g_variant_builder_init(&options_builder, G_VARIANT_TYPE_VARDICT);

        GError* error = nullptr;
        GUnixFDList* out_fd_list = nullptr;
        GVariant* result = g_dbus_connection_call_with_unix_fd_list_sync(
            connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.RemoteDesktop",
            "ConnectToEIS",
            g_variant_new("(o@a{sv})", current_session.c_str(), g_variant_builder_end(&options_builder)),
            G_VARIANT_TYPE("(h)"),
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            &out_fd_list,
            nullptr,
            &error
        );

        if (error) {
            error_msg = std::string("ConnectToEIS failed: ") + error->message;
            g_error_free(error);
            if (out_fd_list) {
                g_object_unref(out_fd_list);
            }
            if (result) {
                g_variant_unref(result);
            }
            return false;
        }

        gint handle_index = -1;
        g_variant_get(result, "(h)", &handle_index);
        int fd = g_unix_fd_list_get(out_fd_list, handle_index, nullptr);

        if (result) {
            g_variant_unref(result);
        }
        if (out_fd_list) {
            g_object_unref(out_fd_list);
        }

        if (fd < 0) {
            error_msg = "ConnectToEIS did not return a valid fd.";
            return false;
        }

        m_eis.context = ei_new_sender(this);
        if (!m_eis.context) {
            close(fd);
            error_msg = "Failed to create libei sender context.";
            return false;
        }

        ei_configure_name(m_eis.context, "input-bridge");
        if (ei_setup_backend_fd(m_eis.context, fd) < 0) {
            close(fd);
            ei_unref(m_eis.context);
            m_eis.context = nullptr;
            error_msg = "Failed to initialize libei on the portal fd.";
            return false;
        }

        m_eis.connected = false;
        m_eis.started = false;
        m_eis.sequence = 0;
        m_eis.pending_disconnect = false;
        m_eis.seat_requested = false;

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            EISDispatchPending();
            if (HasEISDevice()) {
                break;
            }

            const int eis_fd_local = ei_get_fd(m_eis.context);
            if (eis_fd_local < 0) {
                break;
            }

            pollfd pfd{};
            pfd.fd = eis_fd_local;
            pfd.events = POLLIN;
            pfd.revents = 0;

            const int rc = poll(&pfd, 1, 50);
            if (rc < 0 && errno != EINTR) {
                error_msg = std::string("EIS poll failed: ") + std::strerror(errno);
                ReleaseEISState();
                return false;
            }
        }

        EISDispatchPending();
        if (!HasEISDevice()) {
            error_msg = "Timed out waiting for the EIS device to become ready.";
            ReleaseEISState();
            return false;
        }

        eis_connected.store(true, std::memory_order_relaxed);
        input_mode = InputMode::EIS;
        std::cout << "ConnectToEIS succeeded. EIS sender is ready." << std::endl;
        return true;
#endif
    }

    void DisconnectEIS() override {
#if INPUT_BRIDGE_HAS_LIBEI
        ReleaseEISState();
#endif
        if (eis_fd >= 0) {
            close(eis_fd);
            eis_fd = -1;
        }
        eis_connected.store(false, std::memory_order_relaxed);
        if (input_mode == InputMode::EIS) {
            input_mode = InputMode::Notify;
        }
    }

    bool IsEISConnected() const override {
        return eis_connected.load(std::memory_order_relaxed);
    }

    // If portal Notify* calls are rejected we avoid repeated attempts
    bool portal_notify_allowed = true;

    std::mutex clipboard_mutex;
    std::map<std::string, std::string> m_clipboardData;
    guint clipboard_signal_id = 0;
    ClipboardChangeCallback m_clipboardCallback;
    std::mutex clipboard_callback_mutex;
    InputEventCallback m_inputCallback;
    std::mutex input_callback_mutex;

    bool SetClipboardText(const std::string& text) override {
        if (!is_session_ready) return false;

        const std::string session_handle = GetSessionHandle();
        {
            std::lock_guard<std::mutex> lock(clipboard_mutex);
            m_clipboardData["text/plain;charset=utf-8"] = text;
        }

        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);

        GVariantBuilder mime_builder;
        g_variant_builder_init(&mime_builder, G_VARIANT_TYPE_STRING_ARRAY);
        g_variant_builder_add(&mime_builder, "s", "text/plain;charset=utf-8");

        g_variant_builder_add(&options, "{sv}", "mime_types", g_variant_builder_end(&mime_builder));

        GError* error = nullptr;
        GVariant* result = g_dbus_connection_call_sync(
            connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.Clipboard",
            "SetSelection",
            g_variant_new("(o@a{sv})", session_handle.c_str(), g_variant_builder_end(&options)),
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            &error
        );

        if (error) {
            std::cerr << "Failed to SetSelection (Clipboard Text): " << error->message << std::endl;
            g_error_free(error);
            return false;
        }
        if (result) g_variant_unref(result);
        EmitClipboardChange("text", {}, text);
        return true;
    }

    std::optional<std::string> GetClipboardText() override {
        if (!is_session_ready) return std::nullopt;

        const std::string session_handle = GetSessionHandle();
        GError* error = nullptr;
        GUnixFDList* out_fd_list = nullptr;
        GVariant* result = g_dbus_connection_call_with_unix_fd_list_sync(
            connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.Clipboard",
            "SelectionRead",
            g_variant_new("(os)", session_handle.c_str(), "text/plain;charset=utf-8"),
            G_VARIANT_TYPE("(h)"),
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            &out_fd_list,
            nullptr,
            &error
        );

        if (error) {
            // Note: Normal if the clipboard is empty or type not available.
            g_error_free(error);
            return std::nullopt;
        }

        gint handle_index;
        g_variant_get(result, "(h)", &handle_index);
        int fd = g_unix_fd_list_get(out_fd_list, handle_index, nullptr);

        std::string data;
        if (fd >= 0) {
            char buffer[1024];
            ssize_t bytes_read;
            while ((bytes_read = read(fd, buffer, sizeof(buffer))) > 0) {
                data.append(buffer, bytes_read);
            }
            close(fd);
        }

        if (out_fd_list) g_object_unref(out_fd_list);
        if (result) g_variant_unref(result);

        return data;
    }

    bool SetClipboardFiles(const std::vector<std::string>& files) override {
        if (!is_session_ready) return false;

        const std::string session_handle = GetSessionHandle();
        std::string payload;
        for (const auto& file : files) {
            payload += "file://" + file + "\r\n";
        }

        {
            std::lock_guard<std::mutex> lock(clipboard_mutex);
            m_clipboardData["text/uri-list"] = payload;
        }

        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);

        GVariantBuilder mime_builder;
        g_variant_builder_init(&mime_builder, G_VARIANT_TYPE_STRING_ARRAY);
        g_variant_builder_add(&mime_builder, "s", "text/uri-list");

        g_variant_builder_add(&options, "{sv}", "mime_types", g_variant_builder_end(&mime_builder));

        GError* error = nullptr;
        GVariant* result = g_dbus_connection_call_sync(
            connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.Clipboard",
            "SetSelection",
            g_variant_new("(o@a{sv})", session_handle.c_str(), g_variant_builder_end(&options)),
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            &error
        );

        if (error) {
            std::cerr << "Failed to SetSelection (Clipboard Files): " << error->message << std::endl;
            g_error_free(error);
            return false;
        }
        if (result) g_variant_unref(result);
        EmitClipboardChange("files", files, {});
        return true;
    }

    std::optional<std::vector<std::string>> GetClipboardFiles() override {
        if (!is_session_ready) return std::nullopt;

        const std::string session_handle = GetSessionHandle();
        GError* error = nullptr;
        GUnixFDList* out_fd_list = nullptr;
        GVariant* result = g_dbus_connection_call_with_unix_fd_list_sync(
            connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.Clipboard",
            "SelectionRead",
            g_variant_new("(os)", session_handle.c_str(), "text/uri-list"),
            G_VARIANT_TYPE("(h)"),
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            &out_fd_list,
            nullptr,
            &error
        );

        if (error) {
            g_error_free(error);
            return std::nullopt;
        }

        gint handle_index;
        g_variant_get(result, "(h)", &handle_index);
        int fd = g_unix_fd_list_get(out_fd_list, handle_index, nullptr);

        std::string data;
        if (fd >= 0) {
            char buffer[1024];
            ssize_t bytes_read;
            while ((bytes_read = read(fd, buffer, sizeof(buffer))) > 0) {
                data.append(buffer, bytes_read);
            }
            close(fd);
        }

        if (out_fd_list) g_object_unref(out_fd_list);
        if (result) g_variant_unref(result);

        if (data.empty()) return std::nullopt;

        std::vector<std::string> files;
        std::istringstream stream(data);
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const std::string prefix = "file://";
            if (line.compare(0, prefix.size(), prefix) == 0) {
                files.push_back(line.substr(prefix.size()));
            }
        }

        return files.empty() ? std::nullopt : std::make_optional(files);
    }

    bool SetClipboardFilesRemote(const std::vector<std::string>& files) override {
        return SetClipboardFiles(files);
    }

    std::optional<std::vector<std::string>> GetClipboardFilesRemote() override {
        return GetClipboardFiles();
    }

    void SetClipboardChangeCallback(ClipboardChangeCallback cb) override {
        std::lock_guard<std::mutex> lock(clipboard_callback_mutex);
        m_clipboardCallback = std::move(cb);
    }

    void SetInputEventCallback(InputEventCallback cb) override {
        std::lock_guard<std::mutex> lock(input_callback_mutex);
        m_inputCallback = std::move(cb);
    }

    void EmitInputEvent(const InputEvent& ev) {
        InputEventCallback cb;
        {
            std::lock_guard<std::mutex> lock(input_callback_mutex);
            cb = m_inputCallback;
        }
        if (cb) cb(ev);
    }

    void EmitClipboardChange(const std::string& type, const std::vector<std::string>& files, const std::string& text) {
        ClipboardChangeCallback callback;
        {
            std::lock_guard<std::mutex> lock(clipboard_callback_mutex);
            callback = m_clipboardCallback;
        }
        if (callback) {
            callback(type, files, text);
        }
    }


#if INPUT_BRIDGE_HAS_LIBEI
    struct EISenderState {
        ei* context = nullptr;
        ei_seat* seat = nullptr;
        ei_device* device = nullptr;
        bool seat_requested = false;
        bool connected = false;
        bool pending_disconnect = false;
        bool started = false;
        bool has_pointer = false;
        bool has_pointer_absolute = false;
        bool has_button = false;
        bool has_keyboard = false;
        bool has_scroll = false;
        uint32_t sequence = 0;
    };

    EISenderState m_eis;

    static std::string EncodeUTF8(uint32_t codepoint) {
        std::string out;
        if (codepoint <= 0x7F) {
            out.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint <= 0x10FFFF) {
            out.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
        return out;
    }

    bool HasEISDevice() const {
        return m_eis.connected && m_eis.started && m_eis.context != nullptr && m_eis.device != nullptr;
    }

    void ReleaseEISState() {
        if (m_eis.device) {
            if (m_eis.started) {
                ei_device_stop_emulating(m_eis.device);
            }
            ei_device_close(m_eis.device);
            ei_device_unref(m_eis.device);
            m_eis.device = nullptr;
        }
        if (m_eis.seat) {
            ei_seat_unref(m_eis.seat);
            m_eis.seat = nullptr;
        }
        if (m_eis.context) {
            ei_disconnect(m_eis.context);
            ei_unref(m_eis.context);
            m_eis.context = nullptr;
        }
        m_eis.connected = false;
        m_eis.pending_disconnect = false;
        m_eis.seat_requested = false;
        m_eis.started = false;
        eis_connected.store(false, std::memory_order_relaxed);
    }

    void EISDispatchPending() {
        if (!m_eis.context) {
            return;
        }

        const int fd = ei_get_fd(m_eis.context);
        if (fd < 0) {
            return;
        }

        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
            ei_dispatch(m_eis.context);

            for (;;) {
                ei_event* event = ei_get_event(m_eis.context);
                if (!event) {
                    break;
                }

                switch (ei_event_get_type(event)) {
                    case EI_EVENT_SEAT_ADDED: {
                        if (!m_eis.seat) {
                            m_eis.seat = ei_event_get_seat(event);
                            if (m_eis.seat) {
                                ei_seat_ref(m_eis.seat);
                                ei_seat_bind_capabilities(
                                    m_eis.seat,
                                    EI_DEVICE_CAP_POINTER,
                                    EI_DEVICE_CAP_POINTER_ABSOLUTE,
                                    EI_DEVICE_CAP_BUTTON,
                                    EI_DEVICE_CAP_KEYBOARD,
                                    EI_DEVICE_CAP_SCROLL,
                                    NULL
                                );
                            }
                            m_eis.seat_requested = true;
                        }
                        break;
                    }
                    case EI_EVENT_DEVICE_ADDED: {
                        if (!m_eis.device) {
                            m_eis.device = ei_event_get_device(event);
                            if (m_eis.device) {
                                ei_device_ref(m_eis.device);
                                m_eis.connected = true;
                                // Query capabilities and cache them to avoid calling
                                // keyboard APIs on non-keyboard devices.
                                m_eis.has_pointer = ei_device_has_capability(m_eis.device, EI_DEVICE_CAP_POINTER);
                                m_eis.has_pointer_absolute = ei_device_has_capability(m_eis.device, EI_DEVICE_CAP_POINTER_ABSOLUTE);
                                m_eis.has_button = ei_device_has_capability(m_eis.device, EI_DEVICE_CAP_BUTTON);
                                m_eis.has_keyboard = ei_device_has_capability(m_eis.device, EI_DEVICE_CAP_KEYBOARD);
                                m_eis.has_scroll = ei_device_has_capability(m_eis.device, EI_DEVICE_CAP_SCROLL);
                                Log(std::string("EIS device added; capabilities: ") +
                                    (m_eis.has_pointer ? "pointer " : "") +
                                    (m_eis.has_pointer_absolute ? "pointer_abs " : "") +
                                    (m_eis.has_button ? "button " : "") +
                                    (m_eis.has_keyboard ? "keyboard " : "") +
                                    (m_eis.has_scroll ? "scroll " : "")
                                );
                            }
                        }
                        break;
                    }
                    case EI_EVENT_DEVICE_RESUMED: {
                        if (m_eis.device && !m_eis.started) {
                            if (m_eis.sequence == 0) {
                                m_eis.sequence = 1;
                            }
                            ei_device_start_emulating(m_eis.device, m_eis.sequence++);
                            m_eis.started = true;
                            m_eis.connected = true;
                            eis_connected.store(true, std::memory_order_relaxed);
                        }
                        break;
                    }
                    case EI_EVENT_DEVICE_PAUSED: {
                        if (m_eis.device && m_eis.started) {
                            ei_device_stop_emulating(m_eis.device);
                            m_eis.started = false;
                            eis_connected.store(false, std::memory_order_relaxed);
                        }
                        break;
                    }
                    case EI_EVENT_DEVICE_REMOVED:
                    case EI_EVENT_SEAT_REMOVED:
                    case EI_EVENT_DISCONNECT: {
                        m_eis.pending_disconnect = true;
                        break;
                    }
                    default:
                        break;
                }

                ei_event_unref(event);
            }
        }

        if (m_eis.pending_disconnect) {
            ReleaseEISState();
        }
    }

    bool SendEISKeycode(uint32_t evdev_code, bool down) {
        if (!HasEISDevice()) {
            return false;
        }
        if (!m_eis.has_keyboard) {
            Log(std::string("EIS: device lacks keyboard capability, falling back"));
            return false;
        }
        ei_device_keyboard_key(m_eis.device, evdev_code, down);
        EISDispatchPending();
        EISFrame();
        return true;
    }

    void EISFrame() {
        if (!HasEISDevice()) {
            return;
        }
        ei_device_frame(m_eis.device, ei_now(m_eis.context));
    }

    bool SendEISButton(uint32_t button, bool down) {
        if (!HasEISDevice()) {
            return false;
        }
        ei_device_button_button(m_eis.device, button, down);
        EISDispatchPending();
        EISFrame();
        return true;
    }

    bool SendEISRelativeMotion(double x, double y) {
        if (!HasEISDevice()) {
            return false;
        }
        ei_device_pointer_motion(m_eis.device, x, y);
        EISDispatchPending();
        EISFrame();
        return true;
    }

    bool SendEISAbsoluteMotion(double x, double y) {
        if (!HasEISDevice()) {
            return false;
        }
        ei_device_pointer_motion_absolute(m_eis.device, x, y);
        EISDispatchPending();
        EISFrame();
        return true;
    }

    bool SendEISScroll(int32_t delta) {
        if (!HasEISDevice()) {
            return false;
        }
        ei_device_scroll_discrete(m_eis.device, 0, delta * 120);
        EISDispatchPending();
        EISFrame();
        return true;
    }

    bool SendEISText(uint32_t codepoint) {
        return false;
    }

    void SendEISUnicodeFallback(uint32_t codepoint) {
        if (!HasEISDevice()) {
            return;
        }

        auto key_down = [this](uint32_t evdev) {
            if (!HasEISDevice()) {
                return;
            }
            if (m_eis.has_keyboard) {
                // Send via EIS
                SendEISKeycode(evdev, true);
            } else {
                // Fallback to portal NotifyKeyboardKeycode (guarded)
                if (this->portal_notify_allowed) {
                    bool ok = SendKeyboardKeycode(this->connection, this->GetSessionHandle(), evdev, true);
                    if (!ok) {
                        this->portal_notify_allowed = false;
                        Log("EIS: portal NotifyKeyboardKeycode not allowed; disabling Notify fallback");
                    }
                }
            }
        };

        auto key_up = [this](uint32_t evdev) {
            if (!HasEISDevice()) {
                return;
            }
            if (m_eis.has_keyboard) {
                SendEISKeycode(evdev, false);
            } else {
                if (this->portal_notify_allowed) {
                    bool ok = SendKeyboardKeycode(this->connection, this->GetSessionHandle(), evdev, false);
                    if (!ok) {
                        this->portal_notify_allowed = false;
                        Log("EIS: portal NotifyKeyboardKeycode not allowed; disabling Notify fallback");
                    }
                }
            }
        };

        auto tap_key = [this, &key_down, &key_up](uint32_t evdev) {
            key_down(evdev);
            EISFrame();
            key_up(evdev);
            EISFrame();
        };

        if (codepoint == 0x0D || codepoint == 0x0A) { tap_key(28); return; }
        if (codepoint == 0x08) { tap_key(14); return; }
        if (codepoint == 0x09) { tap_key(15); return; }
        if (codepoint == 0x20) { tap_key(57); return; }

        char hex[10];
        snprintf(hex, sizeof(hex), "%x", codepoint);

        key_down(29); // Left Ctrl
        key_down(42); // Left Shift
        EISFrame();
        tap_key(22);  // U
        key_up(42);
        key_up(29);
        EISFrame();

        for (int i = 0; hex[i] != '\0'; ++i) {
            uint32_t evdev = 0;
            const char c = hex[i];
            if (c == '0') evdev = 11;
            else if (c == '1') evdev = 2;
            else if (c == '2') evdev = 3;
            else if (c == '3') evdev = 4;
            else if (c == '4') evdev = 5;
            else if (c == '5') evdev = 6;
            else if (c == '6') evdev = 7;
            else if (c == '7') evdev = 8;
            else if (c == '8') evdev = 9;
            else if (c == '9') evdev = 10;
            else if (c == 'a') evdev = 30;
            else if (c == 'b') evdev = 48;
            else if (c == 'c') evdev = 46;
            else if (c == 'd') evdev = 32;
            else if (c == 'e') evdev = 18;
            else if (c == 'f') evdev = 33;

            if (evdev > 0) {
                tap_key(evdev);
            }
        }

        tap_key(57);
    }
#endif


    private:
    GDBusConnection* connection = nullptr;
    std::string session_handle;
    std::atomic<bool> is_session_ready = false;
    bool m_batchMode = false;
    std::vector<MonitorInfo> m_monitors;
    int32_t m_currentMonitorIndex = 0;

    GMainContext* dbus_context = nullptr;
    GMainLoop* main_loop = nullptr;
    std::thread loop_thread;
    std::atomic<bool> is_running{ false };
    guint response_signal_id = 0;

    std::mutex session_mutex;
    std::condition_variable session_cv;

    std::string GetSessionHandle() {
        std::lock_guard<std::mutex> lock(session_mutex);
        return session_handle;
    }

    static MonitorInfo BuildDefaultMonitor() {
        MonitorInfo monitor;
        monitor.index = 0;
        monitor.id = "portal-default";
        monitor.name = "Portal Desktop";
        monitor.x = 0;
        monitor.y = 0;
        monitor.width = 0;
        monitor.height = 0;
        monitor.primary = true;
        return monitor;
    }

    const MonitorInfo& GetCurrentMonitor() const {
        if (m_monitors.empty()) {
            static MonitorInfo fallback = BuildDefaultMonitor();
            return fallback;
        }

        if (m_currentMonitorIndex < 0 || static_cast<size_t>(m_currentMonitorIndex) >= m_monitors.size()) {
            return m_monitors.front();
        }

        return m_monitors[static_cast<size_t>(m_currentMonitorIndex)];
    }

    static bool SendKeyboardKeycode(GDBusConnection* connection, const std::string& handle, uint32_t evdev, bool down) {
        GVariantBuilder options_builder;
        g_variant_builder_init(&options_builder, G_VARIANT_TYPE_VARDICT);

        GError* error = nullptr;
        uint32_t state = down ? 1 : 0;
        GVariant* result = g_dbus_connection_call_sync(
            connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.RemoteDesktop",
            "NotifyKeyboardKeycode",
            g_variant_new("(o@a{sv}iu)", handle.c_str(), g_variant_builder_end(&options_builder), (int32_t)evdev, state),
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            &error
        );

        if (error) {
            std::cerr << "Failed to send NotifyKeyboardKeycode: " << error->message << std::endl;
            g_error_free(error);
            if (result) {
                g_variant_unref(result);
            }
            return false;
        }

        if (result) {
            g_variant_unref(result);
        }
        return true;
    }

    static bool SendAltGrKeycodeSequence(GDBusConnection* connection, const std::string& handle, uint32_t keycode, bool shift) {
        uint32_t altGr = 100; // Right Alt / AltGr
        uint32_t shiftCode = 42; // Left Shift

        if (!SendKeyboardKeycode(connection, handle, altGr, true)) return false;
        if (shift && !SendKeyboardKeycode(connection, handle, shiftCode, true)) {
            SendKeyboardKeycode(connection, handle, altGr, false);
            return false;
        }

        bool ok = SendKeyboardKeycode(connection, handle, keycode, true)
            && SendKeyboardKeycode(connection, handle, keycode, false);

        if (shift) {
            ok = ok && SendKeyboardKeycode(connection, handle, shiftCode, false);
        }

        ok = ok && SendKeyboardKeycode(connection, handle, altGr, false);
        return ok;
    }

    static void OnClipboardSelectionTransfer(GDBusConnection* connection, const gchar* sender_name,
        const gchar* object_path, const gchar* interface_name,
        const gchar* signal_name, GVariant* parameters, gpointer user_data) {

        PlatformInputLinux* self = static_cast<PlatformInputLinux*>(user_data);
        const std::string current_session_handle = self->GetSessionHandle();
        const gchar* session_handle = nullptr;
        const gchar* mime_type = nullptr;
        guint32 serial = 0;

        g_variant_get(parameters, "(&o&su)", &session_handle, &mime_type, &serial);

        if (current_session_handle != session_handle) return;

        std::string mime(mime_type);
        std::string data_to_send;
        {
            std::lock_guard<std::mutex> lock(self->clipboard_mutex);
            auto it = self->m_clipboardData.find(mime);
            if (it != self->m_clipboardData.end()) {
                data_to_send = it->second;
            }
        }

        if (data_to_send.empty()) {
            g_dbus_connection_call_sync(
                self->connection, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                "org.freedesktop.portal.Clipboard", "SelectionWriteDone",
                g_variant_new("(oub)", current_session_handle.c_str(), serial, FALSE),
                nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr
            );
            return;
        }

        GError* error = nullptr;
        GUnixFDList* out_fd_list = nullptr;
        GVariant* result = g_dbus_connection_call_with_unix_fd_list_sync(
            self->connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.Clipboard",
            "SelectionWrite",
            g_variant_new("(ou)", current_session_handle.c_str(), serial),
            G_VARIANT_TYPE("(h)"),
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            &out_fd_list,
            nullptr,
            &error
        );

        if (error) {
            std::cerr << "Failed to call SelectionWrite: " << error->message << std::endl;
            g_error_free(error);
            g_dbus_connection_call_sync(
                self->connection, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                "org.freedesktop.portal.Clipboard", "SelectionWriteDone",
                g_variant_new("(oub)", current_session_handle.c_str(), serial, FALSE),
                nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr
            );
            return;
        }

        gint handle_index;
        g_variant_get(result, "(h)", &handle_index);
        int fd = g_unix_fd_list_get(out_fd_list, handle_index, nullptr);

        if (fd >= 0) {
            const char* data_ptr = data_to_send.data();
            size_t remaining = data_to_send.size();
            while (remaining > 0) {
                ssize_t written = write(fd, data_ptr, remaining);
                if (written <= 0) {
                    break;
                }
                data_ptr += written;
                remaining -= static_cast<size_t>(written);
            }
            close(fd);
        }

        if (out_fd_list) g_object_unref(out_fd_list);
        if (result) g_variant_unref(result);

        g_dbus_connection_call_sync(
            self->connection, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.Clipboard", "SelectionWriteDone",
            g_variant_new("(oub)", current_session_handle.c_str(), serial, TRUE),
            nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr
        );
    }

    static void OnPortalResponse(GDBusConnection* connection, const gchar* sender_name,
        const gchar* object_path, const gchar* interface_name,
        const gchar* signal_name, GVariant* parameters, gpointer user_data) {
        if (!parameters || !object_path) {
            if (!object_path) {
                std::cerr << "OnPortalResponse received null object_path" << std::endl;
            }
            return;
        }

        PlatformInputLinux* self = static_cast<PlatformInputLinux*>(user_data);
        guint32 response = 0;
        GVariant* results = nullptr;

        g_variant_get(parameters, "(u@a{sv})", &response, &results);

        const bool is_create_resp = g_str_has_suffix(object_path, "createReq");
        const bool is_clipboard_resp = g_str_has_suffix(object_path, "clipboardReq");
        const bool is_select_resp = g_str_has_suffix(object_path, "selectReq");
        const bool is_start_resp = g_str_has_suffix(object_path, "startReq");

        std::cout << "Portal Response signal from: " << object_path
            << " response=" << response << std::endl;

        if (is_create_resp) {
            if (response == 0) {
                if (!results) {
                    std::cerr << "Portal session creation response missing results dictionary." << std::endl;
                    self->session_cv.notify_all();
                } else {
                    GVariant* handle_v = g_variant_lookup_value(results, "session_handle", G_VARIANT_TYPE_STRING);
                    if (handle_v) {
                        const gchar* session_path = g_variant_get_string(handle_v, nullptr);
                        if (session_path) {
                            std::string session_handle_str = session_path;
                            if (!session_handle_str.empty() && session_handle_str[0] != '/') {
                                const char* sender = g_dbus_connection_get_unique_name(self->connection);
                                if (sender) {
                                    std::string sender_str = sender;
                                    if (!sender_str.empty() && sender_str[0] == ':') {
                                        sender_str.erase(0, 1);
                                    }
                                    if (!sender_str.empty() && sender_str[0] >= '0' && sender_str[0] <= '9') {
                                        sender_str = "u" + sender_str;
                                    }
                                    for (char& c : sender_str) {
                                        if (c == '.') c = '_';
                                    }
                                    session_handle_str = "/org/freedesktop/portal/desktop/session/" + sender_str + "/" + session_handle_str;
                                }
                            }
                            std::cout << "Portal session created! Session path: " << session_handle_str << std::endl;
                            {
                                std::lock_guard<std::mutex> lock(self->session_mutex);
                                self->session_handle = session_handle_str;
                            }
                            SelectDevices(self);
                        } else {
                            std::cerr << "Portal response session_handle string was null." << std::endl;
                            self->session_cv.notify_all();
                        }
                        g_variant_unref(handle_v);
                    } else {
                        std::cerr << "Portal response missing session_handle despite success." << std::endl;
                        self->session_cv.notify_all();
                    }
                }
            } else {
                std::cerr << "Portal session creation denied. Response: " << response << std::endl;
                self->session_cv.notify_all();
            }
        } else if (is_clipboard_resp) {
            if (response == 0) {
                std::cout << "Clipboard access granted." << std::endl;
                self->clipboard_signal_id = g_dbus_connection_signal_subscribe(
                    self->connection,
                    "org.freedesktop.portal.Desktop",
                    "org.freedesktop.portal.Clipboard",
                    "SelectionTransfer",
                    "/org/freedesktop/portal/desktop",
                    nullptr,
                    G_DBUS_SIGNAL_FLAGS_NONE,
                    OnClipboardSelectionTransfer,
                    self,
                    nullptr
                );
                StartSession(self);
            } else {
                std::cerr << "Clipboard access denied. Continuing without clipboard. Response: " << response << std::endl;
                self->session_cv.notify_all();
            }
        } else if (is_select_resp) {
            if (response == 0) {
                std::cout << "SelectDevices completed successfully." << std::endl;
                RequestClipboard(self);
            } else {
                std::cerr << "SelectDevices denied. Response: " << response << std::endl;
                self->session_cv.notify_all();
            }
        } else if (is_start_resp) {
            if (response == 0) {
                std::cout << "RemoteDesktop session successfully STARTed! Ready for input injection." << std::endl;
                self->is_session_ready.store(true);
                if (self->input_mode == InputMode::EIS) {
                    std::string eis_error;
                    if (!self->ConnectToEIS(eis_error)) {
                        std::cerr << "EIS mode requested but ConnectToEIS failed: " << eis_error
                            << ". Falling back to notify mode." << std::endl;
                        self->input_mode = InputMode::Notify;
                    }
                }
                self->session_cv.notify_all();
            } else {
                std::cerr << "RemoteDesktop session failed to START. Response: " << response << std::endl;
                self->session_cv.notify_all();
            }
        } else {
            std::cout << "Ignoring portal response for unknown request object: " << object_path << std::endl;
        }

        if (results) {
            g_variant_unref(results);
        }
    }

    static void StartSession(PlatformInputLinux* self) {
        GError* error = nullptr;

        const std::string session_handle = self->GetSessionHandle();
        std::cout << "Calling RemoteDesktop.Start on session " << session_handle << "..." << std::endl;

        GVariantBuilder options_builder;
        g_variant_builder_init(&options_builder, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&options_builder, "{sv}", "handle_token", g_variant_new_string("startReq"));

        GVariant* start_result = g_dbus_connection_call_sync(
            self->connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.RemoteDesktop",
            "Start",
            g_variant_new("(os@a{sv})", session_handle.c_str(), "", g_variant_builder_end(&options_builder)),
            G_VARIANT_TYPE("(o)"),
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            &error
        );

        if (error) {
            std::cerr << "Failed to Start session: " << error->message << std::endl;
            g_error_free(error);
            {
                std::lock_guard<std::mutex> lock(self->session_mutex);
                self->is_session_ready = false;
            }
            self->session_cv.notify_all();
        } else {
            const gchar* request_path = nullptr;
            g_variant_get(start_result, "(&o)", &request_path);
            std::cout << "Start requested successfully. Request path: " << request_path << std::endl;
            g_variant_unref(start_result);
        }
    }

    static void RequestClipboard(PlatformInputLinux* self) {
        GError* error = nullptr;
        const std::string session_handle = self->GetSessionHandle();
        std::cout << "Calling RequestClipboard on session " << session_handle << "..." << std::endl;

        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string("clipboardReq"));

        GVariant* result = g_dbus_connection_call_sync(
            self->connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.Clipboard",
            "RequestClipboard",
            g_variant_new("(o@a{sv})", session_handle.c_str(), g_variant_builder_end(&builder)),
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            &error
        );

        if (error) {
            std::cerr << "Failed to RequestClipboard: " << error->message << std::endl;
            g_error_free(error);
        } else {
            if (self->clipboard_signal_id == 0) {
                self->clipboard_signal_id = g_dbus_connection_signal_subscribe(
                    self->connection,
                    "org.freedesktop.portal.Desktop",
                    "org.freedesktop.portal.Clipboard",
                    "SelectionTransfer",
                    "/org/freedesktop/portal/desktop",
                    nullptr,
                    G_DBUS_SIGNAL_FLAGS_NONE,
                    OnClipboardSelectionTransfer,
                    self,
                    nullptr
                );
            }
            if (result && g_variant_is_of_type(result, G_VARIANT_TYPE("(o)")) && g_variant_n_children(result) == 1) {
                const gchar* request_path = nullptr;
                g_variant_get(result, "(&o)", &request_path);
                std::cout << "RequestClipboard returned request path: " << (request_path ? request_path : "null") << std::endl;
            } else {
                std::cout << "RequestClipboard completed successfully." << std::endl;
            }

            std::cout << "RequestClipboard finished, starting remote desktop session now..." << std::endl;
            StartSession(self);
        }

        if (result) {
            g_variant_unref(result);
        }
    }

    static void SelectDevices(PlatformInputLinux* self) {
        GError* error = nullptr;
        const std::string session_handle = self->GetSessionHandle();
        std::cout << "Calling RemoteDesktop.SelectDevices on session " << session_handle << "..." << std::endl;

        if (!g_variant_is_object_path(session_handle.c_str())) {
            std::cerr << "CRITICAL: Invalid session path: " << session_handle << std::endl;
            self->session_cv.notify_all();
            return;
        }

        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);

        uint32_t types = 3;

        g_variant_builder_add(&builder, "{sv}", "types", g_variant_new_uint32(types));
        g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string("selectReq"));

        GVariant* result = g_dbus_connection_call_sync(
            self->connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.RemoteDesktop",
            "SelectDevices",
            g_variant_new("(o@a{sv})", session_handle.c_str(), g_variant_builder_end(&builder)),
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            &error
        );

        if (error) {
            std::cerr << "Failed to SelectDevices: " << error->message << std::endl;
            g_error_free(error);
            self->session_cv.notify_all();
        } else if (result) {
            if (g_variant_is_of_type(result, G_VARIANT_TYPE("(o)")) && g_variant_n_children(result) == 1) {
                const gchar* request_path = nullptr;
                g_variant_get(result, "(&o)", &request_path);
                std::cout << "SelectDevices returned request path: " << (request_path ? request_path : "null") << std::endl;
            } else {
                std::cout << "SelectDevices returned unexpected result type." << std::endl;
            }
            g_variant_unref(result);
        }
    }

    bool Initialize(std::string& error_msg) override {
        std::promise<bool> dbus_ready;
        auto dbus_future = dbus_ready.get_future();

        dbus_context = g_main_context_new();
        loop_thread = std::thread([this, &dbus_ready]() {
            g_main_context_push_thread_default(this->dbus_context);

            GError* error = nullptr;
            this->connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
            if (!this->connection) {
                std::cerr << "Failed to connect to D-Bus session bus: "
                    << (error ? error->message : "unknown") << std::endl;
                if (error) {
                    g_error_free(error);
                }
                dbus_ready.set_value(false);
                g_main_context_pop_thread_default(this->dbus_context);
                return;
            }

            std::cout << "Successfully connected to D-Bus session bus." << std::endl;

            this->response_signal_id = g_dbus_connection_signal_subscribe(
                this->connection,
                "org.freedesktop.portal.Desktop",
                "org.freedesktop.portal.Request",
                "Response",
                nullptr,
                nullptr,
                G_DBUS_SIGNAL_FLAGS_NONE,
                OnPortalResponse,
                this,
                nullptr
            );

            this->main_loop = g_main_loop_new(this->dbus_context, FALSE);
            dbus_ready.set_value(true);
            g_main_loop_run(this->main_loop);
            g_main_context_pop_thread_default(this->dbus_context);
            });

        if (!dbus_future.get()) {
            if (loop_thread.joinable()) {
                loop_thread.join();
            }
            error_msg = "Failed to initialize DBus thread";
            return false;
        }

        is_running = true;

        // Parameters for CreateSession (e.g. token so we know the Request ID)
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&builder, "{sv}", "session_handle_token", g_variant_new_string("inputbridgesession"));
        g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string("createReq"));


        // Call CreateSession
        std::cout << "Requesting RemoteDesktop session..." << std::endl;
        GError* error = nullptr;
        GVariant* result = g_dbus_connection_call_sync(
            connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.RemoteDesktop",
            "CreateSession",
            g_variant_new("(@a{sv})", g_variant_builder_end(&builder)),
            G_VARIANT_TYPE("(o)"),
            G_DBUS_CALL_FLAGS_NONE,
            -1, // default timeout
            nullptr,
            &error
        );

        if (error != nullptr) {
            error_msg = std::string("Failed to call CreateSession: ") + error->message;
            g_error_free(error);
            return false;
        } else {
            const gchar* request_path = nullptr;
            g_variant_get(result, "(&o)", &request_path);
            std::cout << "CreateSession requested successfully. Request path: " << request_path << std::endl;
            g_variant_unref(result);

            // Waiting for the portal authorization chain to complete (requires system or user action), max 10 seconds
            std::cout << "Waiting (max 45 seconds) for the Portal session to start and external permissions to be granted..." << std::endl;
            std::unique_lock<std::mutex> lock(session_mutex);
            if (session_cv.wait_for(lock, std::chrono::seconds(45), [this] { return this->is_session_ready.load(std::memory_order_relaxed); })) {
                std::cout << "Portal authorized immediately! JS code can continue." << std::endl;
                return true;
            } else {
                error_msg = "Timeout expired or was denied! Authorization failed.";
                return false;
            }
        }
    }

    public:

    PlatformInputLinux() {
        // This object will initialize the session via Async Init
        m_monitors.push_back(BuildDefaultMonitor());
    }

    ~PlatformInputLinux() {
        DisconnectEIS();
        if (main_loop) {
            g_main_loop_quit(main_loop);
            if (loop_thread.joinable()) {
                loop_thread.join();
            }
            g_main_loop_unref(main_loop);
        }
        if (dbus_context) {
            g_main_context_unref(dbus_context);
        }
        if (response_signal_id > 0 && connection) {
            g_dbus_connection_signal_unsubscribe(connection, response_signal_id);
        }
        if (clipboard_signal_id > 0 && connection) {
            g_dbus_connection_signal_unsubscribe(connection, clipboard_signal_id);
        }
        if (connection) {
            g_object_unref(connection);
        }
    }

    void MoveMouseRelative(int32_t x, int32_t y) override {
        if (!is_session_ready) return;
#if INPUT_BRIDGE_HAS_LIBEI
        if (eis_connected.load(std::memory_order_relaxed)) {
            EISDispatchPending();
            if (HasEISDevice()) {
                SendEISRelativeMotion(static_cast<double>(x), static_cast<double>(y));
                return;
            }
        }
#endif

        const std::string session_handle = GetSessionHandle();
        GVariantBuilder options_builder;
        g_variant_builder_init(&options_builder, G_VARIANT_TYPE_VARDICT);

        if (m_batchMode) {
            GError* error = nullptr;
            GVariant* result = g_dbus_connection_call_sync(
                connection,
                "org.freedesktop.portal.Desktop",
                "/org/freedesktop/portal/desktop",
                "org.freedesktop.portal.RemoteDesktop",
                "NotifyPointerMotion",
                g_variant_new("(o@a{sv}dd)", session_handle.c_str(), g_variant_builder_end(&options_builder), (double)x, (double)y),
                nullptr,
                G_DBUS_CALL_FLAGS_NONE,
                -1,
                nullptr,
                &error
            );

            if (error) {
                std::cerr << "Failed to send NotifyPointerMotion: " << error->message << std::endl;
                g_error_free(error);
            }

            if (result) {
                g_variant_unref(result);
            }
            return;
        }

        g_dbus_connection_call(
            connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.RemoteDesktop",
            "NotifyPointerMotion",
            g_variant_new("(o@a{sv}dd)", session_handle.c_str(), g_variant_builder_end(&options_builder), (double)x, (double)y),
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            nullptr,
            nullptr
        );
    }

    void MoveMouseAbsolute(int32_t x, int32_t y) override {
        if (!is_session_ready) return;
#if INPUT_BRIDGE_HAS_LIBEI
        if (eis_connected.load(std::memory_order_relaxed)) {
            EISDispatchPending();
            if (HasEISDevice()) {
                SendEISAbsoluteMotion(static_cast<double>(x), static_cast<double>(y));
                return;
            }
        }
#endif

        const MonitorInfo& monitor = GetCurrentMonitor();

        int32_t localX = x;
        int32_t localY = y;

        // Ograniczamy współrzędne do granic monitora, jeśli jego wymiary są znane
        if (monitor.width > 0) {
            localX = std::max(0, std::min(localX, monitor.width - 1));
        }
        if (monitor.height > 0) {
            localY = std::max(0, std::min(localY, monitor.height - 1));
        }

        const double absoluteX = static_cast<double>(monitor.x + localX);
        const double absoluteY = static_cast<double>(monitor.y + localY);

        const std::string session_handle = GetSessionHandle();
        GVariantBuilder options_builder;
        g_variant_builder_init(&options_builder, G_VARIANT_TYPE_VARDICT);

        if (m_batchMode) {
            GError* error = nullptr;
            GVariant* result = g_dbus_connection_call_sync(
                connection,
                "org.freedesktop.portal.Desktop",
                "/org/freedesktop/portal/desktop",
                "org.freedesktop.portal.RemoteDesktop",
                "NotifyPointerMotionAbsolute",
                g_variant_new("(o@a{sv}udd)", session_handle.c_str(), g_variant_builder_end(&options_builder), 0, absoluteX, absoluteY),
                nullptr,
                G_DBUS_CALL_FLAGS_NONE,
                -1,
                nullptr,
                &error
            );

            if (error) {
                std::cerr << "Failed to send NotifyPointerMotionAbsolute: " << error->message << std::endl;
                g_error_free(error);
            }

            if (result) {
                g_variant_unref(result);
            }
            return;
        }

        g_dbus_connection_call(
            connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.RemoteDesktop",
            "NotifyPointerMotionAbsolute",
            g_variant_new("(o@a{sv}udd)", session_handle.c_str(), g_variant_builder_end(&options_builder), 0, absoluteX, absoluteY),
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            nullptr,
            nullptr
        );
    }

    std::vector<MonitorInfo> GetMonitors() override {
        return m_monitors;
    }

    bool SetCurrentMonitor(int32_t monitorIndex) override {
        if (monitorIndex < 0 || static_cast<size_t>(monitorIndex) >= m_monitors.size()) {
            return false;
        }

        m_currentMonitorIndex = monitorIndex;
        return true;
    }

    void MouseClick(int32_t button, bool down) override {
        if (!is_session_ready) return;
#if INPUT_BRIDGE_HAS_LIBEI
        if (eis_connected.load(std::memory_order_relaxed)) {
            EISDispatchPending();
            if (HasEISDevice()) {
                uint32_t linux_button = 0x110;
                if (button == 1) linux_button = 0x111;
                else if (button == 2) linux_button = 0x112;
                SendEISButton(linux_button, down);
                return;
            }
        }
#endif

        const std::string session_handle = GetSessionHandle();
        GVariantBuilder options_builder;
        g_variant_builder_init(&options_builder, G_VARIANT_TYPE_VARDICT);

        uint32_t state = down ? 1 : 0;
        uint32_t linux_button = 0x110;
        if (button == 1) linux_button = 0x111;
        else if (button == 2) linux_button = 0x112;

        if (m_batchMode) {
            GError* error = nullptr;
            GVariant* result = g_dbus_connection_call_sync(
                connection,
                "org.freedesktop.portal.Desktop",
                "/org/freedesktop/portal/desktop",
                "org.freedesktop.portal.RemoteDesktop",
                "NotifyPointerButton",
                g_variant_new("(o@a{sv}iu)", session_handle.c_str(), g_variant_builder_end(&options_builder), (int32_t)linux_button, state),
                nullptr,
                G_DBUS_CALL_FLAGS_NONE,
                -1,
                nullptr,
                &error
            );

            if (error) {
                std::cerr << "Failed to send NotifyPointerButton: " << error->message << std::endl;
                g_error_free(error);
            }

            if (result) {
                g_variant_unref(result);
            }
            return;
        }

        g_dbus_connection_call(
            connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.RemoteDesktop",
            "NotifyPointerButton",
            g_variant_new("(o@a{sv}iu)", session_handle.c_str(), g_variant_builder_end(&options_builder), (int32_t)linux_button, state),
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            nullptr,
            nullptr
        );
    }

    void KeyPress(int32_t keyCode, bool down) override {
        if (!is_session_ready) return;
#if INPUT_BRIDGE_HAS_LIBEI
        if (eis_connected.load(std::memory_order_relaxed)) {
            EISDispatchPending();
            if (HasEISDevice()) {
                uint32_t evdev_code = 0;

                if ((keyCode & FLAG_RAW_MASK) == FLAG_RAW_LINUX) {
                    evdev_code = keyCode & ~FLAG_RAW_MASK;
                } else if ((keyCode & FLAG_RAW_MASK) == FLAG_RAW_WINDOWS) {
                    evdev_code = KeyTranslator::WindowsToLinux(keyCode & ~FLAG_RAW_MASK);
                } else {
                    evdev_code = KeyTranslator::WindowsToLinux(keyCode);
                }

                if (evdev_code == 0) {
                    std::cerr << "[EIS WARN] Unrecognized key code: " << keyCode << ". Dropping input." << std::endl;
                    return;
                }

                // Try sending via EIS; if device doesn't support keyboard, fall back to Notify
                if (SendEISKeycode(evdev_code, down)) {
                    return;
                }
                // else fall through to portal NotifyKeyboardKeycode fallback
            }
        }
#endif

        const std::string session_handle = GetSessionHandle();
        GVariantBuilder options_builder;
        g_variant_builder_init(&options_builder, G_VARIANT_TYPE_VARDICT);

        uint32_t state = down ? 1 : 0;
        uint32_t evdev_code = 0;

        // Check if we received the raw Linux portal field marker
        if ((keyCode & FLAG_RAW_MASK) == FLAG_RAW_LINUX) {
            evdev_code = keyCode & ~FLAG_RAW_MASK;
        }
        // Or if this is raw Windows code that must be translated to Linux for this module
        else if ((keyCode & FLAG_RAW_MASK) == FLAG_RAW_WINDOWS) {
            evdev_code = KeyTranslator::WindowsToLinux(keyCode & ~FLAG_RAW_MASK);
        }
        // Default input case from Node.js library (in JS it is always a Windows Virtual-Key)
        else {
            evdev_code = KeyTranslator::WindowsToLinux(keyCode);
        }

        // If the translator did not recognize the key, safely drop the injection attempt
        if (evdev_code == 0) {
            std::cerr << "[DBUS WARN] Nierozpoznany kod klawisza: " << keyCode << ". Pomijam zastrzyk." << std::endl;
            return;
        }

        if (m_batchMode) {
            GError* error = nullptr;
            GVariant* result = g_dbus_connection_call_sync(
                connection,
                "org.freedesktop.portal.Desktop",
                "/org/freedesktop/portal/desktop",
                "org.freedesktop.portal.RemoteDesktop",
                "NotifyKeyboardKeycode",
                g_variant_new("(o@a{sv}iu)", session_handle.c_str(), g_variant_builder_end(&options_builder), (int32_t)evdev_code, state),
                nullptr,
                G_DBUS_CALL_FLAGS_NONE,
                -1,
                nullptr,
                &error
            );

            if (error) {
                std::cerr << "Failed to send NotifyKeyboardKeycode: " << error->message << std::endl;
                g_error_free(error);
            }

            if (result) {
                g_variant_unref(result);
            }
            return;
        }

        g_dbus_connection_call(
            connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.RemoteDesktop",
            "NotifyKeyboardKeycode",
            g_variant_new("(o@a{sv}iu)", session_handle.c_str(), g_variant_builder_end(&options_builder), (int32_t)evdev_code, state),
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            nullptr,
            nullptr
        );
    }

    void ScrollMouse(int32_t delta) {
        if (!is_session_ready) return;
#if INPUT_BRIDGE_HAS_LIBEI
        if (eis_connected.load(std::memory_order_relaxed)) {
            EISDispatchPending();
            if (HasEISDevice()) {
                SendEISScroll(delta);
                return;
            }
        }
#endif

        const std::string session_handle = GetSessionHandle();
        GVariantBuilder options_builder;
        g_variant_builder_init(&options_builder, G_VARIANT_TYPE_VARDICT);

        if (m_batchMode) {
            GError* error = nullptr;
            GVariant* result = g_dbus_connection_call_sync(
                connection,
                "org.freedesktop.portal.Desktop",
                "/org/freedesktop/portal/desktop",
                "org.freedesktop.portal.RemoteDesktop",
                "NotifyPointerAxisDiscrete",
                g_variant_new("(o@a{sv}ui)", session_handle.c_str(), g_variant_builder_end(&options_builder), 0u, delta),
                nullptr,
                G_DBUS_CALL_FLAGS_NONE,
                -1,
                nullptr,
                &error
            );

            if (error) {
                std::cerr << "Failed to send NotifyPointerAxisDiscrete: " << error->message << std::endl;
                g_error_free(error);
            }

            if (result) {
                g_variant_unref(result);
            }
            return;
        }

        g_dbus_connection_call(
            connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.RemoteDesktop",
            "NotifyPointerAxisDiscrete",
            g_variant_new("(o@a{sv}ui)", session_handle.c_str(), g_variant_builder_end(&options_builder), 0u, delta),
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            nullptr,
            nullptr
        );
    }

    void TypeCharacter(uint32_t charCode) override {
        if (!is_session_ready) return;
#if INPUT_BRIDGE_HAS_LIBEI
        if (eis_connected.load(std::memory_order_relaxed)) {
            EISDispatchPending();
            if (HasEISDevice()) {

                SendEISUnicodeFallback(charCode);
                return;
            }
        }
#endif

        const std::string session_handle = GetSessionHandle();
        uint32_t codepoint = static_cast<uint32_t>(charCode);

        auto sleep_ms = [](int ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            };

        auto send_key = [&](uint32_t evdev, bool down) {
            GVariantBuilder options_builder;
            g_variant_builder_init(&options_builder, G_VARIANT_TYPE_VARDICT);
            uint32_t state = down ? 1 : 0;
            GVariant* result = g_dbus_connection_call_sync(
                connection,
                "org.freedesktop.portal.Desktop",
                "/org/freedesktop/portal/desktop",
                "org.freedesktop.portal.RemoteDesktop",
                "NotifyKeyboardKeycode",
                g_variant_new("(o@a{sv}iu)", session_handle.c_str(), g_variant_builder_end(&options_builder), (int32_t)evdev, state),
                nullptr,
                G_DBUS_CALL_FLAGS_NONE,
                -1,
                nullptr,
                nullptr
            );
            if (result) g_variant_unref(result);
            sleep_ms(6);
            };

        auto tap_key = [&](uint32_t evdev) {
            send_key(evdev, true);
            send_key(evdev, false);
            };

        if (codepoint == 0x0D || codepoint == 0x0A) { tap_key(28); return; } // Enter
        if (codepoint == 0x08) { tap_key(14); return; } // Backspace
        if (codepoint == 0x09) { tap_key(15); return; } // Tab
        if (codepoint == 0x20) { tap_key(57); return; } // Space

        // Fallback: NotifyKeyboardKeysym usually works well for basic ASCII
        if (codepoint >= 0x20 && codepoint <= 0x7E) {
            GVariantBuilder options_builder1;
            g_variant_builder_init(&options_builder1, G_VARIANT_TYPE_VARDICT);
            g_dbus_connection_call_sync(connection, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                "org.freedesktop.portal.RemoteDesktop", "NotifyKeyboardKeysym",
                g_variant_new("(o@a{sv}iu)", session_handle.c_str(), g_variant_builder_end(&options_builder1), (int32_t)codepoint, 1),
                nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);

            sleep_ms(6);

            GVariantBuilder options_builder2;
            g_variant_builder_init(&options_builder2, G_VARIANT_TYPE_VARDICT);
            g_dbus_connection_call_sync(connection, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                "org.freedesktop.portal.RemoteDesktop", "NotifyKeyboardKeysym",
                g_variant_new("(o@a{sv}iu)", session_handle.c_str(), g_variant_builder_end(&options_builder2), (int32_t)codepoint, 0),
                nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);

            sleep_ms(12);
            return;
        }

        // Dla znaków wykraczających poza layout (Unicode) używamy Ctrl+Shift+U + Hex + Space
        char hex[10];
        snprintf(hex, sizeof(hex), "%x", codepoint);

        send_key(29, true); // Left Ctrl
        send_key(42, true); // Left Shift
        sleep_ms(14);
        tap_key(22);        // U (evdev 22)
        sleep_ms(16);
        send_key(42, false);
        send_key(29, false);
        sleep_ms(20);

        for (int i = 0; hex[i] != '\0'; i++) {
            uint32_t evdev = 0;
            char c = hex[i];
            if (c == '0') evdev = 11;
            else if (c == '1') evdev = 2;
            else if (c == '2') evdev = 3;
            else if (c == '3') evdev = 4;
            else if (c == '4') evdev = 5;
            else if (c == '5') evdev = 6;
            else if (c == '6') evdev = 7;
            else if (c == '7') evdev = 8;
            else if (c == '8') evdev = 9;
            else if (c == '9') evdev = 10;
            else if (c == 'a') evdev = 30;
            else if (c == 'b') evdev = 48;
            else if (c == 'c') evdev = 46;
            else if (c == 'd') evdev = 32;
            else if (c == 'e') evdev = 18;
            else if (c == 'f') evdev = 33;

            if (evdev > 0) {
                tap_key(evdev);
                sleep_ms(9);
            }
        }

        tap_key(57); // Space to finish
        sleep_ms(35);
    }

    void ExecuteEvents(const std::vector<InputEvent>& events) override {
        bool previousBatchMode = m_batchMode;
        m_batchMode = true;
        IPlatformInput::ExecuteEvents(events);
        m_batchMode = previousBatchMode;
    }
};
