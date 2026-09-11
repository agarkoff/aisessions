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
    // Claude: the transcript file. OpenCode: all message and part rows.
    long long sizeBytes = 0;

    std::string updatedStr() const {
        return formatLocal(updatedMs);
    }
    std::string sizeStr() const {
        return formatSize(sizeBytes);
    }

    // "yyyy-MM-dd HH:mm" from epoch ms (local time).
    static std::string formatLocal(long long ms);
    // "12.3 KB" style; an em dash for zero, which means there was nothing to
    // measure (the transcript is gone).
    static std::string formatSize(long long bytes);
};