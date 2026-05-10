#include "cursor_manager.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstdint>
#include <string>

namespace {

struct CursorMapping {
    std::uint16_t oemId;
    const char* css;
};

constexpr std::array<CursorMapping, 14> kMappings = {{
    { 32512, "default"     }, // IDC_ARROW
    { 32513, "text"        }, // IDC_IBEAM
    { 32514, "wait"        }, // IDC_WAIT
    { 32515, "crosshair"   }, // IDC_CROSS
    { 32516, "default"     }, // IDC_UPARROW
    { 32642, "nwse-resize" }, // IDC_SIZENWSE
    { 32643, "nesw-resize" }, // IDC_SIZENESW
    { 32644, "ew-resize"   }, // IDC_SIZEWE
    { 32645, "ns-resize"   }, // IDC_SIZENS
    { 32646, "move"        }, // IDC_SIZEALL
    { 32648, "not-allowed" }, // IDC_NO
    { 32649, "pointer"     }, // IDC_HAND
    { 32650, "progress"    }, // IDC_APPSTARTING
    { 32651, "help"        }, // IDC_HELP
}};

} // namespace

std::string GetSystemCursorCssName() {
    CURSORINFO info = {};
    info.cbSize = sizeof(info);
    if (!GetCursorInfo(&info) || info.hCursor == nullptr) {
        return "default";
    }

    const HCURSOR active = info.hCursor;
    for (const auto& m : kMappings) {
        const HCURSOR ref = LoadCursorW(nullptr, MAKEINTRESOURCEW(m.oemId));
        if (ref != nullptr && ref == active) {
            return m.css;
        }
    }

    return "default";
}
