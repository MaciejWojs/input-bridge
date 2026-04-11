#include "../platform_input.hpp"
#include "../key_translator.hpp"
#include <iostream>
#include <string>
#include <cstdio>
#include <gio/gio.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>

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
            OnPortalResponse,
            this,
            nullptr
        );

        // Parametry dla CreateSession (np. token, abyśmy znali Request ID)
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&builder, "{sv}", "session_handle_token", g_variant_new_string("inputbridgesession"));
        g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string("createReq"));
        
        
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
            error_msg = std::string("Failed to call CreateSession: ") + error->message;
            g_error_free(error);
            return false;
        } else {
            const gchar* request_path = nullptr;
            g_variant_get(result, "(&o)", &request_path);
            std::cout << "CreateSession requested successfully. Request path: " << request_path << std::endl;
            g_variant_unref(result);
            
            // Oczekiwanie na zatwierdzenie całego łąńcucha portalu (wymaga to akcji po stronie systemu lub użytkownika) max 10 sekund
            std::cout << "Oczekiwanie (max 10 sekund) na start sesji Portalu i przydzielenie zewnetznego uprawnienia..." << std::endl;
            std::unique_lock<std::mutex> lock(session_mutex);
            if (session_cv.wait_for(lock, std::chrono::seconds(10), [this] { return this->is_session_ready; })) {
                std::cout << "Portal autoryzowany natychmiastowo! Skrypt moze isc dalej w kodzie JS." << std::endl;
                return true;
            } else {
                error_msg = "Timeout 10s minal lub odrzucono! Oczekiwanie zakonczone błędem autoryzacji.";
                return false;
            }
        }
    }

    public:
    
    PlatformInputLinux() {
        // Obiekt zainicjalizuje sesję przez wywołanie Async Init
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

        // Sprawdzamy czy przyszło bitowe pole "Zostaw mnie, jestem z czystego portalu Linuksa"
        if ((keyCode & FLAG_RAW_MASK) == FLAG_RAW_LINUX) {
            evdev_code = keyCode & ~FLAG_RAW_MASK; 
        } 
        // Oraz "Jestem czystym kodem Windows i trzeba mnie przetłumaczyć na Linuksa bo tu pracuje moduł"
        else if ((keyCode & FLAG_RAW_MASK) == FLAG_RAW_WINDOWS) {
            evdev_code = KeyTranslator::WindowsToLinux(keyCode & ~FLAG_RAW_MASK);
        }
        // Domyślny przypadek wejściowy z biblioteki z Node.js (który w JS zawsze jest Windowsie Virtual-Key'em)
        else {
            evdev_code = KeyTranslator::WindowsToLinux(keyCode);
        }

        // Jeżeli translator nie znał tego klawisza, bezpiecznie odrzucamy próbę napisania "wiatru"
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

        // Zgodnie ze specyfikacją X11 / Wayland RemoteDesktop:
        // Każdy znak Unicode można przetłumaczyć na Keysym poprzez dodanie 0x01000000.
        // Nawet jeśli niektóre znaki mają dedykowane, nazwane kody w specyfikacji XKB (np. ś -> 0x1B6),
        // kompozytory Wayland (np. Mutter) najstabilniej przyjmują absolutny U-Keysym, 
        // dzięki czemu nie jest wymagana zależność od zewnętrznej biblioteki libxkbcommon-dev 
        // ani zmaganie o odpowiedni stan układu klawiatury użytkownika.
        
        int32_t keysym = charCode;
        if (charCode >= 0x0080 || charCode < 0x0020) { 
            // Dla czystego ASCII pomijamy dodawanie maski. Dla wszystkiego innego - bezwzględny uniksowy keysym U.
            keysym = charCode | 0x01000000;
        }

        GVariantBuilder options_builder;
        g_variant_builder_init(&options_builder, G_VARIANT_TYPE_VARDICT);

        // State 1 (Pressed)
        g_dbus_connection_call(
            connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.RemoteDesktop",
            "NotifyKeyboardKeysym",
            g_variant_new("(oa{sv}iu)", session_handle.c_str(), &options_builder, keysym, 1),
            nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr
        );

        // State 0 (Released)
        GVariantBuilder options_builder2;
        g_variant_builder_init(&options_builder2, G_VARIANT_TYPE_VARDICT);

        g_dbus_connection_call(
            connection,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.RemoteDesktop",
            "NotifyKeyboardKeysym",
            g_variant_new("(oa{sv}iu)", session_handle.c_str(), &options_builder2, keysym, 0),
            nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr
        );
    }
};

std::unique_ptr<IPlatformInput> CreatePlatformInput() {
    return std::make_unique<PlatformInputLinux>();
}
