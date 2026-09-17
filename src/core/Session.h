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
    // False for a Claude session whose transcript has been pruned: only its
    // history line is left, so `claude --resume` has nothing to open.
    bool resumable = true;
    // OpenCode only: the session this one was spawned from as a subagent, or
    // empty for a top-level session. `opencode session delete` cascades to a
    // session's own children, so deleting a parent removes these too even
    // though this app lists them as ordinary, separate rows.
    std::string parentId;

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