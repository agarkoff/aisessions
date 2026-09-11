#include "Session.h"
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