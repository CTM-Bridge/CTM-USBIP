#pragma once

// Single source of truth for the product version. Keep the numeric and string
// forms in sync. Consumed by the C++ sources (main.cpp) and by the Win32
// version resource (app/ctm-usbip.rc).
#define CTM_VERSION_MAJOR 0
#define CTM_VERSION_MINOR 0
#define CTM_VERSION_PATCH 1
#define CTM_VERSION_BUILD 1

#define CTM_VERSION_STR      "0.0.1"
#define CTM_VERSION_FULL_STR "0.0.1.1"
#define CTM_VERSION_DISPLAY  "0.0.1 (build 1)"
