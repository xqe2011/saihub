#include "server.hpp"
#include "html.hpp"
#include "log.hpp"
#include "protocol.hpp"
#include "ws_stream_protocol.hpp"

#include <yyjson.h>

#include <charconv>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace saihub {

namespace {

inline constexpr int64_t kAccessTokenExpiresIn = 315360000;
inline constexpr std::string_view kStaticClientId = "saihub-static-client";

const char* status_line(int status) {
    switch (status) {
        case 200: return "200 OK";
        case 201: return "201 Created";
        case 204: return "204 No Content";
        case 400: return "400 Bad Request";
        case 401: return "401 Unauthorized";
        case 403: return "403 Forbidden";
        case 404: return "404 Not Found";
        case 405: return "405 Method Not Allowed";
        case 406: return "406 Not Acceptable";
        case 409: return "409 Conflict";
        case 413: return "413 Content Too Large";
        case 415: return "415 Unsupported Media Type";
        case 422: return "422 Unprocessable Content";
        case 426: return "426 Upgrade Required";
        case 500: return "500 Internal Server Error";
        default: return "500 Internal Server Error";
    }
}

std::string query_param_from(std::string_view query, std::string_view key) {
    // Decode application/x-www-form-urlencoded query or body parameter
    auto decode = [](std::string_view sv) {
        std::string out;
        out.reserve(sv.size());
        auto hex_val = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        for (size_t i = 0; i < sv.size(); ++i) {
            if (sv[i] == '+') {
                out.push_back(' ');
            } else if (sv[i] == '%' && i + 2 < sv.size()) {
                int h1 = hex_val(sv[i + 1]);
                int h2 = hex_val(sv[i + 2]);
                if (h1 >= 0 && h2 >= 0) {
                    out.push_back(static_cast<char>((h1 << 4) | h2));
                    i += 2;
                    continue;
                }
                out.push_back('%');
            } else {
                out.push_back(sv[i]);
            }
        }
        return out;
    };

    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        std::string_view pair = (amp == std::string_view::npos)
            ? query.substr(pos)
            : query.substr(pos, amp - pos);
        size_t eq = pair.find('=');
        std::string_view k_raw = (eq == std::string_view::npos) ? pair : pair.substr(0, eq);
        if (decode(k_raw) == key) {
            return (eq == std::string_view::npos) ? std::string{} : decode(pair.substr(eq + 1));
        }
        if (amp == std::string_view::npos) break;
        pos = amp + 1;
    }
    return {};
}

std::string extract_path_from_http_url(std::string_view url) {
    // Validates http(s)://host[:port]/path... and returns pathname
    if (url.starts_with("http://")) url.remove_prefix(7);
    else if (url.starts_with("https://")) url.remove_prefix(8);
    else return {};

    size_t slash = url.find('/');
    std::string_view host = (slash == std::string_view::npos) ? url : url.substr(0, slash);
    if (host.empty()) return {};
    if (slash == std::string_view::npos) return "/";
    std::string_view rest = url.substr(slash);
    size_t q = rest.find_first_of("?#");
    if (q != std::string_view::npos) rest = rest.substr(0, q);
    return std::string(rest);
}

bool is_absolute_http_url(std::string_view url) {
    if (url.starts_with("http://")) url.remove_prefix(7);
    else if (url.starts_with("https://")) url.remove_prefix(8);
    else return false;
    size_t slash = url.find_first_of("/?#");
    std::string_view authority = (slash == std::string_view::npos) ? url : url.substr(0, slash);
    return !authority.empty();
}

std::string_view extract_digest_from_device_path(std::string_view pathname) {
    // Matches ^/device/([base58]{26,33})(?:/|$)
    if (!pathname.starts_with("/device/")) return {};
    std::string_view rest = pathname.substr(8);
    size_t slash = rest.find('/');
    std::string_view cand = (slash == std::string_view::npos) ? rest : rest.substr(0, slash);
    return is_digest(cand) ? cand : std::string_view{};
}

bool is_device_mcp_path(std::string_view pathname, std::string_view& digest_out) {
    if (!pathname.starts_with("/device/")) return false;
    std::string_view rest = pathname.substr(8);
    size_t slash = rest.find('/');
    if (slash == std::string_view::npos) return false;
    std::string_view cand = rest.substr(0, slash);
    if (!is_digest(cand)) return false;
    std::string_view tail = rest.substr(slash);
    if (tail == "/mcp" || tail == "/mcp/") {
        digest_out = cand;
        return true;
    }
    return false;
}

bool parse_positive_int(std::string_view raw, int& out) {
    if (raw.empty()) return false;
    int val = 0;
    auto [ptr, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), val);
    if (ec != std::errc{} || ptr != raw.data() + raw.size() || val < 1) return false;
    out = val;
    return true;
}

} // namespace

CloudServer::CloudServer(const Config& cfg)
    : cfg_(cfg),
      routing_key_(cfg.routing_token_secret),
      ws_key_(cfg.websocket_token_secret),
      devices_(cfg_, store_, routing_key_, ws_key_) {
    // Pre-calculate static gzip HTML pages at startup
    (void)admin_page_gzip();
    (void)oauth_echo_page_gzip();
    setup_routes();
}

CloudServer::~CloudServer() {
    stop();
}

void CloudServer::setup_routes() {
    // 1. Device control WebSocket: /cloud/device/:digest
    uWS::App::WebSocketBehavior<DeviceWsData> device_ws_behavior;
    device_ws_behavior.compression = uWS::DISABLED;
    device_ws_behavior.maxPayloadLength = 2 * 1024 * 1024;
    device_ws_behavior.idleTimeout = 0;
    device_ws_behavior.maxBackpressure = 4 * 1024 * 1024;
    device_ws_behavior.closeOnBackpressureLimit = false;
    device_ws_behavior.resetIdleTimeoutOnSend = false;
    device_ws_behavior.sendPingsAutomatically = false;
    device_ws_behavior.upgrade = [this](HttpRes* res, uWS::HttpRequest* req, us_socket_context_t* ctx) {
        std::string_view digest = req->getParameter(0);
        if (!is_digest(digest)) {
            send_json_error(res, 400, "invalid digest");
            return;
        }
        if (!iequals(req->getHeader("upgrade"), "websocket")) {
            send_json_error(res, 426, "websocket upgrade required");
            return;
        }
        if (!store_.is_whitelisted(digest)) {
            send_json_error(res, 403, "digest not in whitelist");
            return;
        }
        if (devices_.is_device_connected(digest)) {
            send_json_error(res, 409, "device already connected");
            return;
        }
        if (!device_ws_hooked_) {
            BinaryStreamWsProtocol<DeviceWsData, DeviceBinaryHandler>::install(ctx);
            device_ws_hooked_ = true;
        }
        pending_upgrade_digest_.assign(digest);
        res->template upgrade<DeviceWsData>(
            DeviceWsData{},
            req->getHeader("sec-websocket-key"),
            req->getHeader("sec-websocket-protocol"),
            req->getHeader("sec-websocket-extensions"),
            ctx
        );
        pending_upgrade_digest_.clear();
    };
    device_ws_behavior.open = [this](DeviceWS* ws) {
        devices_.on_device_ws_open(ws, pending_upgrade_digest_);
    };
    device_ws_behavior.message = [this](DeviceWS* ws, std::string_view msg, uWS::OpCode op) {
        devices_.on_device_ws_message(ws, msg, op);
    };
    device_ws_behavior.close = [this](DeviceWS* ws, int, std::string_view) {
        devices_.on_device_ws_close(ws);
    };
    app_.ws<DeviceWsData>("/cloud/device/:digest", std::move(device_ws_behavior));

    // 2. TCP stream WebSocket: /device/:digest/tcp-stream/:service
    uWS::App::WebSocketBehavior<TcpStreamWsData> stream_ws_behavior;
    stream_ws_behavior.compression = uWS::DISABLED;
    stream_ws_behavior.maxPayloadLength = 4 * 1024 * 1024;
    stream_ws_behavior.idleTimeout = 0;
    stream_ws_behavior.maxBackpressure = 4 * 1024 * 1024;
    stream_ws_behavior.closeOnBackpressureLimit = false;
    stream_ws_behavior.resetIdleTimeoutOnSend = false;
    stream_ws_behavior.sendPingsAutomatically = false;
    stream_ws_behavior.upgrade = [this](HttpRes* res, uWS::HttpRequest* req, us_socket_context_t* ctx) {
        std::string_view digest = req->getParameter(0);
        std::string_view service = req->getParameter(1);
        if (service != "dap" && service != "rfc2217") {
            req->setYield(true);
            return;
        }
        if (!is_digest(digest)) {
            send_json_error(res, 400, "invalid digest");
            return;
        }
        if (!store_.is_whitelisted(digest)) {
            send_json_error(res, 403, "digest not in whitelist");
            return;
        }
        if (!iequals(req->getHeader("upgrade"), "websocket")) {
            send_json_error(res, 426, "websocket upgrade required");
            return;
        }
        if (!ws_key_.valid()) {
            send_json_error(res, 500, "websocket token secret not configured");
            return;
        }
        std::string token = query_param_from(req->getQuery(), "token");
        double now_sec = static_cast<double>(DeviceManager::now_ms()) / 1000.0;
        auto payload = token.empty() ? std::nullopt : open_websocket_token(ws_key_, token, now_sec);
        std::string expected_path = "/tcp-stream/" + std::string(service);
        if (!payload || payload->device_public_key_digest != digest || payload->path != expected_path) {
            send_json_error(res, 401, "invalid token");
            return;
        }
        if (!stream_ws_hooked_) {
            BinaryStreamWsProtocol<TcpStreamWsData, TcpStreamBinaryHandler>::install(ctx);
            stream_ws_hooked_ = true;
        }
        devices_.handle_tcp_stream_upgrade(
            res,
            ctx,
            digest,
            service,
            payload->grant_secret,
            req->getHeader("sec-websocket-key"),
            req->getHeader("sec-websocket-protocol"),
            req->getHeader("sec-websocket-extensions")
        );
    };
    stream_ws_behavior.open = [this](TcpStreamWS* ws) {
        devices_.on_tcp_stream_ws_open(ws);
    };
    stream_ws_behavior.message = [this](TcpStreamWS* ws, std::string_view msg, uWS::OpCode op) {
        devices_.on_tcp_stream_ws_message(ws, msg, op);
    };
    stream_ws_behavior.close = [this](TcpStreamWS* ws, int, std::string_view) {
        devices_.on_tcp_stream_ws_close(ws);
    };
    app_.ws<TcpStreamWsData>("/device/:digest/tcp-stream/:service", std::move(stream_ws_behavior));

    // 3. Catch-all HTTP router
    app_.any("/*", [this](HttpRes* res, uWS::HttpRequest* req) {
        handle_http(res, req);
    });
}

bool CloudServer::init() {
    return store_.init_db(cfg_.db_path);
}

bool CloudServer::listen(int& bound_port_out) {
    bool ok = false;
    app_.listen(cfg_.host, cfg_.port, LIBUS_LISTEN_EXCLUSIVE_PORT, [&](us_listen_socket_t* sock) {
        listen_socket_ = sock;
        if (sock) {
            ok = true;
            actual_port_ = us_socket_local_port(0, reinterpret_cast<us_socket_t*>(sock));
            bound_port_out = actual_port_;
        }
    });
    if (ok) {
        devices_.attach_loop(reinterpret_cast<us_loop_t*>(uWS::Loop::get()));
    }
    return ok;
}

void CloudServer::run() {
    app_.run();
}

void CloudServer::stop() {
    if (listen_socket_) {
        us_listen_socket_close(0, listen_socket_);
        listen_socket_ = nullptr;
    }
}

void CloudServer::close_all() {
    listen_socket_ = nullptr;
    app_.close();
}

std::string CloudServer::derive_origin(uWS::HttpRequest* req) const {
    std::string_view proto = req->getHeader("x-forwarded-proto");
    std::string_view scheme = iequals(proto, "https") ? "https" : "http";
    std::string_view host = req->getHeader("x-forwarded-host");
    if (host.empty()) host = req->getHeader("host");
    if (host.empty()) {
        return std::string(scheme) + "://127.0.0.1:" + std::to_string(actual_port_ > 0 ? actual_port_ : cfg_.port);
    }
    std::string out;
    out.reserve(scheme.size() + 3 + host.size());
    out.append(scheme);
    out.append("://");
    out.append(host);
    return out;
}

void CloudServer::send_json(HttpRes* res, int status, std::string_view body) {
    res->writeStatus(status_line(status));
    res->writeHeader("content-type", "application/json");
    res->end(body);
}

void CloudServer::send_json_error(HttpRes* res, int status, std::string_view reason) {
    send_json(res, status, json_error_body(reason));
}

void CloudServer::send_html(HttpRes* res, std::string_view html_gzip) {
    res->writeStatus("200 OK");
    res->writeHeader("content-type", "text/html; charset=utf-8");
    res->writeHeader("content-encoding", "gzip");
    res->end(html_gzip);
}

void CloudServer::send_unauthorized(HttpRes* res, std::string_view origin, std::string_view pathname) {
    std::string_view mcp_digest;
    std::string metadata_path = is_device_mcp_path(pathname, mcp_digest)
        ? "/.well-known/oauth-protected-resource/device/" + std::string(mcp_digest) + "/mcp"
        : "/.well-known/oauth-protected-resource";
    std::string www = "Bearer error=\"invalid_token\", error_description=\"missing or invalid access token\", resource_metadata=\"" +
                      std::string(origin) + metadata_path + "\"";
    res->writeStatus("401 Unauthorized");
    res->writeHeader("content-type", "application/json");
    res->writeHeader("www-authenticate", www);
    res->end(R"({"reason":"missing or invalid access token"})");
}

bool CloudServer::require_admin(HttpRes* res, uWS::HttpRequest* req) {
    if (cfg_.admin_token.empty()) {
        send_json_error(res, 500, "admin token not configured");
        return false;
    }
    std::string_view auth = req->getHeader("authorization");
    if (auth.size() < 7 || !iequals(auth.substr(0, 7), "bearer ")) {
        send_json_error(res, 401, "missing or invalid admin token");
        return false;
    }
    std::string_view token = trim_ascii(auth.substr(7));
    if (token.empty() || !constant_time_equal(token, cfg_.admin_token)) {
        send_json_error(res, 401, "missing or invalid admin token");
        return false;
    }
    return true;
}

std::optional<std::string> CloudServer::require_routing_token(
    HttpRes* res,
    uWS::HttpRequest* req,
    std::string_view digest,
    std::string_view origin,
    std::string_view pathname
) {
    if (!routing_key_.valid()) {
        send_json_error(res, 500, "routing token secret not configured");
        return std::nullopt;
    }
    std::string_view auth = req->getHeader("authorization");
    if (auth.size() < 7 || !iequals(auth.substr(0, 7), "bearer ")) {
        send_unauthorized(res, origin, pathname);
        return std::nullopt;
    }
    std::string_view token = trim_ascii(auth.substr(7));
    if (token.empty()) {
        send_unauthorized(res, origin, pathname);
        return std::nullopt;
    }
    auto payload = open_routing_token(routing_key_, token);
    if (!payload || payload->device_public_key_digest != digest) {
        send_unauthorized(res, origin, pathname);
        return std::nullopt;
    }
    return payload->grant_secret;
}

void CloudServer::read_body_and_run(
    HttpRes* res,
    size_t max_bytes,
    std::function<void(std::string&& body)> cb
) {
    struct BodyState {
        std::string buf;
        size_t max_bytes = 0;
        bool aborted = false;
        bool too_large = false;
        std::function<void(std::string&&)> cb;
    };
    auto state = std::make_shared<BodyState>();
    state->max_bytes = max_bytes;
    state->cb = std::move(cb);

    res->onAborted([state]() {
        state->aborted = true;
    });
    res->onData([res, state](std::string_view chunk, bool is_last) {
        if (state->aborted || state->too_large) return;
        if (state->buf.size() + chunk.size() > state->max_bytes) {
            state->too_large = true;
            res->writeStatus("413 Content Too Large");
            res->writeHeader("content-type", "application/json");
            res->end(json_error_body("body too large"));
            return;
        }
        if (!chunk.empty()) {
            state->buf.append(chunk);
        }
        if (is_last) {
            state->cb(std::move(state->buf));
        }
    });
}

void CloudServer::handle_http(HttpRes* res, uWS::HttpRequest* req) {
    std::string_view pathname = req->getUrl();

    if (pathname == "/health") {
        handle_health(res, req);
        return;
    }
    if (pathname == "/.well-known/oauth-authorization-server") {
        handle_well_known(res, req);
        return;
    }
    if (pathname == "/.well-known/oauth-protected-resource" ||
        pathname.starts_with("/.well-known/oauth-protected-resource/")) {
        handle_protected_resource(res, req, pathname);
        return;
    }
    if (pathname == "/register") {
        handle_register(res, req);
        return;
    }
    if (pathname == "/token") {
        handle_token(res, req);
        return;
    }
    if (pathname == "/cloud/oauth/redirect") {
        handle_oauth_redirect(res, req);
        return;
    }
    if (pathname == "/cloud/oauth/echo") {
        handle_oauth_echo(res, req);
        return;
    }
    if (pathname == "/cloud/pairing/session") {
        handle_pairing_session(res, req);
        return;
    }
    if (pathname == "/cloud/pairing/token") {
        handle_pairing_token(res, req);
        return;
    }
    if (pathname == "/admin" || pathname.starts_with("/admin/")) {
        handle_admin(res, req, pathname);
        return;
    }

    // Landing: /cloud/landing/<digest>/(page|online)
    if (pathname.starts_with("/cloud/landing/")) {
        std::string_view rest = pathname.substr(15);
        size_t slash = rest.find('/');
        if (slash != std::string_view::npos) {
            std::string_view digest = rest.substr(0, slash);
            std::string_view kind = rest.substr(slash + 1);
            if (is_digest(digest) && (kind == "page" || kind == "online")) {
                handle_landing(res, req, digest, kind);
                return;
            }
        }
    }

    // Non-WS fallback for /cloud/device/<digest>
    if (pathname.starts_with("/cloud/device/")) {
        std::string_view digest = pathname.substr(14);
        if (is_digest(digest)) {
            if (req->getCaseSensitiveMethod() != "GET") {
                send_json_error(res, 405, "method not allowed");
            } else {
                send_json_error(res, 426, "websocket upgrade required");
            }
            return;
        }
    }

    // /device/<digest>(/.*)?
    if (pathname.starts_with("/device/")) {
        std::string_view rest = pathname.substr(8);
        size_t slash = rest.find('/');
        std::string_view digest = (slash == std::string_view::npos) ? rest : rest.substr(0, slash);
        std::string_view suffix = (slash == std::string_view::npos) ? std::string_view("/") : rest.substr(slash);
        if (is_digest(digest)) {
            if (is_tcp_stream_path(suffix)) {
                if (!store_.is_whitelisted(digest)) {
                    send_json_error(res, 403, "digest not in whitelist");
                    return;
                }
                if (req->getCaseSensitiveMethod() != "GET") {
                    send_json_error(res, 405, "method not allowed");
                    return;
                }
                send_json_error(res, 426, "websocket upgrade required");
                return;
            }
            handle_device_http(res, req, digest, suffix, req->getQuery());
            return;
        }
    }

    send_json_error(res, 404, "not found");
}

void CloudServer::handle_health(HttpRes* res, uWS::HttpRequest* req) {
    if (req->getCaseSensitiveMethod() != "GET") {
        send_json_error(res, 405, "method not allowed");
        return;
    }
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "status", "ok");
    yyjson_mut_obj_add_uint(doc, root, "connectedDevices", devices_.connected_devices_count());
    yyjson_mut_obj_add_uint(doc, root, "authenticatedDevices", devices_.authenticated_devices_count());
    yyjson_mut_obj_add_uint(doc, root, "activeStreams", devices_.active_streams_count());
    yyjson_mut_obj_add_uint(doc, root, "whitelistedDevices", store_.whitelist_count());
    yyjson_mut_obj_add_uint(doc, root, "cachedFiles", store_.file_cache_count());
    size_t len = 0;
    char* json = yyjson_mut_write(doc, 0, &len);
    yyjson_mut_doc_free(doc);
    res->writeStatus("200 OK");
    res->writeHeader("content-type", "application/json");
    res->writeHeader("cache-control", "no-store");
    res->end(std::string_view(json, len));
    free(json);
}

void CloudServer::handle_well_known(HttpRes* res, uWS::HttpRequest* req) {
    if (req->getCaseSensitiveMethod() != "GET") {
        send_json_error(res, 405, "method not allowed");
        return;
    }
    std::string origin = derive_origin(req);
    std::string auth_ep = origin + "/cloud/oauth/redirect";
    std::string token_ep = origin + "/token";
    std::string reg_ep = origin + "/register";

    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strn(doc, root, "issuer", origin.data(), origin.size());
    yyjson_mut_obj_add_strn(doc, root, "authorization_endpoint", auth_ep.data(), auth_ep.size());
    yyjson_mut_obj_add_strn(doc, root, "token_endpoint", token_ep.data(), token_ep.size());
    yyjson_mut_obj_add_strn(doc, root, "registration_endpoint", reg_ep.data(), reg_ep.size());
    const char* rt_arr[] = {"code"};
    const char* gt_arr[] = {"authorization_code"};
    const char* cc_arr[] = {"S256"};
    const char* ta_arr[] = {"none"};
    yyjson_mut_obj_add_val(doc, root, "response_types_supported", yyjson_mut_arr_with_str(doc, rt_arr, 1));
    yyjson_mut_obj_add_val(doc, root, "grant_types_supported", yyjson_mut_arr_with_str(doc, gt_arr, 1));
    yyjson_mut_obj_add_val(doc, root, "code_challenge_methods_supported", yyjson_mut_arr_with_str(doc, cc_arr, 1));
    yyjson_mut_obj_add_val(doc, root, "token_endpoint_auth_methods_supported", yyjson_mut_arr_with_str(doc, ta_arr, 1));

    size_t len = 0;
    char* json = yyjson_mut_write(doc, 0, &len);
    yyjson_mut_doc_free(doc);
    send_json(res, 200, std::string_view(json, len));
    free(json);
}

void CloudServer::handle_protected_resource(HttpRes* res, uWS::HttpRequest* req, std::string_view pathname) {
    if (req->getCaseSensitiveMethod() != "GET") {
        send_json_error(res, 405, "method not allowed");
        return;
    }
    std::string origin = derive_origin(req);
    static constexpr std::string_view kPrefix = "/.well-known/oauth-protected-resource";
    std::string_view suffix = pathname.substr(kPrefix.size());
    std::string resource = suffix.empty() ? (origin + "/") : (origin + std::string(suffix));

    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strn(doc, root, "resource", resource.data(), resource.size());
    const char* as_arr[] = {origin.c_str()};
    const char* bm_arr[] = {"header"};
    yyjson_mut_obj_add_val(doc, root, "authorization_servers", yyjson_mut_arr_with_str(doc, as_arr, 1));
    yyjson_mut_obj_add_val(doc, root, "bearer_methods_supported", yyjson_mut_arr_with_str(doc, bm_arr, 1));

    size_t len = 0;
    char* json = yyjson_mut_write(doc, 0, &len);
    yyjson_mut_doc_free(doc);
    send_json(res, 200, std::string_view(json, len));
    free(json);
}

void CloudServer::handle_register(HttpRes* res, uWS::HttpRequest* req) {
    if (req->getCaseSensitiveMethod() != "POST") {
        send_json_error(res, 405, "method not allowed");
        return;
    }
    read_body_and_run(res, kMaxBodyBytes, [this, res](std::string&& body) {
        std::vector<std::string> redirect_uris = {"http://127.0.0.1/callback"};
        std::optional<std::string> client_name;
        std::vector<std::string> grant_types = {"authorization_code"};
        std::vector<std::string> response_types = {"code"};
        std::string token_auth = "none";

        if (!body.empty()) {
            yyjson_doc* jdoc = yyjson_read(body.data(), body.size(), 0);
            if (jdoc) {
                yyjson_val* root = yyjson_doc_get_root(jdoc);
                if (yyjson_is_obj(root)) {
                    auto read_str_arr = [](yyjson_val* arr, std::vector<std::string>& dst) {
                        if (!yyjson_is_arr(arr)) return;
                        std::vector<std::string> tmp;
                        size_t idx = 0, max = 0;
                        yyjson_val* item = nullptr;
                        yyjson_arr_foreach(arr, idx, max, item) {
                            if (!yyjson_is_str(item)) return;
                            tmp.emplace_back(yyjson_get_str(item), yyjson_get_len(item));
                        }
                        dst = std::move(tmp);
                    };
                    read_str_arr(yyjson_obj_get(root, "redirect_uris"), redirect_uris);
                    yyjson_val* cn = yyjson_obj_get(root, "client_name");
                    if (yyjson_is_str(cn)) {
                        client_name = std::string(yyjson_get_str(cn), yyjson_get_len(cn));
                    }
                    read_str_arr(yyjson_obj_get(root, "grant_types"), grant_types);
                    read_str_arr(yyjson_obj_get(root, "response_types"), response_types);
                    yyjson_val* ta = yyjson_obj_get(root, "token_endpoint_auth_method");
                    if (yyjson_is_str(ta)) {
                        token_auth.assign(yyjson_get_str(ta), yyjson_get_len(ta));
                    }
                }
                yyjson_doc_free(jdoc);
            }
        }

        yyjson_mut_doc* mdoc = yyjson_mut_doc_new(nullptr);
        yyjson_mut_val* root = yyjson_mut_obj(mdoc);
        yyjson_mut_doc_set_root(mdoc, root);
        yyjson_mut_obj_add_strn(mdoc, root, "client_id", kStaticClientId.data(), kStaticClientId.size());
        yyjson_mut_obj_add_sint(mdoc, root, "client_id_issued_at", DeviceManager::now_ms() / 1000);
        if (client_name) {
            yyjson_mut_obj_add_strn(mdoc, root, "client_name", client_name->data(), client_name->size());
        }
        auto add_arr = [&](const char* key, const std::vector<std::string>& vals) {
            yyjson_mut_val* arr = yyjson_mut_arr(mdoc);
            for (const auto& v : vals) {
                yyjson_mut_arr_add_strn(mdoc, arr, v.data(), v.size());
            }
            yyjson_mut_obj_add_val(mdoc, root, key, arr);
        };
        add_arr("redirect_uris", redirect_uris);
        add_arr("grant_types", grant_types);
        add_arr("response_types", response_types);
        yyjson_mut_obj_add_strn(mdoc, root, "token_endpoint_auth_method", token_auth.data(), token_auth.size());

        size_t len = 0;
        char* json = yyjson_mut_write(mdoc, 0, &len);
        yyjson_mut_doc_free(mdoc);
        send_json(res, 201, std::string_view(json, len));
        free(json);
    });
}

void CloudServer::handle_token(HttpRes* res, uWS::HttpRequest* req) {
    if (req->getCaseSensitiveMethod() != "POST") {
        send_json_error(res, 405, "method not allowed");
        return;
    }
    if (!routing_key_.valid()) {
        send_json_error(res, 500, "routing token secret not configured");
        return;
    }
    std::string content_type(req->getHeader("content-type"));
    read_body_and_run(res, kMaxBodyBytes, [this, res, content_type = std::move(content_type)](std::string&& body) {
        std::string grant_type;
        std::string code;
        bool parsed_ok = false;

        if (content_type.find("application/json") != std::string::npos) {
            yyjson_doc* jdoc = yyjson_read(body.data(), body.size(), 0);
            if (jdoc) {
                yyjson_val* root = yyjson_doc_get_root(jdoc);
                if (yyjson_is_obj(root)) {
                    parsed_ok = true;
                    yyjson_val* gt = yyjson_obj_get(root, "grant_type");
                    yyjson_val* cd = yyjson_obj_get(root, "code");
                    if (yyjson_is_str(gt)) grant_type.assign(yyjson_get_str(gt), yyjson_get_len(gt));
                    if (yyjson_is_str(cd)) code.assign(yyjson_get_str(cd), yyjson_get_len(cd));
                }
                yyjson_doc_free(jdoc);
            }
        } else {
            parsed_ok = true;
            grant_type = query_param_from(body, "grant_type");
            code = query_param_from(body, "code");
        }

        if (!parsed_ok || grant_type != "authorization_code" || code.empty()) {
            send_json(res, 400, R"({"error":"invalid_request"})");
            return;
        }
        auto opened = open_routing_token(routing_key_, code);
        if (!opened) {
            send_json(res, 400, R"({"error":"invalid_grant"})");
            return;
        }

        yyjson_mut_doc* mdoc = yyjson_mut_doc_new(nullptr);
        yyjson_mut_val* root = yyjson_mut_obj(mdoc);
        yyjson_mut_doc_set_root(mdoc, root);
        yyjson_mut_obj_add_strn(mdoc, root, "access_token", code.data(), code.size());
        yyjson_mut_obj_add_str(mdoc, root, "token_type", "Bearer");
        yyjson_mut_obj_add_sint(mdoc, root, "expires_in", kAccessTokenExpiresIn);
        size_t len = 0;
        char* json = yyjson_mut_write(mdoc, 0, &len);
        yyjson_mut_doc_free(mdoc);
        send_json(res, 200, std::string_view(json, len));
        free(json);
    });
}

void CloudServer::handle_oauth_redirect(HttpRes* res, uWS::HttpRequest* req) {
    if (req->getCaseSensitiveMethod() != "GET") {
        send_json_error(res, 405, "method not allowed");
        return;
    }
    std::string_view query = req->getQuery();
    std::string digest = query_param_from(query, "devicePublicKeyDigest");
    if (!is_digest(digest)) {
        std::string resource = query_param_from(query, "resource");
        std::string res_path = extract_path_from_http_url(resource);
        std::string_view mcp_d;
        if (is_device_mcp_path(res_path, mcp_d)) {
            digest.assign(mcp_d);
        } else {
            digest = std::string(extract_digest_from_device_path(res_path));
        }
    }
    if (!is_digest(digest)) {
        send_json_error(res, 400, "invalid devicePublicKeyDigest");
        return;
    }
    std::string redirect_uri = query_param_from(query, "redirect_uri");
    if (!is_absolute_http_url(redirect_uri)) {
        send_json_error(res, 400, "invalid redirect_uri");
        return;
    }
    std::string state = query_param_from(query, "state");
    send_html(res, pairing_page_gzip(digest, redirect_uri, state));
}

void CloudServer::handle_oauth_echo(HttpRes* res, uWS::HttpRequest* req) {
    if (req->getCaseSensitiveMethod() != "GET") {
        send_json_error(res, 405, "method not allowed");
        return;
    }
    send_html(res, oauth_echo_page_gzip());
}

void CloudServer::handle_pairing_session(HttpRes* res, uWS::HttpRequest* req) {
    if (req->getCaseSensitiveMethod() != "POST") {
        send_json_error(res, 405, "method not allowed");
        return;
    }
    read_body_and_run(res, kMaxBodyBytes, [this, res](std::string&& body) {
        yyjson_doc* jdoc = yyjson_read(body.data(), body.size(), 0);
        if (!jdoc || !yyjson_is_obj(yyjson_doc_get_root(jdoc))) {
            if (jdoc) yyjson_doc_free(jdoc);
            send_json_error(res, 400, "invalid body");
            return;
        }
        yyjson_val* root = yyjson_doc_get_root(jdoc);
        yyjson_val* dg_v = yyjson_obj_get(root, "devicePublicKeyDigest");
        yyjson_val* nm_v = yyjson_obj_get(root, "name");
        std::string digest = yyjson_is_str(dg_v) ? std::string(yyjson_get_str(dg_v), yyjson_get_len(dg_v)) : "";
        std::string name = yyjson_is_str(nm_v) ? std::string(trim_ascii(std::string_view(yyjson_get_str(nm_v), yyjson_get_len(nm_v)))) : "";
        yyjson_doc_free(jdoc);

        if (!is_digest(digest)) {
            send_json_error(res, 400, "invalid devicePublicKeyDigest");
            return;
        }
        if (name.empty() || name.size() > kMaxPairingNameLen) {
            send_json_error(res, 400, "invalid name");
            return;
        }
        if (!store_.is_whitelisted(digest)) {
            send_json_error(res, 403, "digest not in whitelist");
            return;
        }
        devices_.handle_pairing_session(res, digest, name);
    });
}

void CloudServer::handle_pairing_token(HttpRes* res, uWS::HttpRequest* req) {
    if (req->getCaseSensitiveMethod() != "POST") {
        send_json_error(res, 405, "method not allowed");
        return;
    }
    if (!routing_key_.valid()) {
        send_json_error(res, 500, "routing token secret not configured");
        return;
    }
    read_body_and_run(res, kMaxBodyBytes, [this, res](std::string&& body) {
        yyjson_doc* jdoc = yyjson_read(body.data(), body.size(), 0);
        if (!jdoc || !yyjson_is_obj(yyjson_doc_get_root(jdoc))) {
            if (jdoc) yyjson_doc_free(jdoc);
            send_json_error(res, 400, "invalid body");
            return;
        }
        yyjson_val* root = yyjson_doc_get_root(jdoc);
        yyjson_val* dg_v = yyjson_obj_get(root, "devicePublicKeyDigest");
        yyjson_val* st_v = yyjson_obj_get(root, "sessionToken");
        std::string digest = yyjson_is_str(dg_v) ? std::string(yyjson_get_str(dg_v), yyjson_get_len(dg_v)) : "";
        std::string session_token = yyjson_is_str(st_v) ? std::string(yyjson_get_str(st_v), yyjson_get_len(st_v)) : "";
        yyjson_doc_free(jdoc);

        if (!is_digest(digest)) {
            send_json_error(res, 400, "invalid devicePublicKeyDigest");
            return;
        }
        if (session_token.empty()) {
            send_json_error(res, 400, "invalid sessionToken");
            return;
        }
        if (!store_.is_whitelisted(digest)) {
            send_json_error(res, 403, "digest not in whitelist");
            return;
        }
        devices_.handle_pairing_token(res, digest, session_token);
    });
}

void CloudServer::handle_admin(HttpRes* res, uWS::HttpRequest* req, std::string_view pathname) {
    std::string_view method = req->getCaseSensitiveMethod();
    if (pathname == "/admin" || pathname == "/admin/") {
        if (method != "GET") {
            send_json_error(res, 405, "method not allowed");
            return;
        }
        send_html(res, admin_page_gzip());
        return;
    }

    if (pathname == "/admin/devices/whitelist") {
        if (!require_admin(res, req)) return;
        if (method == "GET") {
            std::string raw_page = query_param_from(req->getQuery(), "page");
            int page = 1;
            if (!raw_page.empty() && !parse_positive_int(raw_page, page)) {
                send_json_error(res, 400, "invalid page");
                return;
            }
            auto listed = store_.list_whitelist(page);
            yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
            yyjson_mut_val* root = yyjson_mut_obj(doc);
            yyjson_mut_doc_set_root(doc, root);
            yyjson_mut_obj_add_int(doc, root, "page", listed.page);
            yyjson_mut_obj_add_int(doc, root, "pageSize", listed.page_size);
            yyjson_mut_obj_add_uint(doc, root, "total", listed.total);
            yyjson_mut_val* items = yyjson_mut_arr(doc);
            for (const auto& item : listed.items) {
                yyjson_mut_val* obj = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_strn(doc, obj, "digest", item.digest.data(), item.digest.size());
                yyjson_mut_obj_add_sint(doc, obj, "created_at", item.created_at);
                yyjson_mut_arr_append(items, obj);
            }
            yyjson_mut_obj_add_val(doc, root, "items", items);
            size_t len = 0;
            char* json = yyjson_mut_write(doc, 0, &len);
            yyjson_mut_doc_free(doc);
            send_json(res, 200, std::string_view(json, len));
            free(json);
            return;
        }
        if (method == "POST") {
            read_body_and_run(res, kMaxBodyBytes, [this, res](std::string&& body) {
                yyjson_doc* jdoc = yyjson_read(body.data(), body.size(), 0);
                if (!jdoc) {
                    send_json_error(res, 400, "invalid body");
                    return;
                }
                yyjson_val* root = yyjson_doc_get_root(jdoc);
                yyjson_val* dg_v = yyjson_is_obj(root) ? yyjson_obj_get(root, "digest") : nullptr;
                std::string digest = yyjson_is_str(dg_v) ? std::string(yyjson_get_str(dg_v), yyjson_get_len(dg_v)) : "";
                yyjson_doc_free(jdoc);
                if (!is_digest(digest)) {
                    send_json_error(res, 400, "invalid digest");
                    return;
                }
                auto [created, created_at] = store_.insert_whitelist(digest, DeviceManager::now_ms());
                if (!created) {
                    send_json_error(res, 409, "digest already whitelisted");
                    return;
                }
                LOG_INFO("[admin] whitelist add digest=" << digest);
                yyjson_mut_doc* mdoc = yyjson_mut_doc_new(nullptr);
                yyjson_mut_val* mroot = yyjson_mut_obj(mdoc);
                yyjson_mut_doc_set_root(mdoc, mroot);
                yyjson_mut_obj_add_strn(mdoc, mroot, "digest", digest.data(), digest.size());
                yyjson_mut_obj_add_sint(mdoc, mroot, "created_at", created_at);
                size_t len = 0;
                char* json = yyjson_mut_write(mdoc, 0, &len);
                yyjson_mut_doc_free(mdoc);
                send_json(res, 201, std::string_view(json, len));
                free(json);
            });
            return;
        }
        send_json_error(res, 405, "method not allowed");
        return;
    }

    if (pathname.starts_with("/admin/devices/whitelist/")) {
        std::string_view digest = pathname.substr(25);
        if (is_digest(digest)) {
            if (!require_admin(res, req)) return;
            if (method != "DELETE") {
                send_json_error(res, 405, "method not allowed");
                return;
            }
            if (!store_.delete_whitelist(digest)) {
                send_json_error(res, 404, "not found");
                return;
            }
            LOG_INFO("[admin] whitelist delete digest=" << digest);
            devices_.disconnect_device(digest, 1008, "removed from whitelist");
            yyjson_mut_doc* mdoc = yyjson_mut_doc_new(nullptr);
            yyjson_mut_val* mroot = yyjson_mut_obj(mdoc);
            yyjson_mut_doc_set_root(mdoc, mroot);
            yyjson_mut_obj_add_strn(mdoc, mroot, "digest", digest.data(), digest.size());
            size_t len = 0;
            char* json = yyjson_mut_write(mdoc, 0, &len);
            yyjson_mut_doc_free(mdoc);
            send_json(res, 200, std::string_view(json, len));
            free(json);
            return;
        }
    }

    if (pathname == "/admin/file-cache") {
        if (!require_admin(res, req)) return;
        if (method == "GET") {
            std::string raw_page = query_param_from(req->getQuery(), "page");
            int page = 1;
            if (!raw_page.empty() && !parse_positive_int(raw_page, page)) {
                send_json_error(res, 400, "invalid page");
                return;
            }
            auto listed = store_.list_cached_files(page);
            yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
            yyjson_mut_val* root = yyjson_mut_obj(doc);
            yyjson_mut_doc_set_root(doc, root);
            yyjson_mut_obj_add_int(doc, root, "page", listed.page);
            yyjson_mut_obj_add_int(doc, root, "pageSize", listed.page_size);
            yyjson_mut_obj_add_uint(doc, root, "total", listed.total);
            yyjson_mut_val* items = yyjson_mut_arr(doc);
            for (const auto& item : listed.items) {
                yyjson_mut_val* obj = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_strn(doc, obj, "version", item.version.data(), item.version.size());
                yyjson_mut_obj_add_strn(doc, obj, "filename", item.filename.data(), item.filename.size());
                yyjson_mut_obj_add_uint(doc, obj, "bytes", item.bytes);
                yyjson_mut_arr_append(items, obj);
            }
            yyjson_mut_obj_add_val(doc, root, "items", items);
            size_t len = 0;
            char* json = yyjson_mut_write(doc, 0, &len);
            yyjson_mut_doc_free(doc);
            send_json(res, 200, std::string_view(json, len));
            free(json);
            return;
        }
        if (method == "POST") {
            read_body_and_run(res, kMaxFileCacheBytes + 4096, [this, res](std::string&& body) {
                yyjson_doc* jdoc = yyjson_read(body.data(), body.size(), 0);
                if (!jdoc) {
                    send_json_error(res, 400, "invalid body");
                    return;
                }
                yyjson_val* root = yyjson_doc_get_root(jdoc);
                if (!yyjson_is_obj(root)) {
                    yyjson_doc_free(jdoc);
                    send_json_error(res, 400, "invalid body");
                    return;
                }
                yyjson_val* ver_v = yyjson_obj_get(root, "version");
                yyjson_val* fn_v = yyjson_obj_get(root, "filename");
                yyjson_val* ct_v = yyjson_obj_get(root, "content");
                std::string version = yyjson_is_str(ver_v) ? std::string(yyjson_get_str(ver_v), yyjson_get_len(ver_v)) : "";
                std::string filename = yyjson_is_str(fn_v) ? std::string(yyjson_get_str(fn_v), yyjson_get_len(fn_v)) : "";
                if (!yyjson_is_str(ver_v) || !is_cached_version(version)) {
                    yyjson_doc_free(jdoc);
                    send_json_error(res, 400, "invalid version");
                    return;
                }
                if (!yyjson_is_str(fn_v) || !is_cached_filename(filename)) {
                    yyjson_doc_free(jdoc);
                    send_json_error(res, 400, "invalid filename");
                    return;
                }
                std::string serialized;
                if (yyjson_is_str(ct_v)) {
                    yyjson_doc* inner = yyjson_read(yyjson_get_str(ct_v), yyjson_get_len(ct_v), 0);
                    if (!inner || !yyjson_is_obj(yyjson_doc_get_root(inner))) {
                        if (inner) yyjson_doc_free(inner);
                        yyjson_doc_free(jdoc);
                        send_json_error(res, 400, "invalid content");
                        return;
                    }
                    size_t clen = 0;
                    char* cjson = yyjson_write(inner, 0, &clen);
                    yyjson_doc_free(inner);
                    if (!cjson) {
                        yyjson_doc_free(jdoc);
                        send_json_error(res, 400, "invalid content");
                        return;
                    }
                    serialized.assign(cjson, clen);
                    free(cjson);
                } else if (yyjson_is_obj(ct_v)) {
                    size_t clen = 0;
                    char* cjson = yyjson_val_write(ct_v, 0, &clen);
                    if (!cjson) {
                        yyjson_doc_free(jdoc);
                        send_json_error(res, 400, "invalid content");
                        return;
                    }
                    serialized.assign(cjson, clen);
                    free(cjson);
                } else {
                    yyjson_doc_free(jdoc);
                    send_json_error(res, 400, "invalid content");
                    return;
                }
                yyjson_doc_free(jdoc);

                if (serialized.size() > kMaxFileCacheBytes) {
                    send_json_error(res, 413, "content too large");
                    return;
                }
                size_t bytes = serialized.size();
                bool created = store_.put_cached_file(version, filename, std::move(serialized));
                LOG_INFO("[admin] file-cache " << (created ? "create" : "update")
                         << " version=" << version << " filename=" << filename << " bytes=" << bytes);
                yyjson_mut_doc* mdoc = yyjson_mut_doc_new(nullptr);
                yyjson_mut_val* mroot = yyjson_mut_obj(mdoc);
                yyjson_mut_doc_set_root(mdoc, mroot);
                yyjson_mut_obj_add_strn(mdoc, mroot, "version", version.data(), version.size());
                yyjson_mut_obj_add_strn(mdoc, mroot, "filename", filename.data(), filename.size());
                size_t len = 0;
                char* json = yyjson_mut_write(mdoc, 0, &len);
                yyjson_mut_doc_free(mdoc);
                send_json(res, created ? 201 : 200, std::string_view(json, len));
                free(json);
            });
            return;
        }
        send_json_error(res, 405, "method not allowed");
        return;
    }

    if (pathname.starts_with("/admin/file-cache/")) {
        std::string_view rest = pathname.substr(18);
        size_t slash = rest.find('/');
        if (slash != std::string_view::npos) {
            std::string_view version = rest.substr(0, slash);
            std::string_view filename = rest.substr(slash + 1);
            if (is_cached_version(version) && is_cached_filename(filename)) {
                if (!require_admin(res, req)) return;
                if (method != "DELETE") {
                    send_json_error(res, 405, "method not allowed");
                    return;
                }
                if (!store_.delete_cached_file(version, filename)) {
                    send_json_error(res, 404, "not found");
                    return;
                }
                LOG_INFO("[admin] file-cache delete version=" << version << " filename=" << filename);
                yyjson_mut_doc* mdoc = yyjson_mut_doc_new(nullptr);
                yyjson_mut_val* mroot = yyjson_mut_obj(mdoc);
                yyjson_mut_doc_set_root(mdoc, mroot);
                yyjson_mut_obj_add_strn(mdoc, mroot, "version", version.data(), version.size());
                yyjson_mut_obj_add_strn(mdoc, mroot, "filename", filename.data(), filename.size());
                size_t len = 0;
                char* json = yyjson_mut_write(mdoc, 0, &len);
                yyjson_mut_doc_free(mdoc);
                send_json(res, 200, std::string_view(json, len));
                free(json);
                return;
            }
        }
    }

    send_json_error(res, 404, "not found");
}

void CloudServer::handle_landing(HttpRes* res, uWS::HttpRequest* req, std::string_view digest, std::string_view kind) {
    if (req->getCaseSensitiveMethod() != "GET") {
        send_json_error(res, 405, "method not allowed");
        return;
    }
    if (!is_digest(digest)) {
        send_json_error(res, 400, "invalid digest");
        return;
    }
    if (kind == "page") {
        send_html(res, landing_page_gzip(derive_origin(req), digest));
        return;
    }
    bool online = devices_.is_device_online(digest);
    res->writeStatus("200 OK");
    res->writeHeader("content-type", "application/json");
    res->writeHeader("cache-control", "no-store");
    res->end(online ? R"({"online":true})" : R"({"online":false})");
}

void CloudServer::handle_device_http(
    HttpRes* res,
    uWS::HttpRequest* req,
    std::string_view digest,
    std::string_view suffix,
    std::string_view query
) {
    if (!is_digest(digest)) {
        send_json_error(res, 400, "invalid digest");
        return;
    }
    if (!store_.is_whitelisted(digest)) {
        send_json_error(res, 403, "digest not in whitelist");
        return;
    }
    std::string origin = derive_origin(req);
    std::string_view pathname = req->getUrl();
    auto grant_secret = require_routing_token(res, req, digest, origin, pathname);
    if (!grant_secret) return;

    std::string method(req->getCaseSensitiveMethod());
    if (suffix == "/mcp" && (method == "GET" || method == "DELETE")) {
        res->writeStatus("405 Method Not Allowed");
        res->writeHeader("Allow", "POST, OPTIONS");
        res->end("");
        return;
    }

    std::string path = suffix.empty() ? "/" : std::string(suffix);
    if (!query.empty()) {
        path.push_back('?');
        path.append(query);
    }

    std::string_view ct_sv = req->getHeader("content-type");
    bool has_ct = (ct_sv.data() != nullptr);
    std::string content_type(ct_sv);
    std::string accept(req->getHeader("accept"));

    std::vector<std::pair<std::string, std::string>> fwd_headers;
    fwd_headers.reserve(kForwardedHeaders.size());
    for (std::string_view name : kForwardedHeaders) {
        std::string_view val = req->getHeader(name);
        if (val.data() != nullptr && val.size() <= kMaxHeaderValueLen) {
            fwd_headers.emplace_back(std::string(name), std::string(val));
        }
    }

    std::string digest_copy(digest);
    read_body_and_run(res, kMaxBodyBytes, [
        this, res,
        digest = std::move(digest_copy),
        method = std::move(method),
        path = std::move(path),
        fwd_headers = std::move(fwd_headers),
        has_ct,
        content_type = std::move(content_type),
        accept = std::move(accept),
        grant_secret = std::move(*grant_secret)
    ](std::string&& body) mutable {
        if ((has_ct || !body.empty()) && !is_json_content_type(content_type)) {
            send_json(res, 415, unsupported_content_type_json(content_type, has_ct));
            return;
        }
        if (!accept.empty() && !accepts_json(accept)) {
            send_json(res, 406, unsupported_content_type_json(accept, true));
            return;
        }
        devices_.handle_proxy(
            res,
            digest,
            method,
            path,
            std::move(fwd_headers),
            body,
            has_ct,
            content_type,
            grant_secret
        );
    });
}

} // namespace saihub
