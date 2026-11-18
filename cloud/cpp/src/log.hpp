#pragma once

#include "config.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>

namespace saihub {

inline LogLevel g_log_level = LogLevel::Info;

inline void set_log_level(LogLevel level) {
    g_log_level = level;
}

inline void log_prefix(const char* level_str) {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm_utc{};
    gmtime_r(&tt, &tm_utc);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                  tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                  tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec, static_cast<long long>(ms));
    std::cerr << buf << " [" << level_str << "] ";
}

} // namespace saihub

#define LOG_INFO(expr) \
    do { \
        if (static_cast<int>(::saihub::g_log_level) <= static_cast<int>(::saihub::LogLevel::Info)) { \
            ::saihub::log_prefix("INFO"); \
            std::cerr << expr << '\n'; \
        } \
    } while (0)

#define LOG_WARN(expr) \
    do { \
        if (static_cast<int>(::saihub::g_log_level) <= static_cast<int>(::saihub::LogLevel::Warn)) { \
            ::saihub::log_prefix("WARN"); \
            std::cerr << expr << '\n'; \
        } \
    } while (0)

#define LOG_ERROR(expr) \
    do { \
        if (static_cast<int>(::saihub::g_log_level) <= static_cast<int>(::saihub::LogLevel::Error)) { \
            ::saihub::log_prefix("ERROR"); \
            std::cerr << expr << '\n'; \
        } \
    } while (0)
