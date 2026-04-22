#include "../platform_input.hpp"
#include "../key_translator.hpp"
#include <gio/gio.h>
#include <iostream>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <vector>

// ---------------- RAII ----------------

struct GErrorDeleter {
    void operator()(GError* e) const { if (e) g_error_free(e); }
};
using GErrorPtr = std::unique_ptr<GError, GErrorDeleter>;

struct GVariantDeleter {
    void operator()(GVariant* v) const { if (v) g_variant_unref(v); }
};
using GVariantPtr = std::unique_ptr<GVariant, GVariantDeleter>;

struct GDBusConnectionDeleter {
    void operator()(GDBusConnection* c) const { if (c) g_object_unref(c); }
};
using GDBusConnectionPtr = std::unique_ptr<GDBusConnection, GDBusConnectionDeleter>;
using GMainLoopPtr = std::unique_ptr<GMainLoop, decltype(&g_main_loop_unref)>;
using GMainContextPtr = std::unique_ptr<GMainContext, decltype(&g_main_context_unref)>;

// Pomocnik do bezpiecznego pakowania wariantów (zapobiega problemom z floating references i wyciekom)
static GVariantPtr wrap_variant(GVariant* v) {
    if (!v) return nullptr;
    if (g_variant_is_floating(v)) {
        g_variant_ref_sink(v);
    }
    return GVariantPtr(v);
}

// ---------------- VariantDict ----------------

class VariantDict {
    GVariantBuilder builder{};
    public:
    VariantDict() {
        g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    }

    void add(const char* key, GVariant* value) {
        // g_variant_builder_add konsumuje floating reference z value
        g_variant_builder_add(&builder, "{sv}", key, value);
    }

    GVariant* end() {
        return g_variant_ref_sink(g_variant_builder_end(&builder));
    }
};

// ---------------- DBUS ----------------

static GVariantPtr CallSync(GDBusConnection* conn, const char* method, GVariant* params) {
    if (!conn) return nullptr;


    if (params && g_variant_is_floating(params))
        g_variant_ref_sink(params);

    GError* err = nullptr;
    GVariant* res = g_dbus_connection_call_sync(
        conn,
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.RemoteDesktop",
        method,
        params,
        nullptr,
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &err
    );

    if (params)
        g_variant_unref(params);

    if (err) {
        std::cerr << "[DBUS ERROR] " << err->message << std::endl;
        g_error_free(err);
        return nullptr;
    }

    return wrap_variant(res);
}

// ---------------- CLASS ----------------

class PlatformInputLinux : public IPlatformInput {
    private:
    GDBusConnectionPtr connection{ nullptr };
    std::string session_handle;
    bool is_session_ready = false;
    bool m_batchMode = false;

    GMainContextPtr main_context{ nullptr, g_main_context_unref };
    GMainLoopPtr main_loop{ nullptr, g_main_loop_unref };
    std::jthread loop_thread;
    std::atomic<bool> is_stopping = false;
    guint response_signal_id = 0;

    std::mutex session_mutex;
    std::condition_variable session_cv;

    // -------- PORTAL CALLBACK --------

    static void OnPortalResponse(
        GDBusConnection*,
        const gchar* sender_name,
        const gchar* object_path,
        const gchar* interface_name,
        const gchar* signal_name,
        GVariant* parameters,
        gpointer user_data) {

        auto* self = static_cast<PlatformInputLinux*>(user_data);

        if (self->is_stopping.load(std::memory_order_acquire))
            return;

        if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(ua{sv})")))
            return;

        guint32 response;
        GVariant* results_raw = nullptr;

        // FIX: Użycie @ pozwala bezpiecznie pobrać wskaźnik GVariant*
        g_variant_get(parameters, "(u@a{sv})", &response, &results_raw);
        GVariantPtr results = wrap_variant(results_raw);

        // FIX: Rozpoznawanie handle_token ze ścieżki obiektu (object_path), a nie z wyników
        std::string path_str = object_path ? object_path : "";
        std::string t = "";

        if (path_str.find("createReq") != std::string::npos) t = "createReq";
        else if (path_str.find("selectReq") != std::string::npos) t = "selectReq";
        else if (path_str.find("startReq") != std::string::npos) t = "startReq";

        if (t == "createReq") {
            if (response == 0) {
                const gchar* session_path = nullptr;
                if (!g_variant_lookup(results.get(), "session_handle", "&s", &session_path) || !session_path)
                    return;

                {
                    std::lock_guard<std::mutex> lock(self->session_mutex);
                    self->session_handle = session_path;
                }

                VariantDict dict;
                dict.add("handle_token", g_variant_new_string("selectReq"));

                g_dbus_connection_call(
                    self->connection.get(),
                    "org.freedesktop.portal.Desktop",
                    "/org/freedesktop/portal/desktop",
                    "org.freedesktop.portal.RemoteDesktop",
                    "SelectDevices",
                    g_variant_new("(o@a{sv})", session_path, dict.end()),
                    G_VARIANT_TYPE("(o)"),
                    G_DBUS_CALL_FLAGS_NONE,
                    -1,
                    nullptr,
                    nullptr,
                    nullptr
                );
            }
        } else if (t == "selectReq") {
            if (response == 0) {
                std::string handle;
                {
                    std::lock_guard<std::mutex> lock(self->session_mutex);
                    handle = self->session_handle;
                }

                VariantDict dict;
                dict.add("handle_token", g_variant_new_string("startReq"));

                g_dbus_connection_call(
                    self->connection.get(),
                    "org.freedesktop.portal.Desktop",
                    "/org/freedesktop/portal/desktop",
                    "org.freedesktop.portal.RemoteDesktop",
                    "Start",
                    g_variant_new("(os@a{sv})", handle.c_str(), "", dict.end()),
                    G_VARIANT_TYPE("(o)"),
                    G_DBUS_CALL_FLAGS_NONE,
                    -1,
                    nullptr,
                    nullptr,
                    nullptr
                );
            }
        } else if (t == "startReq") {
            {
                std::lock_guard<std::mutex> lock(self->session_mutex);
                if (response == 0)
                    self->is_session_ready = true;
            }
            self->session_cv.notify_all();
        }
    }

    public:

    ~PlatformInputLinux() {
        is_stopping.store(true, std::memory_order_release);

        if (response_signal_id && connection)
            g_dbus_connection_signal_unsubscribe(connection.get(), response_signal_id);

        if (main_loop)
            g_main_loop_quit(main_loop.get());

        if (loop_thread.joinable())
            loop_thread.join();
    }

    // -------- INIT --------

    bool Initialize(std::string& error_msg) override {
        main_context.reset(g_main_context_new());
        g_main_context_push_thread_default(main_context.get());

        GError* err = nullptr;
        connection.reset(g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err));
        g_main_context_pop_thread_default(main_context.get());

        if (!connection) {
            error_msg = err ? err->message : "Failed to connect to session bus";
            if (err) g_error_free(err);
            return false;
        }

        main_loop.reset(g_main_loop_new(main_context.get(), FALSE));

        loop_thread = std::jthread([this](std::stop_token) {
            g_main_context_push_thread_default(main_context.get());
            g_main_loop_run(main_loop.get());
            g_main_context_pop_thread_default(main_context.get());
            });

        response_signal_id = g_dbus_connection_signal_subscribe(
            connection.get(),
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

        VariantDict dict;
        dict.add("session_handle_token", g_variant_new_string("inputbridgesession"));
        dict.add("handle_token", g_variant_new_string("createReq"));

        auto res = CallSync(
            connection.get(),
            "CreateSession",
            g_variant_new("(@a{sv})", dict.end())
        );

        if (!res) {
            error_msg = "CreateSession failed";
            return false;
        }

        std::unique_lock<std::mutex> lock(session_mutex);
        if (!session_cv.wait_for(lock, std::chrono::seconds(10), [this] {
            return is_session_ready;
            })) {
            error_msg = "Portal timeout";
            return false;
        }

        return true;
    }

    // -------- INPUT --------

    void MoveMouseRelative(int32_t x, int32_t y) override {
        std::string h;
        { std::lock_guard<std::mutex> l(session_mutex); if (!is_session_ready) return; h = session_handle; }

        VariantDict dict;
        CallSync(connection.get(), "NotifyPointerMotion",
            g_variant_new("(o@a{sv}dd)", h.c_str(), dict.end(), (double)x, (double)y));
    }

    void MoveMouseAbsolute(int32_t x, int32_t y) override {
        std::string h;
        { std::lock_guard<std::mutex> l(session_mutex); if (!is_session_ready) return; h = session_handle; }

        VariantDict dict;
        // Format: (o: handle, a{sv}: options, u: stream, d: x, d: y)
        CallSync(connection.get(), "NotifyPointerMotionAbsolute",
            g_variant_new("(o@a{sv}udd)", h.c_str(), dict.end(), (guint32)0, (double)x, (double)y));
    }

    void MouseClick(int32_t button, bool down) override {
        std::string h;
        { std::lock_guard<std::mutex> l(session_mutex); if (!is_session_ready) return; h = session_handle; }

        gint32 btn = (button == 1) ? 0x111 : (button == 2 ? 0x112 : 0x110);

        VariantDict dict;
        // Format: (o, a{sv}, i: button, u: state)
        CallSync(connection.get(), "NotifyPointerButton",
            g_variant_new("(o@a{sv}iu)", h.c_str(), dict.end(), btn, (guint32)(down ? 1 : 0)));
    }

    void KeyPress(int32_t keyCode, bool down) override {
        std::string h;
        { std::lock_guard<std::mutex> l(session_mutex); if (!is_session_ready) return; h = session_handle; }

        uint32_t evdev = KeyTranslator::WindowsToLinux(keyCode);
        if (!evdev) return;

        VariantDict dict;
        // Format: (o, a{sv}, i: keycode, u: state)
        CallSync(connection.get(), "NotifyKeyboardKeycode",
            g_variant_new("(o@a{sv}iu)", h.c_str(), dict.end(), (gint32)evdev, (guint32)(down ? 1 : 0)));
    }

    void TypeCharacter(uint32_t charCode) override {
        std::string h;
        { std::lock_guard<std::mutex> l(session_mutex); if (!is_session_ready) return; h = session_handle; }

        GDBusConnection* conn = connection.get();
        if (!conn) return;

        auto send_key = [&](uint32_t evdev, bool down) {
            VariantDict dict;
            // NotifyKeyboardKeycode: (o, a{sv}, i, u)
            GVariant* params = g_variant_new("(o@a{sv}iu)", h.c_str(), dict.end(), (gint32)evdev, (guint32)(down ? 1 : 0));
            CallSync(conn, "NotifyKeyboardKeycode", params);
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            };

        auto tap_key = [&](uint32_t evdev) {
            send_key(evdev, true);
            send_key(evdev, false);
            };

        if (charCode == 0x0D || charCode == 0x0A) { tap_key(28); return; }
        if (charCode == 0x08) { tap_key(14); return; }
        if (charCode == 0x09) { tap_key(15); return; }
        if (charCode == 0x20) { tap_key(57); return; }

        if (charCode >= 0x20 && charCode <= 0x7E) {
            // NotifyKeyboardKeysym: (o, a{sv}, i: keysym, u: state)
            auto send_sym = [&](uint32_t sym, bool down) {
                VariantDict dict;
                // FIX: (o@a{sv}iu) zamiast (o@a{sv}uu)
                GVariant* params = g_variant_new("(o@a{sv}iu)", h.c_str(), dict.end(), (gint32)sym, (guint32)(down ? 1 : 0));
                CallSync(conn, "NotifyKeyboardKeysym", params);
                };
            send_sym(charCode, true);
            send_sym(charCode, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return;
        }

        // Unicode fallback (Ctrl+Shift+U...)
        char hex[10];
        snprintf(hex, sizeof(hex), "%x", charCode);

        send_key(29, true);
        send_key(42, true);
        tap_key(22);
        send_key(42, false);
        send_key(29, false);

        for (int i = 0; hex[i] != '\0'; i++) {
            uint32_t evdev = 0;
            char c = hex[i];
            if (c == '0') evdev = 11;
            else if (c >= '1' && c <= '9') evdev = 2 + (c - '1');
            else if (c == 'a') evdev = 30;
            else if (c == 'b') evdev = 48;
            else if (c == 'c') evdev = 46;
            else if (c == 'd') evdev = 32;
            else if (c == 'e') evdev = 18;
            else if (c == 'f') evdev = 33;

            if (evdev > 0) tap_key(evdev);
        }
        tap_key(57);
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    void ExecuteEvents(const std::vector<InputEvent>& events) override {
        bool prev = m_batchMode;
        m_batchMode = true;
        IPlatformInput::ExecuteEvents(events);
        m_batchMode = prev;
    }
};