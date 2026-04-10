#include "../platform_input.hpp"
#include <iostream>
#include <string>
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

    static void OnStartResponse(GDBusConnection *connection, const gchar *sender_name,
                                const gchar *object_path, const gchar *interface_name,
                                const gchar *signal_name, GVariant *parameters, gpointer user_data) {
        guint32 response;
        GVariant* results = nullptr;
        g_variant_get(parameters, "(u@a{sv})", &response, &results);

        PlatformInputLinux* self = static_cast<PlatformInputLinux*>(user_data);

        if (response == 0) {
            std::cout << "RemoteDesktop session successfully STARTed! Ready for input injection." << std::endl;
            {
                std::lock_guard<std::mutex> lock(self->session_mutex);
                self->is_session_ready = true;
            }
            self->session_cv.notify_all();
        } else {
            std::cerr << "RemoteDesktop session failed to START. Response: " << response << std::endl;
            self->session_cv.notify_all(); // Odblokowujemy wątek w razie porażki autoryzacji
        }
        
        if (results) {
            g_variant_unref(results);
        }
    }

    static void StartSession(PlatformInputLinux* self) {
        GError* error = nullptr;

        // Subskrybuj sygnał Response dla wywołania Start
        g_dbus_connection_signal_subscribe(
            self->connection,
            "org.freedesktop.portal.Desktop",
            "org.freedesktop.portal.Request",
            "Response",
            nullptr,
            nullptr,
            G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
            OnStartResponse,
            self,
            nullptr
        );

        std::cout << "Calling RemoteDesktop.Start on session " << self->session_handle << "..." << std::endl;

        // BŁĄD BYŁ TUTAJ: "s" nie jest typem "container". Pusty napis podajemy jako "s" prosto do argumentów
        GVariantBuilder options_builder;
        g_variant_builder_init(&options_builder, G_VARIANT_TYPE_VARDICT);

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
        
        // Klasy urządzeń zdefiniowane w portalu:
        // 1 = Klawiatura
        // 2 = Myszko-podobne (Pointer)
        // 4 = Dotykowe (Touchscreen)
        // My chcemy Klawiaturę i Myszkę (1 | 2 = 3)
        uint32_t types = 3; 

        // Dodanie flagi "types" dla SelectDevices
        g_variant_builder_add(&builder, "{sv}", "types", g_variant_new_uint32(types));

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

            // Po SelectDevices natychmiast wołamy Start
            StartSession(self);
        }
    }

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
                
                // Natychmiast deklarujemy urządzenia na sesji
                SelectDevices(self);
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
            
            // Oczekiwanie na zatwierdzenie całego łąńcucha portalu (wymaga to akcji po stronie systemu lub użytkownika) max 10 sekund
            std::cout << "Oczekiwanie (max 10 sekund) na start sesji Portalu i przydzielenie zewnetznego uprawnienia..." << std::endl;
            std::unique_lock<std::mutex> lock(session_mutex);
            if (session_cv.wait_for(lock, std::chrono::seconds(10), [this] { return this->is_session_ready; })) {
                std::cout << "Portal autoryzowany natychmiastowo! Skrypt moze isc dalej w kodzie JS." << std::endl;
            } else {
                std::cerr << "Timeout 10s minal! Oczekiwanie zakonczone błędem autoryzacji (albo zbyt powolna akceptacja)." << std::endl;
            }
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
        
        // --- PROSTA TRANSLACJA (TYMCZASOWA) ---
        // Portal zakłada na wejściu kody klawiszy EVDEV xkb (Linux Evdev scancodes minus offset 8).
        // Załóżmy, że z Node.js idzie kod typu ASCII lub kod Windowsowy.
        // Jeśli keyCode z JS odpowiada standardowmu ASCII (A=65, B=66) i ma być na Evdev:
        // A (evdev 30), B (evdev 48), C (evdev 46) itd.
        // Dla testowego "klikania", jeśli dostajemy 65 (A), musimy uderzyć z 30
        uint32_t evdev_code = keyCode; 
        
        // Tylko przykładowe mapowanie do weryfikacji litery z testu, bo mapę kodów musisz przygotowac obok.
        if (keyCode == 65 || keyCode == 'a' || keyCode == 'A') evdev_code = 30; // Litera A na klawiaturze QWERTY US
        else if (keyCode == 66 || keyCode == 'b' || keyCode == 'B') evdev_code = 48; // Litera B
        else if (keyCode == 67 || keyCode == 'c' || keyCode == 'C') evdev_code = 46; // Litera C

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
};

std::unique_ptr<IPlatformInput> CreatePlatformInput() {
    return std::make_unique<PlatformInputLinux>();
}
