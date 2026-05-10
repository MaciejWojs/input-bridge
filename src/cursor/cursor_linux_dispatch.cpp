#include "cursor_manager.hpp"

#include <cstdlib>
#include <cstring>
#include <string>

#ifdef USE_X11_BACKEND
#include "cursor_x11.cpp"
#else
#include "cursor_wayland_stub.cpp"
#endif

namespace {

bool IsWaylandSession() {
    const char* session_type = std::getenv("XDG_SESSION_TYPE");
    if (session_type != nullptr && std::strcmp(session_type, "wayland") == 0) {
        return true;
    }
    const char* wayland_display = std::getenv("WAYLAND_DISPLAY");
    return wayland_display != nullptr && wayland_display[0] != '\0';
}

} // namespace

std::string GetSystemCursorCssName() {
#ifdef USE_X11_BACKEND
    if (IsWaylandSession()) {
        return "default";
    }
    return GetX11CursorCssName();
#else
    (void)IsWaylandSession();
    return GetWaylandCursorCssName();
#endif
}
