#include "Session.h"

#include <cstdio>
#include <ctime>

std::string Session::formatLocal(long long ms) {
    if (ms <= 0) return "";
    time_t t = static_cast<time_t>(ms / 1000);
    struct tm tmv{};
    localtime_s(&tmv, &t);
    char buf[32]{};
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tmv);
    return buf;
}

std::string Session::formatSize(long long bytes) {
    if (bytes <= 0) return "\xE2\x80\x94";  // em dash: nothing on disk to measure
    char buf[32]{};
    if (bytes < 1024) {
        snprintf(buf, sizeof(buf), "%lld B", bytes);
    } else if (bytes < 1024 * 1024) {
        snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    } else {
        snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
    }
    return buf;
}
