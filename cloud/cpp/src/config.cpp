#include "config.hpp"

#include <charconv>
#include <cstdlib>
#include <string_view>

namespace saihub {

namespace {

int64_t parse_positive_i64(const char* raw, int64_t fallback) {
    if (!raw || *raw == '\0') return fallback;
    int64_t val = 0;
    std::string_view sv(raw);
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val);
    if (ec != std::errc{} || ptr != sv.data() + sv.size() || val <= 0) {
        return fallback;
    }
    return val;
}

int parse_int(const char* raw, int fallback) {
    if (!raw || *raw == '\0') return fallback;
    int val = 0;
    std::string_view sv(raw);
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val);
    if (ec != std::errc{} || ptr != sv.data() + sv.size()) {
        return fallback;
    }
    return val;
}

const char* getenv_str(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v != '\0') ? v : nullptr;
}

} // namespace

Config load_config() {
    Config cfg;
    if (const char* v = getenv_str("HOST")) cfg.host = v;
    if (const char* v = getenv_str("PORT")) cfg.port = parse_int(v, cfg.port);
    if (const char* v = getenv_str("AUTH_TIMEOUT_MS")) {
        cfg.auth_timeout_ms = parse_positive_i64(v, kDefaultAuthTimeoutMs);
    }
    if (const char* v = getenv_str("REQUEST_TIMEOUT_MS")) {
        cfg.request_timeout_ms = parse_positive_i64(v, kDefaultRequestTimeoutMs);
    }
    if (const char* v = getenv_str("ROUTING_TOKEN_SECRET")) cfg.routing_token_secret = v;
    if (const char* v = getenv_str("WEBSOCKET_TOKEN_SECRET")) cfg.websocket_token_secret = v;
    if (const char* v = getenv_str("WEBSOCKET_TOKEN_TTL_S")) {
        cfg.websocket_token_ttl_s = parse_positive_i64(v, kDefaultWebsocketTokenTtlS);
    }
    if (const char* v = getenv_str("ADMIN_TOKEN")) cfg.admin_token = v;
    if (const char* v = getenv_str("DB_PATH")) cfg.db_path = v;
    if (const char* v = getenv_str("LOG_LEVEL")) {
        std::string_view lv(v);
        if (lv == "debug") cfg.log_level = LogLevel::Debug;
        else if (lv == "info") cfg.log_level = LogLevel::Info;
        else if (lv == "warn") cfg.log_level = LogLevel::Warn;
        else if (lv == "error") cfg.log_level = LogLevel::Error;
        else if (lv == "silent") cfg.log_level = LogLevel::Silent;
    }
    return cfg;
}

} // namespace saihub
