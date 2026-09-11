#pragma once
#include <string>

struct Session {
    std::string agent;
    std::string sessionId;
    std::string directory;
    std::string title;
    std::string model;
    long long createdMs = 0;
    long long updatedMs = 0;

    std::string updatedStr() const {
        return formatLocal(updatedMs);
    }

    // "yyyy-MM-dd HH:mm" from epoch ms (local time).
    static std::string formatLocal(long long ms);
};