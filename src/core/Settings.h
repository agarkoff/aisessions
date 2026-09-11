#pragma once
#include <string>

// Widths are stored in 96-dpi units and scaled to the window's DPI on load.
struct Settings {
    int leftWidth = 260;
    int col0 = 80;   // Agent
    int col1 = 180;  // Session ID
    int col3 = 100;  // Model
    int col4 = 132;  // Updated ("yyyy-MM-dd HH:mm")

    static Settings load();
    void save() const;
};