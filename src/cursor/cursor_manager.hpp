#pragma once

#include <string>

// Returns the name of the currently displayed system cursor as a string
// compatible with the CSS `cursor` property (e.g. "default", "pointer",
// "text", "crosshair", "move", "wait", "grab", ...).
//
// On platforms or sessions where the cursor cannot be inspected (notably
// Wayland without a screencast-derived metadata stream), the function returns
// "default" as a safe fallback.
std::string GetSystemCursorCssName();
