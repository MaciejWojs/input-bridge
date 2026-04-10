#include "../platform_input.hpp"
#include <iostream>
#include <string>
#include <gio/gio.h>
#include <thread>
#include <atomic>

class PlatformInputLinux : public IPlatformInput {
    private:
    GDBusConnection* connection = nullptr;
    std::string session_handle;
    
    GMainLoop* main_loop = nullptr;
    std::thread loop_thread;
    std::atomic<bool> is_running{false};
    guint response_signal_id = 0;

    static void OnCreateSessionResponse(GDBusConnection *connection, const gchar *sender_name,
                                        const gchar *object_path, const gchar *interface_name,
                                        const gchar *signal_name, GVariant *parameters, gpointer user_data) {
        guint32 response;
        GVariant* results;
        g_variant_get(parameters, "(u@a{sv})", &response, &results);

        if (response == 0) { // 0 oznacza sukces
            const gchar* session_path = nullptr;
            g_variant_lookup(results, "session_handle", "s", &session_path);
            if (session_path) {
                std::cout << "Portal session created! Session path: " << session_path << std::endl;
                PlatformInputLinux* self = static_cast<PlatformInputLinux*>(user_data);
                self->session_handle = session_path;
                
                // TODO: Następny krok to wywołanie SelectDevices i Start na ścieżce sesji
            }
        } else {
            std::cerr << "Portal session creation denied or failed. Response code: " << response << std::endl;
        }
        g_variant_unref(results);
    }

    void InitializePortal() {
        GError* error = nullptr;
        connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
        if (!connection) {
            std::cerr << "Failed to connect to D-Bus session bus: " << error->message << std::endl;
            g_error_free(error);
            return;
        }
        std::cout << "Successfully connected to D-Bus session bus." << std::endl;

        // Uruchamiamy GMainLoop w osobnym wątku, aby odbierać asynchroniczne sygnały D-Bus
        main_loop = g_main_loop_new(nullptr, FALSE);
        is_running = true;
        loop_thread = std::thread([this]() {
            g_main_loop_run(this->main_loop);
        });

        // Subskrybuj sygnał Response OD RAZU, by nie przegapić odpowiedzi portalu
        response_signal_id = g_dbus_connection_signal_subscribe(
            connection,
            "org.freedesktop.portal.Desktop",
            "org.freedesktop.portal.Request",
            "Response",
            nullptr, // object path będzie znany po wywołaniu CreateSession, ale chwytamy globalnie
            nullptr,
            G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
            OnCreateSessionResponse,
            this,
            nullptr
        );

        // Parametry dla CreateSession (np. token, abyśmy znali Request ID)
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&builder, "{sv}", "session_handle_token", g_variant_new_string("inputbridgesession"));
        
        // Wywołujemy CreateSession
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
            -1, // timeout domyślny
            nullptr,
            &error
        );

        if (error != nullptr) {
            std::cerr << "Failed to call CreateSession: " << error->message << std::endl;
            g_error_free(error);
        } else {
            const gchar* request_path = nullptr;
            g_variant_get(result, "(&o)", &request_path);
            std::cout << "CreateSession requested successfully. Request path: " << request_path << std::endl;
            g_variant_unref(result);
        }
    }

    public:
    
    PlatformInputLinux() {
        InitializePortal();
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
