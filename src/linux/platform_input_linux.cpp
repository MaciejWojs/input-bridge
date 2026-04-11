#include "../platform_input.hpp"
#include "../key_translator.hpp"
#include <iostream>
#include <string>
#include <sstream>
#include <cstdio>
#include <gio/gio.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <dlfcn.h>

using xkb_utf32_to_keysym_t = uint32_t(*)(uint32_t);

static std::string ToHex(uint32_t value) {
    std::ostringstream ss;
    ss << "0x" << std::hex << std::uppercase << value;
    return ss.str();
}
static void* xkb_handle = nullptr;
static xkb_utf32_to_keysym_t xkb_utf32_to_keysym_func = nullptr;
static bool xkb_common_initialized = false;

static void InitOptionalXkbCommon() {
    if (xkb_common_initialized) return;
    xkb_common_initialized = true;

    xkb_handle = dlopen("libxkbcommon.so.0", RTLD_LAZY | RTLD_LOCAL);
    if (!xkb_handle) {
        xkb_handle = dlopen("libxkbcommon.so", RTLD_LAZY | RTLD_LOCAL);
    }

    if (xkb_handle) {
        xkb_utf32_to_keysym_func = reinterpret_cast<xkb_utf32_to_keysym_t>(dlsym(xkb_handle, "xkb_utf32_to_keysym"));
    }
}

class PlatformInputLinux : public IPlatformInput {
    private:
    GDBusConnection* connection = nullptr;
    std::string session_handle;
    bool is_session_ready = false;
    
    GMainLoop* main_loop = nullptr;
    std::thread loop_thread;
    std::atomic<bool> is_running{false};
    guint response_signal_id = 0;
    
    std::mutex session_mutex;
    std::condition_variable session_cv;

    static bool SendKeyboardKeycode(GDBusConnection* connection, const std::string& handle, uint32_t evdev, bool down) {
        GVariantBuilder options_builder;
        g_variant_builder_init(&options_builder, G_VARIANT_TYPE_VARDICT);

        uint32_t state = down ? 1 : 0;
        GError* error = nullptr;
        GVariant* result = g_dbus_connection_call_sync(
            connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.RemoteDesktop",
            "NotifyKeyboardKeycode",
            g_variant_new("(oa{sv}iu)", handle.c_str(), &options_builder, evdev, state),
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

    static void OnPortalResponse(GDBusConnection *connection, const gchar *sender_name,
                                 const gchar *object_path, const gchar *interface_name,
                                 const gchar *signal_name, GVariant *parameters, gpointer user_data) {
        guint32 response;
        GVariant* results = nullptr;
        g_variant_get(parameters, "(u@a{sv})", &response, &results);

        PlatformInputLinux* self = static_cast<PlatformInputLinux*>(user_data);

        if (g_str_has_suffix(object_path, "createReq")) {
            if (response == 0) {
                const gchar* session_path = nullptr;
                g_variant_lookup(results, "session_handle", "s", &session_path);
                if (session_path) {
                    std::cout << "Portal session created! Session path: " << session_path << std::endl;
                    self->session_handle = session_path;
                    SelectDevices(self);
                }
            } else {
                std::cerr << "Portal session creation denied. Response: " << response << std::endl;
                self->session_cv.notify_all();
            }
        } else if (g_str_has_suffix(object_path, "selectReq")) {
            if (response == 0) {
                std::cout << "SelectDevices completed successfully." << std::endl;
                StartSession(self);
            } else {
                std::cerr << "SelectDevices denied. Response: " << response << std::endl;
                self->session_cv.notify_all();
            }
        } else if (g_str_has_suffix(object_path, "startReq")) {
            if (response == 0) {
                std::cout << "RemoteDesktop session successfully STARTed! Ready for input injection." << std::endl;
                {
                    std::lock_guard<std::mutex> lock(self->session_mutex);
                    self->is_session_ready = true;
                }
                self->session_cv.notify_all();
            } else {
                std::cerr << "RemoteDesktop session failed to START. Response: " << response << std::endl;
                self->session_cv.notify_all();
            }
        }
        
        if (results) {
            g_variant_unref(results);
        }
    }

    static void StartSession(PlatformInputLinux* self) {
        GError* error = nullptr;

        std::cout << "Calling RemoteDesktop.Start on session " << self->session_handle << "..." << std::endl;

        GVariantBuilder options_builder;
        g_variant_builder_init(&options_builder, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&options_builder, "{sv}", "handle_token", g_variant_new_string("startReq"));

        GVariant* start_result = g_dbus_connection_call_sync(
            self->connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.RemoteDesktop",
            "Start",
            g_variant_new("(osa{sv})", self->session_handle.c_str(), "", &options_builder),
            G_VARIANT_TYPE("(o)"),
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            &error
        );

        if (error) {
            std::cerr << "Failed to Start session: " << error->message << std::endl;
            g_error_free(error);
        } else {
            const gchar* request_path = nullptr;
            g_variant_get(start_result, "(&o)", &request_path);
            std::cout << "Start requested successfully. Request path: " << request_path << std::endl;
            g_variant_unref(start_result);
        }
    }

    static void SelectDevices(PlatformInputLinux* self) {
        GError* error = nullptr;
        std::cout << "Calling RemoteDesktop.SelectDevices on session " << self->session_handle << "..." << std::endl;

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
            g_variant_new("(oa{sv})", self->session_handle.c_str(), &builder),
            G_VARIANT_TYPE("(o)"),
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            &error
        );

        if (error) {
            std::cerr << "Failed to SelectDevices: " << error->message << std::endl;
            g_error_free(error);
        } else {
            const gchar* request_path = nullptr;
            g_variant_get(result, "(&o)", &request_path);
            std::cout << "SelectDevices returned request path: " << request_path << std::endl;
            g_variant_unref(result);
        }
    }

    bool Initialize(std::string& error_msg) override {
        GError* error = nullptr;
        connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
        if (!connection) {
            error_msg = std::string("Failed to connect to D-Bus session bus: ") + error->message;
            g_error_free(error);
            return false;
        }
        std::cout << "Successfully connected to D-Bus session bus." << std::endl;

        // Run the GMainLoop in a separate thread to receive asynchronous D-Bus signals
        main_loop = g_main_loop_new(nullptr, FALSE);
        is_running = true;
        loop_thread = std::thread([this]() {
            g_main_loop_run(this->main_loop);
        });

        // Subscribe to the Response signal IMMEDIATELY so we don't miss the portal reply
        response_signal_id = g_dbus_connection_signal_subscribe(
            connection,
            "org.freedesktop.portal.Desktop",
            "org.freedesktop.portal.Request",
            "Response",
            nullptr, // object path will be known after CreateSession, but we capture it globally
            nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE,
            OnPortalResponse,
            this,
            nullptr
        );

        // Parameters for CreateSession (e.g. token so we know the Request ID)
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&builder, "{sv}", "session_handle_token", g_variant_new_string("inputbridgesession"));
        g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string("createReq"));
        
        
        // Call CreateSession
        std::cout << "Requesting RemoteDesktop session..." << std::endl;
        GVariant* result = g_dbus_connection_call_sync(
            connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.RemoteDesktop",
            "CreateSession",
            g_variant_new("(a{sv})", &builder),
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
            std::cout << "Waiting (max 10 seconds) for the Portal session to start and external permissions to be granted..." << std::endl;
            std::unique_lock<std::mutex> lock(session_mutex);
            if (session_cv.wait_for(lock, std::chrono::seconds(10), [this] { return this->is_session_ready; })) {
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
    }

    ~PlatformInputLinux() {
        if (main_loop) {
            g_main_loop_quit(main_loop);
            if (loop_thread.joinable()) {
                loop_thread.join();
            }
            g_main_loop_unref(main_loop);
        }
        if (response_signal_id > 0 && connection) {
            g_dbus_connection_signal_unsubscribe(connection, response_signal_id);
        }
        if (connection) {
            g_object_unref(connection);
        }
    }

    void MoveMouseRelative(int32_t x, int32_t y) override {
        if (!is_session_ready) return;

        GVariantBuilder options_builder;
        g_variant_builder_init(&options_builder, G_VARIANT_TYPE_VARDICT);

        g_dbus_connection_call(
            connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.RemoteDesktop",
            "NotifyPointerMotion",
            g_variant_new("(oa{sv}dd)", session_handle.c_str(), &options_builder, (double)x, (double)y),
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

        GVariantBuilder options_builder;
        g_variant_builder_init(&options_builder, G_VARIANT_TYPE_VARDICT);

        g_dbus_connection_call(
            connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.RemoteDesktop",
            "NotifyPointerMotionAbsolute",
            g_variant_new("(oa{sv}udd)", session_handle.c_str(), &options_builder, 0, (double)x, (double)y),
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            nullptr,
            nullptr
        );
    }

    void MouseClick(int32_t button, bool down) override {
        if (!is_session_ready) return;

        GVariantBuilder options_builder;
        g_variant_builder_init(&options_builder, G_VARIANT_TYPE_VARDICT);

        uint32_t state = down ? 1 : 0;
        uint32_t linux_button = 0x110; 
        if (button == 1) linux_button = 0x111;
        else if (button == 2) linux_button = 0x112;

        g_dbus_connection_call(
            connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.RemoteDesktop",
            "NotifyPointerButton",
            g_variant_new("(oa{sv}iu)", session_handle.c_str(), &options_builder, linux_button, state),
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

        g_dbus_connection_call(
            connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.RemoteDesktop",
            "NotifyKeyboardKeycode",
            g_variant_new("(oa{sv}iu)", session_handle.c_str(), &options_builder, evdev_code, state),
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            nullptr,
            nullptr
        );
    }

   void TypeCharacter(char16_t charCode) override {
    if (!is_session_ready) return;

    uint32_t codepoint = static_cast<uint32_t>(charCode);
    uint32_t keysym = 0;

    // 1. Try mapping with libxkbcommon (if loaded)
    InitOptionalXkbCommon();
    if (xkb_utf32_to_keysym_func) {
        std::cout << "Mapping char '" << (char)charCode << "' (U+" << ToHex(codepoint) << ") using xkbcommon..." << std::endl;
        keysym = xkb_utf32_to_keysym_func(codepoint);
    }

    // 2. Fallback to the standard Unicode Keysym (if xkb fails)
    if (keysym == 0) {
        if (codepoint >= 0x20 && codepoint <= 0x7E) {
            keysym = codepoint; // ASCII
        } else if (codepoint > 0x7E) {
            keysym = codepoint | 0x01000000; // Standard Unicode prefix in X11
        } else {
            // Handle control keys
            if (codepoint == 0x0D) keysym = 0xFF0D;      // Enter
            else if (codepoint == 0x08) keysym = 0xFF08; // Backspace
            else if (codepoint == 0x09) keysym = 0xFF09; // Tab
            else return;
        }
    }

    // Helper function for synchronous sending
    auto send_keysym_sync = [&](bool down) {
        GVariantBuilder options_builder;
        g_variant_builder_init(&options_builder, G_VARIANT_TYPE_VARDICT);
        
        GError* error = nullptr;
        // Use CALL_SYNC to preserve character order
        GVariant* result = g_dbus_connection_call_sync(
            connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.RemoteDesktop",
            "NotifyKeyboardKeysym",
            g_variant_new("(oa{sv}iu)", session_handle.c_str(), &options_builder, (int32_t)keysym, down ? 1 : 0),
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            &error
        );

        if (error) {
            std::cerr << "[DBUS ERR] TypeCharacter: " << error->message << std::endl;
            g_error_free(error);
        }
        if (result) g_variant_unref(result);
    };

    // The sequence must be atomic for each character
    send_keysym_sync(true);  // Press
    send_keysym_sync(false); // Release
}
};

std::unique_ptr<IPlatformInput> CreatePlatformInput() {
    return std::make_unique<PlatformInputLinux>();
}
