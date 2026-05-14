#include "../platform_input.hpp"
#include "../key_translator.hpp"
#include "linux_uinput_injector.hpp"
#include "linux_wl_clipboard.hpp"
#include <iostream>
#include <string>
#include <sstream>
#include <cstdio>
#include <cstdlib>
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
#include <regex>
#include <fcntl.h>
#include <cctype>

#include <glib.h>

#if defined(INPUT_BRIDGE_DISABLE_LIBEI)
#pragma message(">>> COMPILING WITH DISABLED LIBEI SUPPORT<<<")
#warning "LIBEI support disabled"
#  define INPUT_BRIDGE_HAS_LIBEI 0
#else
#  if defined(__has_include)
#    if __has_include(<libei.h>)
#      include <libei.h>
#      define INPUT_BRIDGE_HAS_LIBEI 1
#    elif __has_include(<libei/libei.h>)
#      include <libei/libei.h>
#      define INPUT_BRIDGE_HAS_LIBEI 1
#    else
#      define INPUT_BRIDGE_HAS_LIBEI 0
#    endif
#  else
#    define INPUT_BRIDGE_HAS_LIBEI 0
#  endif
#endif

#include <algorithm>

namespace {

std::string SanitizeClipboardBasename(const std::string& file_name) {
    std::string base = file_name;
    const size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) {
        base = base.substr(slash + 1);
    }
    std::string out;
    out.reserve(base.size());
    for (unsigned char c : base) {
        if (std::isalnum(c) != 0 || c == '.' || c == '_' || c == '-') {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            out.push_back('_');
        }
    }
    if (out.empty() || out == "." || out == "..") {
        out = "file.bin";
    }
    return out;
}

bool WriteClipboardTempFile(const std::vector<uint8_t>& bytes, const std::string& basename_hint, std::string& out_abs_path) {
    const std::string safe = SanitizeClipboardBasename(basename_hint);
    GError* err = nullptr;
    gchar* tmpl = g_strdup_printf("input-bridge-clipboard-XXXXXX-%s", safe.c_str());
    gchar* path_used = nullptr;
    const int fd = g_file_open_tmp(tmpl, &path_used, &err);
    g_free(tmpl);
    if (fd < 0) {
        if (err) {
            g_error_free(err);
        }
        return false;
    }
    if (!bytes.empty()) {
        const ssize_t n = static_cast<ssize_t>(write(fd, bytes.data(), bytes.size()));
        if (n != static_cast<ssize_t>(bytes.size())) {
            close(fd);
            unlink(path_used);
            g_free(path_used);
            return false;
        }
    }
    close(fd);
    out_abs_path.assign(path_used);
    g_free(path_used);
    return true;
}

std::string BuildUriListFromAbsPaths(const std::vector<std::string>& abs_paths) {
    std::string payload;
    for (const auto& file : abs_paths) {
        GError* err = nullptr;
        gchar* uri = g_filename_to_uri(file.c_str(), nullptr, &err);
        if (!uri) {
            if (err) {
                g_error_free(err);
            }
            continue;
        }
        payload += uri;
        payload += "\r\n";
        g_free(uri);
    }
    return payload;
}

std::string TrimWhitespace(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    size_t i = 0;
    while (i < s.size() && (s[i] == '\n' || s[i] == '\r' || s[i] == ' ' || s[i] == '\t')) {
        ++i;
    }
    if (i > 0) {
        s.erase(0, i);
    }
    return s;
}

} // namespace

class PlatformInputLinux : public IPlatformInput {

    enum class InputMode {
        Notify,
        EIS
    };

    enum class ActiveBackend {
        Portal,
        LibeiSocket,
        Uinput
    };

    enum class KeyboardRouting {
        EIS,
        Fallback
    };

    InputMode input_mode = InputMode::Notify;
    KeyboardRouting keyboard_routing = KeyboardRouting::EIS;
    bool allow_notify_keyboard = true;
    bool allow_notify_pointer = true;
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

    bool SetBackendMethods(const BackendMethods& methods, std::string& error_msg) override {
        (void)error_msg;
        keyboard_routing = (methods.keyboardMethod == KeyboardMethod::Fallback)
            ? KeyboardRouting::Fallback
            : KeyboardRouting::EIS;
        allow_notify_keyboard = methods.allowNotifyKeyboard;
        allow_notify_pointer = methods.allowNotifyPointer;
        return true;
    }

    BackendMethods GetBackendMethods() const override {
        BackendMethods methods;
        methods.keyboardMethod = keyboard_routing == KeyboardRouting::EIS ? KeyboardMethod::EIS : KeyboardMethod::Fallback;
        methods.allowNotifyKeyboard = allow_notify_keyboard;
        methods.allowNotifyPointer = allow_notify_pointer;
        return methods;
    }

    bool ConnectToEIS(std::string& error_msg) override {
        if (m_activeBackend == ActiveBackend::LibeiSocket && eis_connected.load(std::memory_order_relaxed)) {
            return true;
        }
        if (m_activeBackend != ActiveBackend::Portal) {
            error_msg = "Portal ConnectToEIS is unavailable for the active backend.";
            return false;
        }
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
    guint clipboard_owner_changed_signal_id = 0;
    guint m_file_transfer_signal_id = 0;
    std::map<std::string, std::vector<std::string>> m_fileTransferTempByKey;
    std::vector<std::string> m_pendingWlRemoteTempPaths;
    ClipboardChangeCallback m_clipboardCallback;
    std::mutex clipboard_callback_mutex;
    bool m_hasLastClipboardContent = false;
    std::string m_lastClipboardType;
    std::vector<std::string> m_lastClipboardFiles;
    std::string m_lastClipboardText;
    InputEventCallback m_inputCallback;
    std::mutex input_callback_mutex;

    // Set when RequestClipboard returns a successful Response (response==0).
    // The portal Clipboard interface allows clipboard ops only after the session
    // has been started, but only if clipboard access was granted in the meantime.
    std::atomic<bool> m_clipboardAccessGranted{ false };
    // Authoritative clipboard state, updated from RemoteDesktop.Start response
    // ('clipboard_enabled' key, since RemoteDesktop interface v2). For older
    // portals where the key is missing we fall back to m_clipboardAccessGranted.
    std::atomic<bool> m_clipboardEnabled{ false };
    bool m_clipboardEnabledKnown = false;
    unsigned int m_remoteDesktopVersion = 0;
    unsigned int m_clipboardVersion = 0;
    std::string m_restoreToken;
    // Optional fallback: issue RequestClipboard and SelectDevices in parallel
    // right after CreateSession completes, then run SelectSources only after
    // both responses arrive. Toggled via the INPUT_BRIDGE_PORTAL_PARALLEL env
    // variable for environments where the merged dialog needs both pending
    // requests at the same time to render the clipboard switch.
    bool m_portalParallelInit = false;
    std::atomic<int> m_pendingFirstStageReplies{ 0 };

    bool ClipboardAvailable() const {
        if (!is_session_ready.load(std::memory_order_relaxed)) return false;
        if (m_clipboardEnabledKnown) {
            return m_clipboardEnabled.load(std::memory_order_relaxed);
        }
        return m_clipboardAccessGranted.load(std::memory_order_relaxed);
    }

    void LogClipboardUnavailable(const char* op) const {
        std::cerr << "Clipboard portal not enabled for this session ("
                  << op << " ignored). RequestClipboard granted="
                  << (m_clipboardAccessGranted.load(std::memory_order_relaxed) ? "true" : "false")
                  << ", Start.clipboard_enabled="
                  << (m_clipboardEnabledKnown
                        ? (m_clipboardEnabled.load(std::memory_order_relaxed) ? "true" : "false")
                        : "missing")
                  << "." << std::endl;
    }

    void ClearPendingWlRemoteTempPathsUnlocked() {
        for (const auto& p : m_pendingWlRemoteTempPaths) {
            unlink(p.c_str());
        }
        m_pendingWlRemoteTempPaths.clear();
    }

    void StopAllPortalFileTransfersUnlocked() {
        if (!connection) {
            m_fileTransferTempByKey.clear();
            return;
        }
        while (!m_fileTransferTempByKey.empty()) {
            auto it = m_fileTransferTempByKey.begin();
            const std::string k = it->first;
            std::vector<std::string> paths = std::move(it->second);
            m_fileTransferTempByKey.erase(it);
            GError* e = nullptr;
            g_dbus_connection_call_sync(
                connection,
                "org.freedesktop.portal.Desktop",
                "/org/freedesktop/portal/documents",
                "org.freedesktop.portal.FileTransfer",
                "StopTransfer",
                g_variant_new("(s)", k.c_str()),
                nullptr,
                G_DBUS_CALL_FLAGS_NONE,
                -1,
                nullptr,
                &e
            );
            if (e) {
                g_error_free(e);
            }
            for (const auto& p : paths) {
                unlink(p.c_str());
            }
        }
    }

    std::optional<std::string> PortalReadClipboardMime(const std::string& mime_type) {
        const std::string session_handle = GetSessionHandle();
        GError* error = nullptr;
        GUnixFDList* out_fd_list = nullptr;
        GVariant* result = g_dbus_connection_call_with_unix_fd_list_sync(
            connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.Clipboard",
            "SelectionRead",
            g_variant_new("(os)", session_handle.c_str(), mime_type.c_str()),
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

        gint handle_index = -1;
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

        if (out_fd_list) {
            g_object_unref(out_fd_list);
        }
        if (result) {
            g_variant_unref(result);
        }

        if (data.empty()) {
            return std::nullopt;
        }
        return data;
    }

    std::optional<std::vector<std::string>> PortalRetrieveFilesForKey(const std::string& key) {
        GVariantBuilder opts;
        g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
        GVariant* opts_done = g_variant_builder_end(&opts);
        GError* error = nullptr;
        GVariant* res = g_dbus_connection_call_sync(
            connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/documents",
            "org.freedesktop.portal.FileTransfer",
            "RetrieveFiles",
            g_variant_new("(s@a{sv})", key.c_str(), opts_done),
            G_VARIANT_TYPE("(as)"),
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            &error
        );
        if (error) {
            g_error_free(error);
            return std::nullopt;
        }
        if (!res) {
            return std::nullopt;
        }
        GVariant* arr = g_variant_get_child_value(res, 0);
        if (!arr) {
            g_variant_unref(res);
            return std::nullopt;
        }
        gsize len = 0;
        gchar** paths = g_variant_dup_strv(arr, &len);
        g_variant_unref(arr);
        g_variant_unref(res);
        std::optional<std::vector<std::string>> out;
        if (paths) {
            out.emplace();
            for (gsize i = 0; i < len; ++i) {
                out->emplace_back(paths[i]);
            }
            g_strfreev(paths);
        }
        return out;
    }

    static void OnFileTransferClosed(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
        GVariant* parameters, gpointer user_data) {
        PlatformInputLinux* self = static_cast<PlatformInputLinux*>(user_data);
        const gchar* key = nullptr;
        g_variant_get(parameters, "(&s)", &key);
        if (!key || !key[0]) {
            return;
        }
        const std::string k(key);
        std::vector<std::string> paths;
        {
            std::lock_guard<std::mutex> lock(self->clipboard_mutex);
            auto it = self->m_fileTransferTempByKey.find(k);
            if (it == self->m_fileTransferTempByKey.end()) {
                return;
            }
            paths = std::move(it->second);
            self->m_fileTransferTempByKey.erase(it);
        }
        for (const auto& p : paths) {
            unlink(p.c_str());
        }
    }

    static int64_t CurrentTimestampMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

    bool IsLastClipboardContentUnlocked(const std::string& type, const std::vector<std::string>& files, const std::string& text) const {
        return m_hasLastClipboardContent
            && m_lastClipboardType == type
            && m_lastClipboardFiles == files
            && m_lastClipboardText == text;
    }

    bool IsLastClipboardContent(const std::string& type, const std::vector<std::string>& files, const std::string& text) {
        std::lock_guard<std::mutex> lock(clipboard_callback_mutex);
        return IsLastClipboardContentUnlocked(type, files, text);
    }

    void StoreLastClipboardContentUnlocked(const std::string& type, const std::vector<std::string>& files, const std::string& text) {
        m_hasLastClipboardContent = true;
        m_lastClipboardType = type;
        m_lastClipboardFiles = files;
        m_lastClipboardText = text;
    }

    bool SetClipboardText(const std::string& text) override {
        if (IsLastClipboardContent("text", {}, text)) {
            return true;
        }
        if (m_activeBackend != ActiveBackend::Portal) {
            {
                std::lock_guard<std::mutex> lock(clipboard_mutex);
                ClearPendingWlRemoteTempPathsUnlocked();
            }
            const bool ok = m_wlClipboard.SetText(text);
            if (ok) EmitClipboardChange("text", {}, text);
            return ok;
        }
        if (!ClipboardAvailable()) {
            LogClipboardUnavailable("SetClipboardText");
            return false;
        }

        const std::string session_handle = GetSessionHandle();
        {
            std::lock_guard<std::mutex> lock(clipboard_mutex);
            StopAllPortalFileTransfersUnlocked();
            ClearPendingWlRemoteTempPathsUnlocked();
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
        if (m_activeBackend != ActiveBackend::Portal) {
            return m_wlClipboard.GetText();
        }
        if (!ClipboardAvailable()) {
            LogClipboardUnavailable("GetClipboardText");
            return std::nullopt;
        }

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
        if (IsLastClipboardContent("files", files, {})) {
            return true;
        }
        if (m_activeBackend != ActiveBackend::Portal) {
            {
                std::lock_guard<std::mutex> lock(clipboard_mutex);
                ClearPendingWlRemoteTempPathsUnlocked();
            }
            const bool ok = m_wlClipboard.SetFiles(files);
            if (ok) EmitClipboardChange("files", files, {});
            return ok;
        }
        if (!ClipboardAvailable()) {
            LogClipboardUnavailable("SetClipboardFiles");
            return false;
        }

        const std::string session_handle = GetSessionHandle();
        const std::string payload = BuildUriListFromAbsPaths(files);

        {
            std::lock_guard<std::mutex> lock(clipboard_mutex);
            StopAllPortalFileTransfersUnlocked();
            ClearPendingWlRemoteTempPathsUnlocked();
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
        if (m_activeBackend != ActiveBackend::Portal) {
            return m_wlClipboard.GetFiles();
        }
        if (!ClipboardAvailable()) {
            LogClipboardUnavailable("GetClipboardFiles");
            return std::nullopt;
        }

        if (auto raw_key = PortalReadClipboardMime("application/vnd.portal.filetransfer")) {
            const std::string key = TrimWhitespace(*raw_key);
            if (!key.empty()) {
                if (auto retrieved = PortalRetrieveFilesForKey(key)) {
                    if (!retrieved->empty()) {
                        return retrieved;
                    }
                }
            }
        }

        auto uri_data = PortalReadClipboardMime("text/uri-list");
        if (!uri_data || uri_data->empty()) {
            return std::nullopt;
        }

        std::vector<std::string> files;
        std::istringstream stream(*uri_data);
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            GError* ferr = nullptr;
            gchar* fn = g_filename_from_uri(line.c_str(), nullptr, &ferr);
            if (fn) {
                files.emplace_back(fn);
                g_free(fn);
            } else {
                if (ferr) {
                    g_error_free(ferr);
                }
                const std::string prefix = "file://";
                if (line.compare(0, prefix.size(), prefix) == 0) {
                    files.push_back(line.substr(prefix.size()));
                }
            }
        }

        return files.empty() ? std::nullopt : std::make_optional(files);
    }

    bool SetClipboardFilesRemote(const std::vector<ClipboardRemoteFileEntry>& files) override {
        if (files.empty()) {
            return false;
        }

        if (m_activeBackend != ActiveBackend::Portal) {
            {
                std::lock_guard<std::mutex> lock(clipboard_mutex);
                ClearPendingWlRemoteTempPathsUnlocked();
            }
            std::vector<std::string> paths;
            paths.reserve(files.size());
            for (const auto& e : files) {
                std::string p;
                if (!WriteClipboardTempFile(e.bytes, e.file_name, p)) {
                    for (const auto& rp : paths) {
                        unlink(rp.c_str());
                    }
                    return false;
                }
                paths.push_back(std::move(p));
            }
            const bool ok = m_wlClipboard.SetFiles(paths);
            if (!ok) {
                for (const auto& rp : paths) {
                    unlink(rp.c_str());
                }
                return false;
            }
            {
                std::lock_guard<std::mutex> lock(clipboard_mutex);
                m_pendingWlRemoteTempPaths = paths;
            }
            EmitClipboardChange("files", paths, {});
            return true;
        }

        if (!ClipboardAvailable()) {
            LogClipboardUnavailable("SetClipboardFilesRemote");
            return false;
        }

        std::vector<std::string> abs_paths;
        abs_paths.reserve(files.size());
        for (const auto& e : files) {
            std::string p;
            if (!WriteClipboardTempFile(e.bytes, e.file_name, p)) {
                for (const auto& rp : abs_paths) {
                    unlink(rp.c_str());
                }
                return false;
            }
            abs_paths.push_back(std::move(p));
        }

        GVariantBuilder st_opts;
        g_variant_builder_init(&st_opts, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&st_opts, "{sv}", "writable", g_variant_new_boolean(FALSE));
        g_variant_builder_add(&st_opts, "{sv}", "autostop", g_variant_new_boolean(TRUE));
        GVariant* st_opts_done = g_variant_builder_end(&st_opts);

        GError* st_err = nullptr;
        GVariant* st_res = g_dbus_connection_call_sync(
            connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/documents",
            "org.freedesktop.portal.FileTransfer",
            "StartTransfer",
            // Jedna wartość typu a{sv} w krotce — musi być @, inaczej varargs są źle interpretowane.
            g_variant_new("(@a{sv})", st_opts_done),
            G_VARIANT_TYPE("(s)"),
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            &st_err
        );

        if (!st_res) {
            std::cerr << "FileTransfer.StartTransfer failed: " << (st_err ? st_err->message : "?") << std::endl;
            if (st_err) {
                g_error_free(st_err);
            }
            for (const auto& rp : abs_paths) {
                unlink(rp.c_str());
            }
            return false;
        }

        const gchar* key_c = nullptr;
        g_variant_get(st_res, "(&s)", &key_c);
        std::string transfer_key = key_c ? std::string(key_c) : std::string();
        g_variant_unref(st_res);

        if (transfer_key.empty()) {
            for (const auto& rp : abs_paths) {
                unlink(rp.c_str());
            }
            return false;
        }

        constexpr size_t kMaxFdsPerAdd = 12;
        auto fail_cleanup = [&]() {
            GError* se = nullptr;
            g_dbus_connection_call_sync(
                connection,
                "org.freedesktop.portal.Desktop",
                "/org/freedesktop/portal/documents",
                "org.freedesktop.portal.FileTransfer",
                "StopTransfer",
                g_variant_new("(s)", transfer_key.c_str()),
                nullptr,
                G_DBUS_CALL_FLAGS_NONE,
                -1,
                nullptr,
                &se
            );
            if (se) {
                g_error_free(se);
            }
            for (const auto& rp : abs_paths) {
                unlink(rp.c_str());
            }
        };

        for (size_t off = 0; off < abs_paths.size(); off += kMaxFdsPerAdd) {
            GUnixFDList* fd_list = g_unix_fd_list_new();
            GVariantBuilder handle_builder;
            g_variant_builder_init(&handle_builder, G_VARIANT_TYPE("ah"));
            const size_t batch_end = std::min(off + kMaxFdsPerAdd, abs_paths.size());
            for (size_t i = off; i < batch_end; ++i) {
                const int raw_fd = open(abs_paths[i].c_str(), O_RDONLY | O_CLOEXEC);
                if (raw_fd < 0) {
                    g_object_unref(fd_list);
                    fail_cleanup();
                    return false;
                }
                const gint idx = g_unix_fd_list_append(fd_list, raw_fd, nullptr);
                if (idx < 0) {
                    g_object_unref(fd_list);
                    fail_cleanup();
                    return false;
                }
                close(raw_fd);
                g_variant_builder_add(&handle_builder, "h", idx);
            }

            GVariantBuilder empty_opts;
            g_variant_builder_init(&empty_opts, G_VARIANT_TYPE_VARDICT);
            GVariant* handles_done = g_variant_builder_end(&handle_builder);
            GVariant* empty_done = g_variant_builder_end(&empty_opts);

            GError* add_err = nullptr;
            // DBus: AddFiles(s key, ah fds, a{sv} options) — musi być @ah (tablica h), nie @h.
            GVariant* add_res = g_dbus_connection_call_with_unix_fd_list_sync(
                connection,
                "org.freedesktop.portal.Desktop",
                "/org/freedesktop/portal/documents",
                "org.freedesktop.portal.FileTransfer",
                "AddFiles",
                g_variant_new("(s@ah@a{sv})", transfer_key.c_str(), handles_done, empty_done),
                nullptr,
                G_DBUS_CALL_FLAGS_NONE,
                -1,
                fd_list,
                nullptr,
                nullptr,
                &add_err
            );
            g_object_unref(fd_list);
            if (add_res) {
                g_variant_unref(add_res);
            }
            if (add_err) {
                std::cerr << "FileTransfer.AddFiles failed: " << add_err->message << std::endl;
                g_error_free(add_err);
                fail_cleanup();
                return false;
            }
        }

        const std::string uri_payload = BuildUriListFromAbsPaths(abs_paths);

        {
            std::lock_guard<std::mutex> lock(clipboard_mutex);
            StopAllPortalFileTransfersUnlocked();
            ClearPendingWlRemoteTempPathsUnlocked();
            m_clipboardData["application/vnd.portal.filetransfer"] = transfer_key;
            m_clipboardData["text/uri-list"] = uri_payload;
            m_fileTransferTempByKey[transfer_key] = abs_paths;
        }

        const std::string session_handle = GetSessionHandle();
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        GVariantBuilder mime_builder;
        g_variant_builder_init(&mime_builder, G_VARIANT_TYPE_STRING_ARRAY);
        g_variant_builder_add(&mime_builder, "s", "application/vnd.portal.filetransfer");
        g_variant_builder_add(&mime_builder, "s", "text/uri-list");
        g_variant_builder_add(&options, "{sv}", "mime_types", g_variant_builder_end(&mime_builder));

        GError* sel_err = nullptr;
        GVariant* sel_res = g_dbus_connection_call_sync(
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
            &sel_err
        );

        if (sel_err) {
            std::cerr << "Failed to SetSelection (Clipboard Remote Files): " << sel_err->message << std::endl;
            g_error_free(sel_err);
            fail_cleanup();
            {
                std::lock_guard<std::mutex> lock(clipboard_mutex);
                m_clipboardData.erase("application/vnd.portal.filetransfer");
                m_clipboardData.erase("text/uri-list");
                m_fileTransferTempByKey.erase(transfer_key);
            }
            return false;
        }
        if (sel_res) {
            g_variant_unref(sel_res);
        }

        EmitClipboardChange("files", abs_paths, {});
        return true;
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
        const int64_t timestampMs = CurrentTimestampMs();
        {
            std::lock_guard<std::mutex> lock(clipboard_callback_mutex);
            if (IsLastClipboardContentUnlocked(type, files, text)) {
                return;
            }
            StoreLastClipboardContentUnlocked(type, files, text);
            callback = m_clipboardCallback;
        }
        if (callback) {
            callback(type, files, text, timestampMs);
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
    ActiveBackend m_activeBackend = ActiveBackend::Portal;
    std::string session_handle;
    std::atomic<bool> is_session_ready = false;
    bool m_batchMode = false;
    std::mutex m_monitorMutex;
    std::vector<MonitorInfo> m_monitors;
    int32_t m_currentMonitorIndex = 0;
    int32_t m_virtMinX = 0;
    int32_t m_virtMinY = 0;
    int32_t m_virtWidth = 1920;
    int32_t m_virtHeight = 1080;

    // Śledzenie ostatnich współrzędnych myszy do emulacji ruchu względnego
    int32_t m_lastX = 0;
    int32_t m_lastY = 0;

    GMainContext* dbus_context = nullptr;
    GMainLoop* main_loop = nullptr;
    std::thread loop_thread;
    std::atomic<bool> is_running{ false };
    guint response_signal_id = 0;

    std::mutex session_mutex;
    std::condition_variable session_cv;
    std::atomic<bool> is_session_started = false;
    std::unique_ptr<LinuxUinputInjector> m_uinputInjector;
    LinuxWlClipboard m_wlClipboard;

    std::string GetSessionHandle() {
        std::lock_guard<std::mutex> lock(session_mutex);
        return session_handle;
    }

    std::optional<std::string> GetPortalSessionHandle() override {
        if (m_activeBackend != ActiveBackend::Portal) {
            return std::nullopt;
        }
        const std::string handle = GetSessionHandle();
        if (handle.empty()) {
            return std::nullopt;
        }
        return handle;
    }

    static MonitorInfo BuildDefaultMonitor() {
        MonitorInfo monitor;
        monitor.index = 0;
        monitor.id = "portal-default";
        monitor.name = "Portal Desktop";
        monitor.x = 0;
        monitor.y = 0;
        monitor.width = 1920;
        monitor.height = 1080;
        monitor.primary = true;
        return monitor;
    }

    bool TryLoadHyprlandMonitors() {
        if (!std::getenv("HYPRLAND_INSTANCE_SIGNATURE")) {
            return false;
        }

        FILE* pipe = popen("hyprctl -j monitors 2>/dev/null", "r");
        if (!pipe) {
            return false;
        }

        std::string json;
        char buffer[1024];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            json += buffer;
        }
        const int status = pclose(pipe);
        if (status != 0 || json.empty()) {
            return false;
        }

        std::vector<MonitorInfo> parsed;
        std::regex object_regex("\\{[^\\{\\}]*\\}");
        std::regex id_regex("\"id\"\\s*:\\s*(\\d+)");
        std::regex name_regex("\"name\"\\s*:\\s*\"([^\"]+)\"");
        std::regex x_regex("\"x\"\\s*:\\s*(-?\\d+)");
        std::regex y_regex("\"y\"\\s*:\\s*(-?\\d+)");
        std::regex w_regex("\"width\"\\s*:\\s*(\\d+)");
        std::regex h_regex("\"height\"\\s*:\\s*(\\d+)");
        std::regex focused_regex("\"focused\"\\s*:\\s*(true|false)");

        auto begin = std::sregex_iterator(json.begin(), json.end(), object_regex);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            const std::string obj = it->str();
            std::smatch m;
            MonitorInfo monitor;
            monitor.index = static_cast<int32_t>(parsed.size());
            if (std::regex_search(obj, m, id_regex)) monitor.id = m[1].str();
            if (std::regex_search(obj, m, name_regex)) monitor.name = m[1].str();
            if (std::regex_search(obj, m, x_regex)) monitor.x = std::stoi(m[1].str());
            if (std::regex_search(obj, m, y_regex)) monitor.y = std::stoi(m[1].str());
            if (std::regex_search(obj, m, w_regex)) monitor.width = std::stoi(m[1].str());
            if (std::regex_search(obj, m, h_regex)) monitor.height = std::stoi(m[1].str());
            if (std::regex_search(obj, m, focused_regex)) monitor.primary = m[1].str() == "true";
            if (monitor.width > 0 && monitor.height > 0) {
                if (monitor.id.empty()) monitor.id = std::to_string(monitor.index);
                if (monitor.name.empty()) monitor.name = "Hyprland monitor";
                parsed.push_back(std::move(monitor));
            }
        }

        if (parsed.empty()) return false;

        std::lock_guard<std::mutex> lock(m_monitorMutex);
        m_monitors = std::move(parsed);
        m_currentMonitorIndex = std::stoi(m_monitors.front().id);
        UpdateVirtualDesktopBounds();
        return true;
    }

    const MonitorInfo& GetCurrentMonitor() const {
        // Szukamy monitora, którego id (jako string) odpowiada zapisanemu m_currentMonitorIndex (PipeWire ID)
        std::string targetId = std::to_string(m_currentMonitorIndex);
        auto it = std::find_if(m_monitors.begin(), m_monitors.end(),
                               [&targetId](const MonitorInfo& m) { return m.id == targetId; });

        if (it != m_monitors.end()) return *it;
        if (!m_monitors.empty())    return m_monitors.front();

        static MonitorInfo fallback = BuildDefaultMonitor();
        return fallback;
    }

    bool EnsureStarted() {
        if (m_activeBackend != ActiveBackend::Portal) {
            return is_session_ready.load(std::memory_order_relaxed);
        }
        if (is_session_started.load(std::memory_order_relaxed)) return true;
        if (!is_session_ready.load(std::memory_order_relaxed)) return false;

        StartSession(this);
        std::unique_lock<std::mutex> lock(session_mutex);
        return session_cv.wait_for(lock, std::chrono::seconds(5), [this] { return this->is_session_started.load(std::memory_order_relaxed); });
    }

    static std::string ReadBackendMode() {
        if (const char* mode = std::getenv("INPUT_BRIDGE_LINUX_BACKEND")) {
            return mode;
        }
        return "auto";
    }

    bool TryInitializeLibeiSocket(std::string& error_msg) {
#if !INPUT_BRIDGE_HAS_LIBEI
        error_msg = "Libei socket backend is not available in this build.";
        return false;
#else
        const char* socket_path = std::getenv("INPUT_BRIDGE_EI_SOCKET");
        if (!socket_path || std::string(socket_path).empty()) {
            error_msg = "INPUT_BRIDGE_EI_SOCKET is not set.";
            return false;
        }

        if (m_eis.context || m_eis.device || m_eis.seat) {
            ReleaseEISState();
        }

        m_eis.context = ei_new_sender(this);
        if (!m_eis.context) {
            error_msg = "Failed to create libei sender context.";
            return false;
        }

        ei_configure_name(m_eis.context, "input-bridge");
        if (ei_setup_backend_socket(m_eis.context, socket_path) < 0) {
            ei_unref(m_eis.context);
            m_eis.context = nullptr;
            error_msg = "ei_setup_backend_socket failed for INPUT_BRIDGE_EI_SOCKET.";
            return false;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            EISDispatchPending();
            if (HasEISDevice()) {
                break;
            }
            pollfd pfd{};
            pfd.fd = ei_get_fd(m_eis.context);
            pfd.events = POLLIN;
            pfd.revents = 0;
            if (pfd.fd < 0) {
                break;
            }
            poll(&pfd, 1, 50);
        }

        EISDispatchPending();
        if (!HasEISDevice()) {
            ReleaseEISState();
            error_msg = "Timed out waiting for libei socket device readiness.";
            return false;
        }

        m_activeBackend = ActiveBackend::LibeiSocket;
        is_session_ready.store(true, std::memory_order_relaxed);
        is_session_started.store(true, std::memory_order_relaxed);
        eis_connected.store(true, std::memory_order_relaxed);
        input_mode = InputMode::EIS;
        return true;
#endif
    }

    bool TryInitializeUinput(std::string& error_msg) {
        m_uinputInjector = std::make_unique<LinuxUinputInjector>();
        if (!m_uinputInjector->Initialize(error_msg)) {
            m_uinputInjector.reset();
            return false;
        }
        m_activeBackend = ActiveBackend::Uinput;
        is_session_ready.store(true, std::memory_order_relaxed);
        is_session_started.store(true, std::memory_order_relaxed);
        return true;
    }

    bool TryInitializeFallback(std::string& error_msg, const std::string& reason) {
        const std::string mode = ReadBackendMode();
        std::string libei_error;
        std::string uinput_error;

        if ((mode == "auto" || mode == "libei-socket") && TryInitializeLibeiSocket(libei_error)) {
            TryLoadHyprlandMonitors();
            std::cerr << "Portal unavailable (" << reason << "). Using libei socket backend." << std::endl;
            return true;
        }

        if ((mode == "auto" || mode == "uinput") && TryInitializeUinput(uinput_error)) {
            TryLoadHyprlandMonitors();
            std::cerr << "Portal unavailable (" << reason << "). Using uinput backend." << std::endl;
            return true;
        }

        std::ostringstream oss;
        oss << "Failed to initialize Linux backend. portal_reason=" << reason;
        if (!libei_error.empty()) {
            oss << "; libei_socket=" << libei_error;
        }
        if (!uinput_error.empty()) {
            oss << "; uinput=" << uinput_error;
        }
        error_msg = oss.str();
        return false;
    }

    void UpdateVirtualDesktopBounds() {
        // Assumes m_monitorMutex is held by caller
        if (m_monitors.empty()) {
            m_virtMinX = 0; m_virtMinY = 0;
            m_virtWidth = 1920; m_virtHeight = 1080;
            return;
        }

        int32_t minX = m_monitors[0].x;
        int32_t minY = m_monitors[0].y;
        int32_t maxX = m_monitors[0].x + m_monitors[0].width;
        int32_t maxY = m_monitors[0].y + m_monitors[0].height;

        for (const auto& m : m_monitors) {
            minX = std::min(minX, m.x);
            minY = std::min(minY, m.y);
            maxX = std::max(maxX, m.x + m.width);
            maxY = std::max(maxY, m.y + m.height);
        }

        m_virtMinX = minX;
        m_virtMinY = minY;
        m_virtWidth = std::max(1, maxX - minX);
        m_virtHeight = std::max(1, maxY - minY);
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

    static std::vector<std::string> ExtractClipboardMimeTypes(GVariant* options) {
        std::vector<std::string> mime_types;
        if (!options) return mime_types;

        GVariant* value = g_variant_lookup_value(options, "mime_types", G_VARIANT_TYPE_STRING_ARRAY);
        if (!value) return mime_types;

        GVariantIter iter;
        const gchar* mime_type = nullptr;
        g_variant_iter_init(&iter, value);
        while (g_variant_iter_next(&iter, "&s", &mime_type)) {
            mime_types.emplace_back(mime_type);
        }
        g_variant_unref(value);
        return mime_types;
    }

    static bool HasMimeType(const std::vector<std::string>& mime_types, const char* mime_type) {
        return std::find(mime_types.begin(), mime_types.end(), mime_type) != mime_types.end();
    }

    static bool ClipboardSessionIsOwner(GVariant* options) {
        if (!options) return false;
        gboolean session_is_owner = FALSE;
        return g_variant_lookup(options, "session_is_owner", "b", &session_is_owner) && session_is_owner == TRUE;
    }

    static void OnClipboardSelectionOwnerChanged(GDBusConnection* connection, const gchar* sender_name,
        const gchar* object_path, const gchar* interface_name,
        const gchar* signal_name, GVariant* parameters, gpointer user_data) {
        (void)connection;
        (void)sender_name;
        (void)object_path;
        (void)interface_name;
        (void)signal_name;

        PlatformInputLinux* self = static_cast<PlatformInputLinux*>(user_data);
        const std::string current_session_handle = self->GetSessionHandle();
        const gchar* session_handle = nullptr;
        GVariant* options = nullptr;

        g_variant_get(parameters, "(&o@a{sv})", &session_handle, &options);

        if (current_session_handle != session_handle || !self->ClipboardAvailable()) {
            if (options) g_variant_unref(options);
            return;
        }

        if (ClipboardSessionIsOwner(options)) {
            g_variant_unref(options);
            return;
        }

        const std::vector<std::string> mime_types = ExtractClipboardMimeTypes(options);
        if (options) g_variant_unref(options);

        const bool mime_types_known = !mime_types.empty();
        const bool can_read_files = !mime_types_known
            || HasMimeType(mime_types, "text/uri-list")
            || HasMimeType(mime_types, "application/vnd.portal.filetransfer");
        const bool can_read_text = !mime_types_known
            || HasMimeType(mime_types, "text/plain;charset=utf-8")
            || HasMimeType(mime_types, "text/plain");

        if (can_read_files) {
            auto files = self->GetClipboardFiles();
            if (files && !files->empty()) {
                self->EmitClipboardChange("files", *files, {});
                return;
            }
        }

        if (can_read_text) {
            auto text = self->GetClipboardText();
            if (text && !text->empty()) {
                self->EmitClipboardChange("text", {}, *text);
            }
        }
    }

    // Counts down responses to the first stage of parallel portal initialization
    // (RequestClipboard + SelectDevices). When both have completed we proceed to
    // SelectSources. Used only when m_portalParallelInit is true.
    static void OnFirstStageReply(PlatformInputLinux* self) {
        const int remaining = self->m_pendingFirstStageReplies.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining <= 0) {
            SelectSources(self);
        }
    }

    static unsigned int QueryPortalVersion(GDBusConnection* conn, const char* iface) {
        if (!conn || !iface) return 0;
        GError* error = nullptr;
        GVariant* result = g_dbus_connection_call_sync(
            conn,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.DBus.Properties",
            "Get",
            g_variant_new("(ss)", iface, "version"),
            G_VARIANT_TYPE("(v)"),
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            &error
        );
        if (error) {
            std::cerr << "Failed to read " << iface << " version: " << error->message << std::endl;
            g_error_free(error);
            return 0;
        }
        unsigned int version = 0;
        if (result) {
            GVariant* inner = nullptr;
            g_variant_get(result, "(v)", &inner);
            if (inner) {
                if (g_variant_is_of_type(inner, G_VARIANT_TYPE_UINT32)) {
                    version = g_variant_get_uint32(inner);
                }
                g_variant_unref(inner);
            }
            g_variant_unref(result);
        }
        return version;
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

        const bool is_create_resp = g_str_has_suffix(object_path, "ib_create");
        const bool is_clipboard_resp = g_str_has_suffix(object_path, "ib_clipboard");
        const bool is_select_resp = g_str_has_suffix(object_path, "ib_select");
        const bool is_sources_resp = g_str_has_suffix(object_path, "ib_sources") || g_str_has_suffix(object_path, "sc_sources");
        const bool is_start_resp = g_str_has_suffix(object_path, "ib_start") || g_str_has_suffix(object_path, "sc_start");

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
                            // On this portal backend RequestClipboard can return
                            // "Invalid state" until screen sources are selected.
                            // Default flow: SelectDevices -> SelectSources ->
                            // RequestClipboard -> Start.
                            if (self->m_portalParallelInit) {
                                self->m_pendingFirstStageReplies.store(2, std::memory_order_relaxed);
                                SelectDevices(self);
                                RequestClipboard(self);
                            } else {
                                SelectDevices(self);
                            }
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
                std::cout << "RequestClipboard completed successfully." << std::endl;
                self->m_clipboardAccessGranted.store(true, std::memory_order_relaxed);
            } else {
                std::cerr << "RequestClipboard denied. Response: " << response << std::endl;
                self->m_clipboardAccessGranted.store(false, std::memory_order_relaxed);
            }

            if (self->m_portalParallelInit) {
                OnFirstStageReply(self);
            } else {
                // For sequential mode: after clipboard success, request SelectDevices
                // If clipboard was denied, we still need to continue the portal flow
                // but clipboard operations will fail until later enabled
                SelectDevices(self);
            }
        } else if (is_select_resp) {
            if (response == 0) {
                std::cout << "SelectDevices completed successfully." << std::endl;
                if (results) {
                    GVariant* token_v = g_variant_lookup_value(results, "restore_token", G_VARIANT_TYPE_STRING);
                    if (token_v) {
                        self->m_restoreToken = g_variant_get_string(token_v, nullptr);
                        std::cout << "Portal Persistent Session restore_token: " << self->m_restoreToken << std::endl;
                        g_variant_unref(token_v);
                    }
                }
                if (self->m_portalParallelInit) {
                    OnFirstStageReply(self);
                } else {
                    // For sequential mode: after SelectDevices success, request RequestClipboard
                    RequestClipboard(self);
                }
            } else {
                std::cerr << "SelectDevices denied. Response: " << response << std::endl;
                self->session_cv.notify_all();
            }
        } else if (is_sources_resp) {
            if (response == 0) {
                std::cout << "ScreenCast.SelectSources completed successfully." << std::endl;
                     if (results) {
                    GVariant* token_v = g_variant_lookup_value(results, "restore_token", G_VARIANT_TYPE_STRING);
                    if (token_v) {
                        self->m_restoreToken = g_variant_get_string(token_v, nullptr);
                        std::cout << "Portal Persistent Session restore_token: " << self->m_restoreToken << std::endl;
                        g_variant_unref(token_v);
                    }
                }
                // Eager Start: negotiate screen cast streams immediately so GetMonitors() /
                // OpenPipeWireRemoteFd callers see real PipeWire node IDs after init().
                // screen-capture can still reuse the session; duplicate Start is handled there.
                StartSession(self);
            } else {
                std::cerr << "Screen capture permission denied. Response: " << response << std::endl;
                self->session_cv.notify_all();
            }
        } else if (is_start_resp) {
            if (response == 0) {
                if (results) {
                    GVariant* token_v = g_variant_lookup_value(results, "restore_token", G_VARIANT_TYPE_STRING);
                    if (token_v) {
                        self->m_restoreToken = g_variant_get_string(token_v, nullptr);
                        std::cout << "Portal Persistent Session restore_token (from Start): " << self->m_restoreToken << std::endl;
                        g_variant_unref(token_v);
                    }
                }
                if (results) {
                    // RemoteDesktop v2 reports the user's clipboard decision and the
                    // negotiated device bitmask in the Start response. Parsing both
                    // gives us a definitive picture of what the portal allowed.
                    GVariant* clipboard_v = g_variant_lookup_value(results, "clipboard_enabled", G_VARIANT_TYPE_BOOLEAN);
                    if (clipboard_v) {
                        const gboolean enabled = g_variant_get_boolean(clipboard_v);
                        self->m_clipboardEnabled.store(enabled == TRUE, std::memory_order_relaxed);
                        self->m_clipboardEnabledKnown = true;
                        std::cout << "Start: clipboard_enabled=" << (enabled ? "true" : "false") << std::endl;
                        g_variant_unref(clipboard_v);
                    } else {
                        self->m_clipboardEnabledKnown = false;
                        const bool granted = self->m_clipboardAccessGranted.load(std::memory_order_relaxed);
                        self->m_clipboardEnabled.store(granted, std::memory_order_relaxed);
                        std::cout << "Start: clipboard_enabled key missing (legacy portal); falling back to RequestClipboard grant="
                                  << (granted ? "true" : "false") << std::endl;
                    }

                    GVariant* devices_v = g_variant_lookup_value(results, "devices", G_VARIANT_TYPE_UINT32);
                    if (devices_v) {
                        const guint32 mask = g_variant_get_uint32(devices_v);
                        std::cout << "Start: devices bitmask=0x" << std::hex << mask << std::dec
                                  << " (keyboard=" << ((mask & 1u) ? "y" : "n")
                                  << " pointer=" << ((mask & 2u) ? "y" : "n")
                                  << " touchscreen=" << ((mask & 4u) ? "y" : "n") << ")" << std::endl;
                        g_variant_unref(devices_v);
                    } else {
                        std::cout << "Start: devices key missing in response." << std::endl;
                    }

                    std::vector<MonitorInfo> detectedMonitors;
                    GVariant* streams_v = g_variant_lookup_value(results, "streams", G_VARIANT_TYPE("a(ua{sv})"));
                    if (streams_v) {
                        GVariantIter stream_iter;
                        g_variant_iter_init(&stream_iter, streams_v);
                        GVariant* stream_tuple = nullptr;
                        int monitorPos = 0;

                        while ((stream_tuple = g_variant_iter_next_value(&stream_iter)) != nullptr) {
                            guint32 node_id = 0;
                            GVariant* props = nullptr;
                            g_variant_get(stream_tuple, "(u@a{sv})", &node_id, &props);

                            MonitorInfo monitor;
                            monitor.index = monitorPos++;
                            monitor.id = std::to_string(node_id);
                            monitor.name = "Portal stream " + monitor.id;
                            monitor.primary = detectedMonitors.empty();

                            if (props) {
                                GVariant* title_v = g_variant_lookup_value(props, "title", G_VARIANT_TYPE_STRING);
                                if (title_v) {
                                    const gchar* title = g_variant_get_string(title_v, nullptr);
                                    if (title && *title) {
                                        monitor.name = title;
                                    }
                                    g_variant_unref(title_v);
                                }

                                GVariant* connector_v = g_variant_lookup_value(props, "connector", G_VARIANT_TYPE_STRING);
                                if (connector_v) {
                                    const gchar* connector = g_variant_get_string(connector_v, nullptr);
                                    if (connector && *connector) {
                                        monitor.name = connector;
                                    }
                                    g_variant_unref(connector_v);
                                }

                                GVariant* size_v = g_variant_lookup_value(props, "size", G_VARIANT_TYPE("(ii)"));
                                if (size_v) {
                                    int32_t width = 0;
                                    int32_t height = 0;
                                    g_variant_get(size_v, "(ii)", &width, &height);
                                    monitor.width = std::max(1, width);
                                    monitor.height = std::max(1, height);
                                    g_variant_unref(size_v);
                                }

                                GVariant* pos_v = g_variant_lookup_value(props, "position", G_VARIANT_TYPE("(ii)"));
                                if (pos_v) {
                                    int32_t x = 0;
                                    int32_t y = 0;
                                    g_variant_get(pos_v, "(ii)", &x, &y);
                                    monitor.x = x;
                                    monitor.y = y;
                                    g_variant_unref(pos_v);
                                }

                                g_variant_unref(props);
                            }

                            detectedMonitors.push_back(std::move(monitor));
                            g_variant_unref(stream_tuple);
                        }
                        g_variant_unref(streams_v);
                    }

                    if (!detectedMonitors.empty()) {
                        std::lock_guard<std::mutex> lock(self->m_monitorMutex);
                        self->m_monitors = std::move(detectedMonitors);
                        self->m_currentMonitorIndex = std::stoi(self->m_monitors.front().id);
                        self->UpdateVirtualDesktopBounds();
                    }
                }

                std::cout << "RemoteDesktop session successfully STARTed! Ready for input injection." << std::endl;
                // Mark session as started only after streams/monitors parsing is complete.
                // This prevents callers waiting on EnsureStarted() from observing stale monitor data.
                self->is_session_started.store(true);
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
        g_variant_builder_add(&options_builder, "{sv}", "handle_token", g_variant_new_string("ib_start"));

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
                self->is_session_ready.store(false, std::memory_order_relaxed);
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
        g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string("ib_clipboard"));

        GVariant* result = g_dbus_connection_call_sync(
            self->connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.Clipboard",
            "RequestClipboard",
            g_variant_new("(o@a{sv})", session_handle.c_str(), g_variant_builder_end(&builder)),
            nullptr, // Some portals return (), some return (o). Let signal handler manage it.
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            &error
        );

        if (error) {
            std::cerr << "Failed to RequestClipboard: " << error->message
                      << " [domain=" << g_quark_to_string(error->domain)
                      << " code=" << error->code << "]" << std::endl;
            g_error_free(error);

            // Proceed even if RequestClipboard fails
            if (self->m_portalParallelInit) {
                OnFirstStageReply(self);
            } else {
                SelectSources(self);
            }
        } else {
            bool has_request_handle = false;
            if (result && g_variant_is_of_type(result, G_VARIANT_TYPE("(o)"))) {
                const gchar* request_path = nullptr;
                g_variant_get(result, "(&o)", &request_path);
                std::cout << "RequestClipboard requested. Request path: " << (request_path ? request_path : "null") << std::endl;
                has_request_handle = true;
            } else {
                std::cout << "RequestClipboard succeeded immediately (no request handle)." << std::endl;
            }

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

            if (self->clipboard_owner_changed_signal_id == 0) {
                self->clipboard_owner_changed_signal_id = g_dbus_connection_signal_subscribe(
                    self->connection,
                    "org.freedesktop.portal.Desktop",
                    "org.freedesktop.portal.Clipboard",
                    "SelectionOwnerChanged",
                    "/org/freedesktop/portal/desktop",
                    nullptr,
                    G_DBUS_SIGNAL_FLAGS_NONE,
                    OnClipboardSelectionOwnerChanged,
                    self,
                    nullptr
                );
            }

            if (!has_request_handle) {
                // If no request handle was returned, we won't receive a Response signal.
                // We treat this as a success and move forward.
                self->m_clipboardAccessGranted.store(true, std::memory_order_relaxed);
                if (self->m_portalParallelInit) {
                    OnFirstStageReply(self);
                } else {
                    SelectSources(self);
                }
            }
        }

        if (result) {
            g_variant_unref(result);
        }
    }

    static void SelectSources(PlatformInputLinux* self) {
        GError* error = nullptr;
        const std::string session_handle = self->GetSessionHandle();
        std::cout << "Calling ScreenCast.SelectSources on session " << session_handle << "..." << std::endl;

        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&builder, "{sv}", "types", g_variant_new_uint32(1)); // 1 = Monitor
        g_variant_builder_add(&builder, "{sv}", "multiple", g_variant_new_boolean(TRUE));
        g_variant_builder_add(&builder, "{sv}", "cursor_mode", g_variant_new_uint32(2)); // 2 = Metadata
        g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string("ib_sources"));

        GVariant* result = g_dbus_connection_call_sync(
            self->connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.ScreenCast",
            "SelectSources",
            g_variant_new("(o@a{sv})", session_handle.c_str(), g_variant_builder_end(&builder)),
            G_VARIANT_TYPE("(o)"),
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            &error
        );

        if (error) {
            std::cerr << "Failed to SelectSources: " << error->message << std::endl;
            g_error_free(error);
            self->session_cv.notify_all();
            return;
        }

        bool has_request_handle = false;
        if (result && g_variant_is_of_type(result, G_VARIANT_TYPE("(o)"))) {
            const gchar* request_path = nullptr;
            g_variant_get(result, "(&o)", &request_path);
            std::cout << "SelectSources requested. Request path: " << (request_path ? request_path : "null") << std::endl;
            has_request_handle = true;
        } else {
            std::cout << "SelectSources succeeded immediately (no request handle)." << std::endl;
        }

        if (!has_request_handle) {
            // No request handle means no Response signal will come. Start session now.
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

        uint32_t types = 7; // Klawiatura (1) | Wskaźnik (2) | Dotyk (4)

        g_variant_builder_add(&builder, "{sv}", "types", g_variant_new_uint32(types));
        g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string("ib_select"));

        // Obsługa sesji permanentnej
        g_variant_builder_add(&builder, "{sv}", "persist_mode", g_variant_new_uint32(2)); // 2 = Permanentnie
        if (!self->m_restoreToken.empty()) {
            g_variant_builder_add(&builder, "{sv}", "restore_token", g_variant_new_string(self->m_restoreToken.c_str()));
        }

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
            if (self->m_portalParallelInit) {
                OnFirstStageReply(self);
            } else {
                self->session_cv.notify_all();
            }
        } else if (result) {
            bool has_request_handle = false;
            if (g_variant_is_of_type(result, G_VARIANT_TYPE("(o)"))) {
                const gchar* request_path = nullptr;
                g_variant_get(result, "(&o)", &request_path);
                std::cout << "SelectDevices returned request path: " << (request_path ? request_path : "null") << std::endl;
                has_request_handle = true;
            } else {
                std::cout << "SelectDevices succeeded immediately (no request handle)." << std::endl;
            }

            if (!has_request_handle) {
                // No request handle means no Response signal. Move to next step.
                if (self->m_portalParallelInit) {
                    OnFirstStageReply(self);
                } else {
                    RequestClipboard(self);
                }
            }

            g_variant_unref(result);
        }
    }

    bool Initialize(std::string& error_msg) override {
        const std::string mode = ReadBackendMode();
        if (mode == "uinput") {
            return TryInitializeUinput(error_msg);
        }
        if (mode == "libei-socket") {
            return TryInitializeLibeiSocket(error_msg);
        }

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

            this->m_remoteDesktopVersion = QueryPortalVersion(this->connection, "org.freedesktop.portal.RemoteDesktop");
            this->m_clipboardVersion = QueryPortalVersion(this->connection, "org.freedesktop.portal.Clipboard");
            std::cout << "Portal versions: RemoteDesktop=" << this->m_remoteDesktopVersion
                      << " Clipboard=" << this->m_clipboardVersion << std::endl;
            if (this->m_remoteDesktopVersion < 2) {
                std::cerr << "Warning: org.freedesktop.portal.RemoteDesktop version < 2; "
                          << "clipboard_enabled flag and ConnectToEIS will be unavailable." << std::endl;
            }
            if (this->m_clipboardVersion == 0) {
                std::cerr << "Warning: org.freedesktop.portal.Clipboard not available; "
                          << "clipboard switch will not appear in the portal dialog." << std::endl;
            }

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

            if (this->m_file_transfer_signal_id == 0) {
                this->m_file_transfer_signal_id = g_dbus_connection_signal_subscribe(
                    this->connection,
                    "org.freedesktop.portal.Desktop",
                    "org.freedesktop.portal.FileTransfer",
                    "TransferClosed",
                    "/org/freedesktop/portal/documents",
                    nullptr,
                    G_DBUS_SIGNAL_FLAGS_NONE,
                    OnFileTransferClosed,
                    this,
                    nullptr
                );
            }

            this->main_loop = g_main_loop_new(this->dbus_context, FALSE);
            dbus_ready.set_value(true);
            g_main_loop_run(this->main_loop);
            g_main_context_pop_thread_default(this->dbus_context);
            });

        if (!dbus_future.get()) {
            if (loop_thread.joinable()) {
                loop_thread.join();
            }
            return TryInitializeFallback(error_msg, "Failed to initialize DBus thread");
        }

        is_running = true;

        // Próba odczytania tokenu przywracania z env dla ułatwienia testów/wdrożenia
        if (const char* env_token = std::getenv("INPUT_BRIDGE_RESTORE_TOKEN")) {
            m_restoreToken = env_token;
            std::cout << "Using restore_token from environment: " << m_restoreToken << std::endl;
        }

        if (const char* parallel = std::getenv("INPUT_BRIDGE_PORTAL_PARALLEL")) {
            m_portalParallelInit = (parallel[0] == '1' || parallel[0] == 't' || parallel[0] == 'T'
                                    || parallel[0] == 'y' || parallel[0] == 'Y');
        }
        std::cout << "Portal init mode: "
                  << (m_portalParallelInit
                        ? "parallel (RequestClipboard + SelectDevices issued together)"
                        : "sequential (SelectDevices then RequestClipboard then SelectSources)")
                  << std::endl;

        // Parameters for CreateSession (e.g. token so we know the Request ID)
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&builder, "{sv}", "session_handle_token", g_variant_new_string("ib_session"));
        g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string("ib_create"));


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
            const std::string portal_error = std::string("Failed to call CreateSession: ") + error->message;
            g_error_free(error);
            return TryInitializeFallback(error_msg, portal_error);
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
                m_activeBackend = ActiveBackend::Portal;
                return true;
            } else {
                return TryInitializeFallback(error_msg, "Timeout expired or was denied! Authorization failed.");
            }
        }
    }

    public:

    PlatformInputLinux() {
        // This object will initialize the session via Async Init
        m_monitors.push_back(BuildDefaultMonitor());
        UpdateVirtualDesktopBounds();
    }

    ~PlatformInputLinux() {
        DisconnectEIS();
        if (m_uinputInjector) {
            m_uinputInjector->Shutdown();
            m_uinputInjector.reset();
        }
        {
            std::lock_guard<std::mutex> lock(clipboard_mutex);
            StopAllPortalFileTransfersUnlocked();
            ClearPendingWlRemoteTempPathsUnlocked();
        }
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
        if (clipboard_owner_changed_signal_id > 0 && connection) {
            g_dbus_connection_signal_unsubscribe(connection, clipboard_owner_changed_signal_id);
        }
        if (m_file_transfer_signal_id > 0 && connection) {
            g_dbus_connection_signal_unsubscribe(connection, m_file_transfer_signal_id);
        }
        if (connection) {
            g_object_unref(connection);
        }
    }

    std::optional<int> OpenPipeWireRemoteFd(std::string& error_msg) override {
        if (m_activeBackend != ActiveBackend::Portal) {
            error_msg = "PipeWire remote fd is available only with portal backend.";
            return std::nullopt;
        }
        if (!is_session_ready.load(std::memory_order_relaxed)) {
            error_msg = "Session is not ready. Call init() and wait for portal authorization first.";
            return std::nullopt;
        }

        if (!EnsureStarted()) {
            error_msg = "Failed to start session before opening PipeWire remote.";
            return std::nullopt;
        }

        const std::string current_session = GetSessionHandle();
        if (current_session.empty()) {
            error_msg = "RemoteDesktop session handle is empty.";
            return std::nullopt;
        }

        GVariantBuilder options_builder;
        g_variant_builder_init(&options_builder, G_VARIANT_TYPE_VARDICT);

        GError* error = nullptr;
        GUnixFDList* out_fd_list = nullptr;
        GVariant* result = g_dbus_connection_call_with_unix_fd_list_sync(
            connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.ScreenCast",
            "OpenPipeWireRemote",
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
            error_msg = std::string("OpenPipeWireRemote failed: ") + error->message;
            g_error_free(error);
            if (out_fd_list) g_object_unref(out_fd_list);
            if (result) g_variant_unref(result);
            return std::nullopt;
        }

        gint handle_index = -1;
        g_variant_get(result, "(h)", &handle_index);
        int fd = g_unix_fd_list_get(out_fd_list, handle_index, nullptr);

        if (result) g_variant_unref(result);
        if (out_fd_list) g_object_unref(out_fd_list);

        if (fd < 0) {
            error_msg = "OpenPipeWireRemote did not return a valid fd.";
            return std::nullopt;
        }

        return fd;
    }

    void MoveMouseRelative(int32_t x, int32_t y) override {
        if (!EnsureStarted()) return;
        if (m_activeBackend == ActiveBackend::Uinput && m_uinputInjector) {
            m_uinputInjector->MoveRelative(x, y);
            return;
        }
#if INPUT_BRIDGE_HAS_LIBEI
        if (eis_connected.load(std::memory_order_relaxed)) {
            EISDispatchPending();
            if (HasEISDevice()) {
                SendEISRelativeMotion(static_cast<double>(x), static_cast<double>(y));
                return;
            }
            if (!allow_notify_pointer) {
                Log("EIS pointer routing requested but notify fallback is disabled.");
                return;
            }
        }
#endif

        if (m_activeBackend != ActiveBackend::Portal) return;

        if (!allow_notify_pointer) {
            Log("Pointer routing requested but notify fallback is disabled.");
            return;
        }

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
        if (!EnsureStarted()) return;
        if (m_activeBackend == ActiveBackend::Uinput && m_uinputInjector) {
            MonitorInfo monitor;
            {
                std::lock_guard<std::mutex> lock(m_monitorMutex);
                monitor = GetCurrentMonitor();
            }
            m_uinputInjector->MoveAbsolute(x, y, monitor.width, monitor.height);
            return;
        }

        MonitorInfo monitor;
        {
            std::lock_guard<std::mutex> lock(m_monitorMutex);
            monitor = GetCurrentMonitor();
        }

        if (monitor.width <= 0 || monitor.height <= 0) return;

        // Przyjmujemy wejściowe x i y jako piksele logiczne [0..width/height]
        // double localX = std::max(0.0, std::min(static_cast<double>(x), static_cast<double>(monitor.width - 1)));
        // double localY = std::max(0.0, std::min(static_cast<double>(y), static_cast<double>(monitor.height - 1)));
        
        double localX = (double)x ;
        double localY = (double)y ; 
        // double localX = (double)x / (double)monitor.width;
        // double localY = (double)y / (double)monitor.height;



        // Faktyczny PipeWire Stream ID znajduje się w monitor.id (string). 
        // monitor.index to tylko pozycja w tablicy.
        uint32_t streamIndex = 0;
        try {
            streamIndex = std::stoul(monitor.id);
        } catch (const std::exception& e) {
            Log("Error parsing PipeWire Stream ID from monitor.id '" + monitor.id + "': " + e.what());
            return; // Nie można kontynuować bez poprawnego ID strumienia
        }

        // streamIndex musi odpowiadać ID strumienia z metadanych PipeWire (przechowywane w .index)
        Log("MoveMouseAbsolute: x=" + std::to_string(localX) + 
            " y=" + std::to_string(localY) + 
            " stream=" + std::to_string(streamIndex) + 
            " (target: " + monitor.id + ")");

#if INPUT_BRIDGE_HAS_LIBEI
        if (eis_connected.load(std::memory_order_relaxed)) {
            EISDispatchPending();
            if (HasEISDevice()) {
                // EIS preferuje współrzędne lokalne urządzenia (urządzenie jest mapowane na monitor)
                SendEISAbsoluteMotion(localX, localY);
                return;
            }
        }
#endif

        if (m_activeBackend != ActiveBackend::Portal) return;
        if (!allow_notify_pointer) return;

        const std::string session_handle_str = GetSessionHandle();
        GVariantBuilder options_builder;
        g_variant_builder_init(&options_builder, G_VARIANT_TYPE_VARDICT);

        GVariant* parameters = g_variant_new("(o@a{sv}udd)", 
                                            session_handle_str.c_str(), 
                                            g_variant_builder_end(&options_builder), 
                                            streamIndex, 
                                            localX, 
                                            localY);

        if (m_batchMode) {
            GError* error = nullptr;
            GVariant* result = g_dbus_connection_call_sync(
                connection,
                "org.freedesktop.portal.Desktop",
                "/org/freedesktop/portal/desktop",
                "org.freedesktop.portal.RemoteDesktop",
                "NotifyPointerMotionAbsolute",
                parameters,
                nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error
            );

            if (error) {
                std::cerr << "Failed to send NotifyPointerMotionAbsolute: " << error->message 
                          << " (pos: " << localX << "," << localY << " stream: " << streamIndex << ")" << std::endl;
                g_error_free(error);
            }
            if (result) g_variant_unref(result);
            return;
        }

        g_dbus_connection_call(
            connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.RemoteDesktop",
            "NotifyPointerMotionAbsolute",
            parameters,
            nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr
        );
    }

    std::vector<MonitorInfo> GetMonitors() override {
        std::lock_guard<std::mutex> lock(m_monitorMutex);
        return m_monitors;
    }

    void SetMonitors(const std::vector<MonitorInfo>& monitors) override {
        {
           std::lock_guard<std::mutex> lock(m_monitorMutex);
           m_monitors = monitors;
           UpdateVirtualDesktopBounds();
        }

        std::cout << "Monitors updated. Virtual Desktop: " << m_virtWidth << "x" << m_virtHeight 
                  << " starting at (" << m_virtMinX << "," << m_virtMinY << ")" << std::endl;
        for (const auto& m : monitors) {
            std::cout << "  [" << m.index << "] id=" << m.id << " name=" << m.name << " " << m.width << "x" << m.height << " at (" << m.x << "," << m.y << ")" << (m.primary ? " [PRIMARY]" : "") << std::endl;
        }
    }

    bool SetCurrentMonitor(int32_t monitorIndex, int32_t width, int32_t height) override {
        Log("Selecting monitor index " + std::to_string(monitorIndex) + " with requested resolution " + std::to_string(width) + "x" + std::to_string(height));
        std::lock_guard<std::mutex> lock(m_monitorMutex);

        // Szukamy monitora po PipeWire ID (id) a nie po indeksie listy (index)
        std::string targetId = std::to_string(monitorIndex);
        auto it = std::find_if(m_monitors.begin(), m_monitors.end(),
                               [&targetId](const MonitorInfo& m) { return m.id == targetId; });

        if (it == m_monitors.end()) {
            Log("Failed to select monitor: index=" + std::to_string(monitorIndex) + " not found in monitor list.");
            return false;
        }

        m_currentMonitorIndex = monitorIndex;
        if (width > 0 && height > 0) {
            it->width = width;
            it->height = height;
            UpdateVirtualDesktopBounds();
        }

        // Resetujemy ślad myszy, aby uniknąć gwałtownych ruchów przy zmianie układu współrzędnych
        m_lastX = 0;
        m_lastY = 0;
        
        Log("Monitor selected: index=" + std::to_string(monitorIndex) + " (PipeWire Stream ID) id=" + it->id + 
            " res=" + std::to_string(it->width) + "x" + std::to_string(it->height));

        return true;
    }

    void MouseClick(int32_t button, bool down) override {
        if (!EnsureStarted()) return;
        if (m_activeBackend == ActiveBackend::Uinput && m_uinputInjector) {
            m_uinputInjector->MouseClick(button, down);
            return;
        }
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

        if (m_activeBackend != ActiveBackend::Portal) return;

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
        if (!EnsureStarted()) return;
        uint32_t evdev_code = 0;
        if ((keyCode & FLAG_RAW_MASK) == FLAG_RAW_LINUX) {
            evdev_code = keyCode & ~FLAG_RAW_MASK;
        } else if ((keyCode & FLAG_RAW_MASK) == FLAG_RAW_WINDOWS) {
            evdev_code = KeyTranslator::WindowsToLinux(keyCode & ~FLAG_RAW_MASK);
        } else {
            evdev_code = KeyTranslator::WindowsToLinux(keyCode);
        }
        if (m_activeBackend == ActiveBackend::Uinput && m_uinputInjector) {
            if (evdev_code != 0) {
                m_uinputInjector->KeyPress(evdev_code, down);
            }
            return;
        }
#if INPUT_BRIDGE_HAS_LIBEI
        if (keyboard_routing == KeyboardRouting::EIS && eis_connected.load(std::memory_order_relaxed)) {
            EISDispatchPending();
            if (HasEISDevice()) {
                if (evdev_code == 0) {
                    std::cerr << "[EIS WARN] Unrecognized key code: " << keyCode << ". Dropping input." << std::endl;
                    return;
                }

                // Try sending via EIS; if device doesn't support keyboard, fall back to Notify
                if (SendEISKeycode(evdev_code, down)) {
                    return;
                }
            }
            if (!allow_notify_keyboard) {
                Log("EIS keyboard routing requested but notify fallback is disabled.");
                return;
            }
        }
#endif

        if (m_activeBackend != ActiveBackend::Portal) return;

        if (!allow_notify_keyboard) {
            Log("Keyboard routing requested but notify fallback is disabled.");
            return;
        }

        const std::string session_handle = GetSessionHandle();
        GVariantBuilder options_builder;
        g_variant_builder_init(&options_builder, G_VARIANT_TYPE_VARDICT);

        uint32_t state = down ? 1 : 0;
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
        if (!EnsureStarted()) return;
        if (m_activeBackend == ActiveBackend::Uinput && m_uinputInjector) {
            m_uinputInjector->Scroll(delta);
            return;
        }
#if INPUT_BRIDGE_HAS_LIBEI
        if (eis_connected.load(std::memory_order_relaxed)) {
            EISDispatchPending();
            if (HasEISDevice()) {
                SendEISScroll(delta);
                return;
            }
            if (!allow_notify_pointer) {
                Log("EIS pointer routing requested but notify fallback is disabled.");
                return;
            }
        }
#endif

        if (m_activeBackend != ActiveBackend::Portal) return;

        if (!allow_notify_pointer) {
            Log("Pointer routing requested but notify fallback is disabled.");
            return;
        }

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
        if (!EnsureStarted()) return;
        if (m_activeBackend == ActiveBackend::Uinput && m_uinputInjector) {
            const uint32_t evdev = KeyTranslator::WindowsToLinux(static_cast<int32_t>(charCode));
            if (evdev > 0) {
                m_uinputInjector->KeyPress(evdev, true);
                m_uinputInjector->KeyPress(evdev, false);
            }
            return;
        }
#if INPUT_BRIDGE_HAS_LIBEI
        if (keyboard_routing == KeyboardRouting::EIS && eis_connected.load(std::memory_order_relaxed)) {
            EISDispatchPending();
            if (HasEISDevice()) {
                if (m_eis.has_keyboard) {
                    SendEISUnicodeFallback(charCode);
                    return;
                }
                if (!allow_notify_keyboard) {
                    Log("EIS Unicode routing requested but keyboard capability is missing and notify fallback is disabled.");
                    return;
                }
            }
        }
#endif

        if (m_activeBackend != ActiveBackend::Portal) return;

        if (!allow_notify_keyboard) {
            Log("Unicode routing requested but notify fallback is disabled.");
            return;
        }

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

std::unique_ptr<IPlatformInput> CreateLinuxPlatformInput() {
    return std::make_unique<PlatformInputLinux>();
}
