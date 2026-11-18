#pragma once

#include <cstdint>
#include <string>

namespace saihub {

constexpr int64_t kDefaultAuthTimeoutMs = 10000;
constexpr int64_t kDefaultRequestTimeoutMs = 55000;

enum class LogLevel {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
    Silent = 4,
};

struct Config {
    std::string host = "0.0.0.0";
    int port = 8787;
    int64_t auth_timeout_ms = kDefaultAuthTimeoutMs;
    int64_t request_timeout_ms = kDefaultRequestTimeoutMs;
    std::string routing_token_secret;
    std::string websocket_token_secret;
    int64_t websocket_token_ttl_s = 0;
    std::string admin_token;
    std::string db_path;
    LogLevel log_level = LogLevel::Info;
};

Config load_config();

} // namespace saihub
