#include "cursor_manager.hpp"

#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>

#include <cctype>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#undef KeyPress
#undef KeyRelease

namespace {

const std::unordered_map<std::string, std::string>& X11CursorNameMap() {
    static const std::unordered_map<std::string, std::string> kMap = {
        { "left_ptr",            "default"      },
        { "default",             "default"      },
        { "arrow",               "default"      },
        { "top_left_arrow",      "default"      },

        { "hand1",               "pointer"      },
        { "hand2",               "pointer"      },
        { "pointing_hand",       "pointer"      },
        { "pointer",             "pointer"      },

        { "xterm",               "text"         },
        { "text",                "text"         },
        { "ibeam",               "text"         },

        { "vertical-text",       "vertical-text" },

        { "crosshair",           "crosshair"    },
        { "tcross",              "crosshair"    },
        { "cross",               "crosshair"    },

        { "watch",               "wait"         },
        { "wait",                "wait"         },

        { "left_ptr_watch",      "progress"     },
        { "progress",            "progress"     },
        { "half-busy",           "progress"     },

        { "question_arrow",      "help"         },
        { "help",                "help"         },
        { "whats_this",          "help"         },

        { "fleur",               "move"         },
        { "move",                "move"         },
        { "all-scroll",          "all-scroll"   },

        { "openhand",            "grab"         },
        { "grab",                "grab"         },
        { "closedhand",          "grabbing"     },
        { "grabbing",            "grabbing"     },

        { "not-allowed",         "not-allowed"  },
        { "forbidden",           "not-allowed"  },
        { "circle",              "not-allowed"  },
        { "crossed_circle",      "not-allowed"  },

        { "no-drop",             "no-drop"      },
        { "dnd-no-drop",         "no-drop"      },

        { "copy",                "copy"         },
        { "dnd-copy",            "copy"         },
        { "alias",               "alias"        },
        { "dnd-link",            "alias"        },
        { "context-menu",        "context-menu" },

        { "zoom-in",             "zoom-in"      },
        { "zoom-out",            "zoom-out"     },

        { "col-resize",          "col-resize"   },
        { "row-resize",          "row-resize"   },

        { "sb_h_double_arrow",   "ew-resize"    },
        { "ew-resize",           "ew-resize"    },
        { "h_double_arrow",      "ew-resize"    },

        { "sb_v_double_arrow",   "ns-resize"    },
        { "ns-resize",           "ns-resize"    },
        { "v_double_arrow",      "ns-resize"    },

        { "top_left_corner",     "nwse-resize"  },
        { "bottom_right_corner", "nwse-resize"  },
        { "nwse-resize",         "nwse-resize"  },
        { "size_fdiag",          "nwse-resize"  },

        { "top_right_corner",    "nesw-resize"  },
        { "bottom_left_corner",  "nesw-resize"  },
        { "nesw-resize",         "nesw-resize"  },
        { "size_bdiag",          "nesw-resize"  },

        { "top_side",            "n-resize"     },
        { "n-resize",            "n-resize"     },
        { "bottom_side",         "s-resize"     },
        { "s-resize",            "s-resize"     },
        { "left_side",           "w-resize"     },
        { "w-resize",            "w-resize"     },
        { "right_side",          "e-resize"     },
        { "e-resize",            "e-resize"     },
    };
    return kMap;
}

std::string NormalizeCursorName(std::string_view raw) {
    std::string lowered;
    lowered.reserve(raw.size());
    for (char c : raw) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lowered;
}

} // namespace

static std::string GetX11CursorCssName() {
    static std::mutex display_mutex;
    std::lock_guard<std::mutex> lock(display_mutex);

    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        return "default";
    }

    int event_base = 0;
    int error_base = 0;
    if (!XFixesQueryExtension(display, &event_base, &error_base)) {
        XCloseDisplay(display);
        return "default";
    }

    XFixesCursorImage* image = XFixesGetCursorImage(display);
    if (image == nullptr) {
        XCloseDisplay(display);
        return "default";
    }

    std::string css = "default";
    if (image->name != nullptr && image->name[0] != '\0') {
        const std::string normalized = NormalizeCursorName(image->name);
        const auto& map = X11CursorNameMap();
        auto it = map.find(normalized);
        if (it != map.end()) {
            css = it->second;
        }
    }

    XFree(image);
    XCloseDisplay(display);
    return css;
}
