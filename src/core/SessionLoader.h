#pragma once
#include "Session.h"
#include <vector>

class SessionLoader {
public:
    // Loads all sessions across agents, sorted by Updated desc.
    static std::vector<Session> loadAll();

private:
    static std::vector<Session> loadClaude();
    static std::vector<Session> loadOpenCode();
};