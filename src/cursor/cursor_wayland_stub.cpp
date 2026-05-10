#include "cursor_manager.hpp"

#include <string>

// Wayland intentionally hides the global pointer state from background
// processes. Without integrating with a PipeWire screencast metadata stream
// we cannot reliably inspect the cursor of another surface, so the function
// returns the safe CSS fallback.
static std::string GetWaylandCursorCssName() {
    return "default";
}
