#pragma once
#include <string>

// Persisted layout. Widths are in 96-dpi units and scaled to the window's DPI
// on load. Everything is keyed by what it describes, never by column index, so
// reordering the columns in the UI cannot silently reassign a width or the
// sort to a different column.
struct Settings {
    int leftWidth = 260;

    int agentWidth = 80;
    int sessionIdWidth = 180;
    int modelWidth = 100;
    int sizeWidth = 80;
    int updatedWidth = 132;  // "yyyy-MM-dd HH:mm"

    std::string sortBy = "updated";  // agent | sessionId | title | model | size | updated
    bool sortDescending = true;      // newest first

    static Settings load();
    void save() const;
};
