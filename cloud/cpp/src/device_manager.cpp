#include "device_manager.hpp"
#include "log.hpp"

#include <yyjson.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace saihub {

namespace {

const char* http_status_line(int status) {
    switch (status) {
        case 200: return "200 OK";
        case 201: return "201 Created";
        case 202: return "202 Accepted";
        case 204: return "204 No Content";
        case 205: return "205 Reset Content";
        case 304: return "304 Not Modified";
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
        case 502: return "502 Bad Gateway";
        case 503: return "503 Service Unavailable";
        case 504: return "504 Gateway Timeout";
        default: return nullptr;
    }
}

std::string format_status_line(int status) {
    if (const char* s = http_status_line(status)) return s;
    return std::to_string(status);
}

bool is_valid_header_name(std::string_view name) {
    if (name.empty()) return false;
    for (unsigned char c : name) {
        if (c <= 0x20 || c >= 0x7f) return false;
        switch (c) {
            case '(': case ')': case '<': case '>': case '@':
            case ',': case ';': case ':': case '\\': case '"':
            case '/': case '[': case ']': case '?': case '=':
            case '{': case '}':
                return false;
            default:
                break;
        }
    }
    return true;
}

bool is_valid_header_value(std::string_view value) {
    for (unsigned char c : value) {
        if (c == '\r' || c == '\n' || c == '\0') return false;
    }
    return true;
}

template <typename T>
T* ptr_from_node(TimerNode* node, TimerNode T::*member) {
    auto offset = reinterpret_cast<uintptr_t>(&(static_cast<T*>(nullptr)->*member));
    return reinterpret_cast<T*>(reinterpret_cast<char*>(node) - offset);
}

} // namespace

int64_t DeviceManager::now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

DeviceManager::DeviceManager(
    const Config& cfg,
    MemoryStore& store,
    const AesGcmKey& routing_key,
    const AesGcmKey& ws_key
)
    : cfg_(cfg), store_(store), routing_key_(routing_key), ws_key_(ws_key) {}

DeviceManager::~DeviceManager() {
    if (timer_) {
        us_timer_close(timer_);
        timer_ = nullptr;
    }
    for (auto& [_, dev] : devices_) {
        for (auto* p : dev->pending) delete p;
        for (auto* sw : dev->stream_pending) delete sw;
        if (dev->pairing_session_waiter) delete dev->pairing_session_waiter;
        for (auto* ptw : dev->pairing_token_waiters) delete ptw;
    }
}

void DeviceManager::attach_loop(us_loop_t* loop) {
    if (timer_) return;
    timer_ = us_create_timer(loop, 1, sizeof(DeviceManager*));
    if (timer_) {
        DeviceManager* self = this;
        std::memcpy(us_timer_ext(timer_), &self, sizeof(DeviceManager*));
    }
}

void DeviceManager::close_timer() {
    if (timer_) {
        us_timer_close(timer_);
        timer_ = nullptr;
    }
}

void DeviceManager::on_us_timer(us_timer_t* t) {
    DeviceManager* self = nullptr;
    std::memcpy(&self, us_timer_ext(t), sizeof(DeviceManager*));
    if (self) {
        self->in_timer_cb_ = true;
        self->armed_deadline_ms_ = INT64_MAX;
        int64_t now = now_ms();
        self->process_timers(now);
        self->in_timer_cb_ = false;
        self->arm_next_timer(now_ms());
    }
}

void DeviceManager::schedule_timer_at(int64_t deadline_ms) {
    if (!timer_ || in_timer_cb_) return;
    if (deadline_ms < armed_deadline_ms_) {
        armed_deadline_ms_ = deadline_ms;
        int64_t now = now_ms();
        int delay = static_cast<int>(std::clamp<int64_t>(deadline_ms - now, 1, 60000));
        us_timer_set(timer_, on_us_timer, delay, 0);
    }
}

void DeviceManager::arm_next_timer(int64_t now) {
    if (!timer_) return;
    int64_t next_dl = INT64_MAX;
    next_dl = std::min(next_dl, auth_timers_.min_deadline());
    next_dl = std::min(next_dl, device_heartbeat_timers_.min_deadline());
    next_dl = std::min(next_dl, stream_heartbeat_timers_.min_deadline());
    next_dl = std::min(next_dl, request_timers_.min_deadline());
    next_dl = std::min(next_dl, stream_open_timers_.min_deadline());
    next_dl = std::min(next_dl, pairing_session_timers_.min_deadline());
    for (auto* w : pairing_token_waiters_) {
        if (w->deadline_ms < next_dl) next_dl = w->deadline_ms;
    }

    if (next_dl == INT64_MAX) {
        armed_deadline_ms_ = INT64_MAX;
        return;
    }
    armed_deadline_ms_ = next_dl;
    int delay = static_cast<int>(std::clamp<int64_t>(next_dl - now, 1, 60000));
    us_timer_set(timer_, on_us_timer, delay, 0);
}

void DeviceManager::process_timers(int64_t now) {
    while (!auth_timers_.empty() && auth_timers_.head->deadline_ms <= now) {
        auto* sess = ptr_from_node(auth_timers_.head, &DeviceSession::auth_timer);
        auth_timers_.remove(&sess->auth_timer);
        if (!sess->authenticated && sess->ws) {
            fail_auth(sess, "authentication timeout");
        }
    }

    while (!device_heartbeat_timers_.empty() && device_heartbeat_timers_.head->deadline_ms <= now) {
        auto* sess = ptr_from_node(device_heartbeat_timers_.head, &DeviceSession::heartbeat_timer);
        device_heartbeat_timers_.remove(&sess->heartbeat_timer);
        if (sess->device && sess->device->ws_session.get() == sess) {
            close_device_session(sess->device, 1001, "heartbeat timeout");
        }
    }

    while (!stream_heartbeat_timers_.empty() && stream_heartbeat_timers_.head->deadline_ms <= now) {
        auto* stream = ptr_from_node(stream_heartbeat_timers_.head, &TcpStreamSession::heartbeat_timer);
        stream_heartbeat_timers_.remove(&stream->heartbeat_timer);
        if (stream->device) {
            drop_stream(stream->device, stream, 1001, "heartbeat timeout", true);
        }
    }

    while (!request_timers_.empty() && request_timers_.head->deadline_ms <= now) {
        auto* req = ptr_from_node(request_timers_.head, &PendingRequest::timeout_timer);
        request_timers_.remove(&req->timeout_timer);
        complete_pending(req, 504, json_error_body("device timeout"));
    }

    while (!stream_open_timers_.empty() && stream_open_timers_.head->deadline_ms <= now) {
        auto* sw = ptr_from_node(stream_open_timers_.head, &StreamWaiter::timeout_timer);
        stream_open_timers_.remove(&sw->timeout_timer);
        if (!sw->completed && sw->device) {
            sw->completed = true;
            auto& vec = sw->device->stream_pending;
            vec.erase(std::remove(vec.begin(), vec.end(), sw), vec.end());
            if (sw->res) {
                send_http_response(sw->res, 504, json_error_body("device timeout"), nullptr, true);
                sw->res = nullptr;
            }
            maybe_gc_device(sw->device);
            delete sw;
        }
    }

    while (!pairing_session_timers_.empty() && pairing_session_timers_.head->deadline_ms <= now) {
        auto* pw = ptr_from_node(pairing_session_timers_.head, &PairingSessionWaiter::timeout_timer);
        pairing_session_timers_.remove(&pw->timeout_timer);
        if (pw->device && pw->device->pairing_session_waiter == pw) {
            complete_pairing_session(pw->device, 504, json_error_body("device timeout"));
        }
    }

    for (size_t i = 0; i < pairing_token_waiters_.size();) {
        auto* w = pairing_token_waiters_[i];
        if (w->deadline_ms <= now) {
            pairing_token_waiters_[i] = pairing_token_waiters_.back();
            pairing_token_waiters_.pop_back();
            if (!w->completed && w->device) {
                w->completed = true;
                auto& dv = w->device->pairing_token_waiters;
                dv.erase(std::remove(dv.begin(), dv.end(), w), dv.end());
                if (w->res) {
                    send_http_response(w->res, 401, json_error_body("session token is invalid or expired"), nullptr, true);
                    w->res = nullptr;
                }
                maybe_gc_device(w->device);
                delete w;
            }
        } else {
            ++i;
        }
    }
}

DeviceRuntime* DeviceManager::find_device(std::string_view digest) {
    auto it = devices_.find(digest);
    return it == devices_.end() ? nullptr : it->second.get();
}

DeviceRuntime* DeviceManager::get_or_create_device(std::string_view digest) {
    auto it = devices_.find(digest);
    if (it != devices_.end()) return it->second.get();
    auto dev = std::make_unique<DeviceRuntime>();
    dev->digest.assign(digest);
    auto* ptr = dev.get();
    devices_.emplace(dev->digest, std::move(dev));
    return ptr;
}

void DeviceManager::maybe_gc_device(DeviceRuntime* dev) {
    if (!dev) return;
    if (dev->ws_session || !dev->pending.empty() || !dev->headroom.empty() ||
        !dev->stream_pending.empty() || !dev->streams.empty() ||
        dev->pairing_session_waiter || !dev->pairing_token_waiters.empty()) {
        return;
    }
    if (dev->pairing) {
        if (now_ms() < dev->pairing->expired_at_s * 1000) {
            return;
        }
        dev->pairing.reset();
    }
    devices_.erase(dev->digest);
}

void DeviceManager::validate_device_ws(DeviceRuntime* dev, int64_t now) {
    if (!dev || !dev->ws_session) return;
    if (now - dev->ws_session->last_seen_ms > kHeartbeatTimeoutMs) {
        close_device_session(dev, 1001, "heartbeat timeout");
    }
}

bool DeviceManager::is_device_connected(std::string_view digest) {
    auto* dev = find_device(digest);
    if (!dev) return false;
    validate_device_ws(dev, now_ms());
    return dev->ws_session != nullptr;
}

bool DeviceManager::is_device_online(std::string_view digest) {
    auto* dev = find_device(digest);
    if (!dev) return false;
    validate_device_ws(dev, now_ms());
    return dev->ws_session != nullptr && dev->ws_session->authenticated;
}

void DeviceManager::disconnect_device(std::string_view digest, int code, std::string_view reason) {
    auto* dev = find_device(digest);
    if (!dev) return;
    close_device_session(dev, code, reason);
    maybe_gc_device(dev);
}

void DeviceManager::on_device_ws_open(DeviceWS* ws, std::string_view digest) {
    auto* dev = get_or_create_device(digest);
    int64_t now = now_ms();
    validate_device_ws(dev, now);

    auto sess = std::make_unique<DeviceSession>();
    sess->device = dev;
    sess->ws = ws;
    sess->authenticated = false;
    sess->challenge.resize(kChallengeBytes);
    random_bytes(sess->challenge);
    sess->connected_at_ms = now;
    sess->last_seen_ms = now;

    ws->getUserData()->session = sess.get();
    ws->getUserData()->mgr = this;

    int64_t auth_dl = now + cfg_.auth_timeout_ms;
    int64_t hb_dl = now + kHeartbeatTimeoutMs + 1;
    auth_timers_.push_back(&sess->auth_timer, auth_dl);
    device_heartbeat_timers_.push_back(&sess->heartbeat_timer, hb_dl);
    schedule_timer_at(std::min(auth_dl, hb_dl));

    std::string challenge_b64 = bytes_to_base64url(sess->challenge);
    dev->ws_session = std::move(sess);
    connected_devices_count_++;
    LOG_INFO("[device] connected digest=" << digest);

    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "type", "authRequest");
    yyjson_mut_obj_add_strn(doc, root, "challenge", challenge_b64.data(), challenge_b64.size());
    size_t len = 0;
    char* json = yyjson_mut_write(doc, 0, &len);
    yyjson_mut_doc_free(doc);
    if (json) {
        ws->send(std::string_view(json, len), uWS::OpCode::TEXT);
        free(json);
    }
}

void DeviceManager::on_device_ws_message(DeviceWS* ws, std::string_view msg, uWS::OpCode op) {
    auto* sess = ws->getUserData()->session;
    if (!sess || !sess->device || sess->device->ws_session.get() != sess) {
        ws->getUserData()->session = nullptr;
        ws->end(1008, "stale connection");
        return;
    }
    DeviceRuntime* dev = sess->device;
    int64_t now = now_ms();
    if (now - sess->last_seen_ms > kHeartbeatTimeoutMs) {
        close_device_session(dev, 1001, "heartbeat timeout");
        maybe_gc_device(dev);
        return;
    }

    if (op != uWS::OpCode::TEXT) {
        return;
    }

    // Protocol heartbeat: text "ping" -> "pong"
    if (msg == "ping") {
        sess->last_seen_ms = now;
        device_heartbeat_timers_.push_back(&sess->heartbeat_timer, now + kHeartbeatTimeoutMs + 1);
        ws->send("pong", uWS::OpCode::TEXT);
        return;
    }

    auto parsed = parse_device_message(msg);
    if (parsed.type == ParsedDeviceMessage::Type::Invalid) {
        if (!sess->authenticated) {
            fail_auth(sess, "malformed auth response");
        }
        return;
    }

    if (parsed.type == ParsedDeviceMessage::Type::AuthResponse) {
        handle_auth_response(sess, parsed.auth_response);
        return;
    }

    if (!sess->authenticated) {
        fail_auth(sess, "not authenticated");
        return;
    }

    switch (parsed.type) {
        case ParsedDeviceMessage::Type::PairingSessionResponse:
            handle_pairing_session_response(dev, parsed.pairing_session_response);
            break;
        case ParsedDeviceMessage::Type::PairingSessionTokenResponse:
            handle_pairing_token_response(dev, parsed.pairing_token_response);
            break;
        case ParsedDeviceMessage::Type::TcpStreamOpenResponse:
            handle_tcp_stream_open_response(dev, parsed.tcp_stream_open_response);
            break;
        case ParsedDeviceMessage::Type::TcpStreamCloseResponse:
            break;
        case ParsedDeviceMessage::Type::TcpStreamAck:
            handle_tcp_stream_ack(dev, parsed.tcp_stream_ack);
            break;
        case ParsedDeviceMessage::Type::Response:
            handle_device_response(dev, parsed.response);
            break;
        default:
            break;
    }
}

void DeviceManager::on_device_ws_close(DeviceWS* ws) {
    auto* sess = ws->getUserData()->session;
    ws->getUserData()->session = nullptr;
    if (!sess || !sess->device) return;
    DeviceRuntime* dev = sess->device;
    if (dev->ws_session.get() != sess) return;

    auth_timers_.remove(&sess->auth_timer);
    device_heartbeat_timers_.remove(&sess->heartbeat_timer);
    bool was_authed = sess->authenticated;
    if (connected_devices_count_ > 0) connected_devices_count_--;
    if (was_authed && authenticated_devices_count_ > 0) authenticated_devices_count_--;
    LOG_INFO("[device] disconnected digest=" << dev->digest << " authenticated=" << (was_authed ? "true" : "false"));
    sess->ws = nullptr;
    dev->ws_session.reset();
    reject_all_pending(dev, "device offline");
    maybe_gc_device(dev);
}

void DeviceManager::fail_auth(DeviceSession* sess, std::string_view reason) {
    if (!sess || !sess->device) return;
    DeviceRuntime* dev = sess->device;
    if (dev->ws_session.get() != sess) {
        if (sess->ws) {
            sess->ws->getUserData()->session = nullptr;
            sess->ws->end(1008, reason);
            sess->ws = nullptr;
        }
        return;
    }

    LOG_WARN("[device] auth failed digest=" << dev->digest << " reason=\"" << reason << "\"");
    if (sess->ws) {
        yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
        yyjson_mut_val* root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
        yyjson_mut_obj_add_str(doc, root, "type", "authResult");
        yyjson_mut_obj_add_bool(doc, root, "success", false);
        yyjson_mut_obj_add_strn(doc, root, "reason", reason.data(), reason.size());
        size_t len = 0;
        char* json = yyjson_mut_write(doc, 0, &len);
        yyjson_mut_doc_free(doc);
        if (json) {
            sess->ws->send(std::string_view(json, len), uWS::OpCode::TEXT);
            free(json);
        }
    }
    close_device_session(dev, 1008, reason);
    maybe_gc_device(dev);
}

void DeviceManager::close_device_session(DeviceRuntime* dev, int code, std::string_view reason) {
    if (!dev) return;
    if (dev->ws_session) {
        auto sess = std::move(dev->ws_session);
        auth_timers_.remove(&sess->auth_timer);
        device_heartbeat_timers_.remove(&sess->heartbeat_timer);
        if (connected_devices_count_ > 0) connected_devices_count_--;
        if (sess->authenticated && authenticated_devices_count_ > 0) authenticated_devices_count_--;
        LOG_INFO("[device] closed digest=" << dev->digest << " code=" << code << " reason=\"" << reason << "\"");
        if (sess->ws) {
            DeviceWS* ws = sess->ws;
            ws->getUserData()->session = nullptr;
            sess->ws = nullptr;
            ws->end(code, reason);
        }
    }
    reject_all_pending(dev, "device offline");
}

void DeviceManager::handle_auth_response(DeviceSession* sess, const AuthResponseMsg& msg) {
    if (!sess->device || sess->device->ws_session.get() != sess) return;
    if (sess->authenticated) return;

    if (sess->challenge.empty()) {
        fail_auth(sess, "missing challenge");
        return;
    }

    auto verified = verify_device_auth(
        msg.device_public_key,
        msg.response,
        sess->challenge,
        sess->device->digest,
        msg.device_public_key_digest
    );
    if (!verified.ok) {
        fail_auth(sess, verified.reason);
        return;
    }

    sess->challenge.clear();
    sess->challenge.shrink_to_fit();
    sess->authenticated = true;
    sess->version = msg.version;
    auth_timers_.remove(&sess->auth_timer);
    authenticated_devices_count_++;
    LOG_INFO("[device] authenticated digest=" << sess->device->digest << " version=\"" << msg.version << "\"");

    if (sess->ws) {
        sess->ws->send(R"({"type":"authResult","success":true})", uWS::OpCode::TEXT);
    }
}

void DeviceManager::handle_proxy(
    HttpRes* res,
    std::string_view digest,
    std::string_view method,
    std::string_view path,
    std::vector<std::pair<std::string, std::string>> headers,
    std::string_view body_str,
    bool has_content_type,
    std::string_view content_type,
    std::string_view grant_secret
) {
    auto* dev = find_device(digest);
    if (dev) validate_device_ws(dev, now_ms());
    if (!dev || !dev->ws_session || !dev->ws_session->authenticated) {
        send_json_error(res, 503, "device offline");
        return;
    }

    if (path.empty() || path.size() > kMaxPathLen || path[0] != '/') {
        send_json_error(res, 400, "invalid path");
        return;
    }

    if (body_str.size() > kMaxBodyBytes) {
        send_json_error(res, 413, "body too large");
        return;
    }

    if ((has_content_type || !body_str.empty()) && !is_json_content_type(content_type)) {
        send_http_response(res, 415, unsupported_content_type_json(content_type, has_content_type), nullptr, false);
        return;
    }

    yyjson_doc* body_doc = nullptr;
    yyjson_val* body_root = nullptr;
    if (!body_str.empty()) {
        body_doc = yyjson_read(body_str.data(), body_str.size(), 0);
        if (!body_doc || !yyjson_is_obj(yyjson_doc_get_root(body_doc))) {
            if (body_doc) yyjson_doc_free(body_doc);
            send_json_error(res, 400, "body must be a JSON object");
            return;
        }
        body_root = yyjson_doc_get_root(body_doc);
    }

    // Check file_cache before forwarding to device
    if (!dev->ws_session->version.empty()) {
        size_t q = path.find('?');
        std::string_view path_name = (q == std::string_view::npos) ? path : path.substr(0, q);
        if (method == "GET" && path_name == "/openapi.json") {
            if (auto cached = store_.get_cached_file(dev->ws_session->version, "openapi.json")) {
                if (body_doc) yyjson_doc_free(body_doc);
                send_http_response(res, 200, *cached, nullptr, false);
                return;
            }
        }
        if (method == "POST" && path_name == "/mcp" && body_root) {
            yyjson_val* jr = yyjson_obj_get(body_root, "jsonrpc");
            yyjson_val* m = yyjson_obj_get(body_root, "method");
            yyjson_val* id = yyjson_obj_get(body_root, "id");
            if (yyjson_is_str(jr) && std::string_view(yyjson_get_str(jr), yyjson_get_len(jr)) == "2.0" &&
                yyjson_is_str(m) && std::string_view(yyjson_get_str(m), yyjson_get_len(m)) == "tools/list" &&
                (yyjson_is_str(id) || yyjson_is_num(id))) {
                if (auto cached = store_.get_cached_file(dev->ws_session->version, "mcp.json")) {
                    yyjson_doc* res_doc = yyjson_read(cached->data(), cached->size(), 0);
                    if (res_doc) {
                        yyjson_mut_doc* mdoc = yyjson_mut_doc_new(nullptr);
                        yyjson_mut_val* mroot = yyjson_mut_obj(mdoc);
                        yyjson_mut_doc_set_root(mdoc, mroot);
                        yyjson_mut_obj_add_str(mdoc, mroot, "jsonrpc", "2.0");
                        yyjson_mut_obj_add_val(mdoc, mroot, "result", yyjson_val_mut_copy(mdoc, yyjson_doc_get_root(res_doc)));
                        yyjson_mut_obj_add_val(mdoc, mroot, "id", yyjson_val_mut_copy(mdoc, id));
                        size_t len = 0;
                        char* json = yyjson_mut_write(mdoc, 0, &len);
                        yyjson_mut_doc_free(mdoc);
                        yyjson_doc_free(res_doc);
                        if (json) {
                            std::string out(json, len);
                            free(json);
                            if (body_doc) yyjson_doc_free(body_doc);
                            send_http_response(res, 200, out, nullptr, false);
                            return;
                        }
                    }
                }
            }
        }
    }

    // Build RequestMessage JSON
    std::string request_id = next_request_id(dev);
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "type", "request");
    yyjson_mut_obj_add_strn(doc, root, "requestId", request_id.data(), request_id.size());
    yyjson_mut_obj_add_strn(doc, root, "method", method.data(), method.size());
    yyjson_mut_obj_add_strn(doc, root, "path", path.data(), path.size());
    yyjson_mut_val* hdrs_obj = yyjson_mut_obj(doc);
    for (const auto& [k, v] : headers) {
        yyjson_mut_obj_add(hdrs_obj,
                           yyjson_mut_strncpy(doc, k.data(), k.size()),
                           yyjson_mut_strncpy(doc, v.data(), v.size()));
    }
    yyjson_mut_obj_add_val(doc, root, "headers", hdrs_obj);
    if (body_root) {
        yyjson_mut_obj_add_val(doc, root, "body", yyjson_val_mut_copy(doc, body_root));
    } else {
        yyjson_mut_obj_add_null(doc, root, "body");
    }
    if (!grant_secret.empty()) {
        yyjson_mut_obj_add_strn(doc, root, "grantSecret", grant_secret.data(), grant_secret.size());
    }
    if (body_doc) yyjson_doc_free(body_doc);

    size_t len = 0;
    char* json = yyjson_mut_write(doc, 0, &len);
    yyjson_mut_doc_free(doc);
    if (!json) {
        send_json_error(res, 500, "serialization failed");
        return;
    }
    std::string serialized(json, len);
    free(json);

    dev->headroom.push_back(HeadroomRequest{
        std::move(request_id),
        std::move(serialized),
        std::string(grant_secret),
        res,
        false,
    });
    HeadroomRequest* qref = &dev->headroom.back();
    std::string req_id_copy = qref->request_id;
    std::string digest_copy = dev->digest;
    res->onAborted([this, digest_copy, req_id_copy]() {
        auto* d = find_device(digest_copy);
        if (!d) return;
        for (auto& h : d->headroom) {
            if (h.request_id == req_id_copy) {
                h.res = nullptr;
                h.aborted = true;
                return;
            }
        }
        for (auto* p : d->pending) {
            if (p->request_id == req_id_copy) {
                p->res = nullptr;
                return;
            }
        }
    });

    drain_headroom(dev);
}

void DeviceManager::drain_headroom(DeviceRuntime* dev) {
    if (!dev->ws_session || !dev->ws_session->authenticated || !dev->ws_session->ws) {
        reject_all_pending(dev, "device offline");
        return;
    }
    int64_t now = now_ms();
    while (dev->pending.size() < kMaxPendingRequests && !dev->headroom.empty()) {
        auto item = std::move(dev->headroom.front());
        dev->headroom.pop_front();
        if (item.aborted) continue;

        auto* req = new PendingRequest();
        req->device = dev;
        req->request_id = std::move(item.request_id);
        req->grant_secret = std::move(item.grant_secret);
        req->res = item.res;

        int64_t dl = now + cfg_.request_timeout_ms;
        request_timers_.push_back(&req->timeout_timer, dl);
        schedule_timer_at(dl);
        dev->pending.push_back(req);

        auto status = dev->ws_session->ws->send(item.serialized_message, uWS::OpCode::TEXT);
        if (status == DeviceWS::DROPPED) {
            request_timers_.remove(&req->timeout_timer);
            dev->pending.pop_back();
            if (req->res) {
                send_http_response(req->res, 503, json_error_body("device offline"), nullptr, true);
            }
            delete req;
            reject_all_pending(dev, "device offline");
            return;
        }
    }
}

void DeviceManager::handle_device_response(DeviceRuntime* dev, const ResponseMsg& msg) {
    PendingRequest* found = nullptr;
    for (auto* p : dev->pending) {
        if (p->request_id == msg.request_id) {
            found = p;
            break;
        }
    }
    if (!found) return;

    // Validate response headers
    std::vector<std::pair<std::string, std::string>> headers;
    headers.reserve(msg.headers.size());
    std::string_view content_type;
    bool has_ct = false;
    for (const auto& [k, v] : msg.headers) {
        if (!is_valid_header_name(k) || !is_valid_header_value(v)) {
            complete_pending(found, 502, json_error_body("invalid device response headers"));
            return;
        }
        if (iequals(k, "content-length") || iequals(k, "transfer-encoding")) {
            continue;
        }
        if (iequals(k, "content-type")) {
            content_type = v;
            has_ct = true;
        }
        headers.emplace_back(k, v);
    }

    std::string body_text;
    if (msg.body != nullptr) {
        if (msg.status == 204 || msg.status == 205 || msg.status == 304) {
            complete_pending(found, 502, json_error_body("invalid device response body"));
            return;
        }
        if (!has_ct || !is_json_content_type(content_type)) {
            complete_pending(found, 502, unsupported_content_type_json(content_type, has_ct));
            return;
        }

        // Check if we need to inject websocket token
        bool injected = false;
        if (msg.add_websocket_token &&
            is_tcp_stream_path(msg.add_websocket_token->websocket_path) &&
            !found->grant_secret.empty() &&
            ws_key_.valid()) {
            int64_t expire_at = (now_ms() / 1000) + cfg_.websocket_token_ttl_s;
            auto token = seal_websocket_token(ws_key_, WebsocketTokenPayload{
                expire_at,
                dev->digest,
                msg.add_websocket_token->websocket_path,
                found->grant_secret,
            });
            if (token) {
                yyjson_mut_doc* mdoc = yyjson_mut_doc_new(nullptr);
                yyjson_mut_val* mbody = yyjson_val_mut_copy(mdoc, msg.body);
                yyjson_mut_doc_set_root(mdoc, mbody);
                inject_websocket_token(mdoc, mbody, msg.add_websocket_token->json_pathes, *token);
                if (!msg.add_websocket_token->expired_at_json_pathes.empty()) {
                    inject_expired_at(mdoc, mbody, msg.add_websocket_token->expired_at_json_pathes, expire_at);
                }
                size_t len = 0;
                char* raw = yyjson_mut_write(mdoc, 0, &len);
                yyjson_mut_doc_free(mdoc);
                if (raw) {
                    body_text.assign(raw, len);
                    free(raw);
                    injected = true;
                }
            }
        }
        if (!injected) {
            size_t len = 0;
            char* raw = yyjson_val_write(msg.body, 0, &len);
            if (!raw) {
                complete_pending(found, 502, json_error_body("invalid device response body"));
                return;
            }
            body_text.assign(raw, len);
            free(raw);
        }
        if (body_text.size() > kMaxResponseBodyBytes) {
            complete_pending(found, 502, json_error_body("invalid device response body"));
            return;
        }
    }

    complete_pending(found, msg.status, body_text, &headers);
}

void DeviceManager::complete_pending(
    PendingRequest* req,
    int status,
    std::string_view body,
    const std::vector<std::pair<std::string, std::string>>* headers
) {
    if (!req || req->completed) return;
    req->completed = true;
    request_timers_.remove(&req->timeout_timer);
    DeviceRuntime* dev = req->device;
    if (dev) {
        auto& vec = dev->pending;
        vec.erase(std::remove(vec.begin(), vec.end(), req), vec.end());
    }
    if (req->res) {
        send_http_response(req->res, status, body, headers, true);
        req->res = nullptr;
    }
    delete req;
    if (dev) {
        drain_headroom(dev);
        maybe_gc_device(dev);
    }
}

void DeviceManager::reject_all_pending(DeviceRuntime* dev, std::string_view reason) {
    std::string err = json_error_body(reason);

    auto pending_copy = std::move(dev->pending);
    dev->pending.clear();
    for (auto* req : pending_copy) {
        req->completed = true;
        request_timers_.remove(&req->timeout_timer);
        if (req->res) {
            send_http_response(req->res, 503, err, nullptr, true);
            req->res = nullptr;
        }
        delete req;
    }

    while (!dev->headroom.empty()) {
        auto h = std::move(dev->headroom.front());
        dev->headroom.pop_front();
        if (!h.aborted && h.res) {
            send_http_response(h.res, 503, err, nullptr, true);
        }
    }

    auto sw_copy = std::move(dev->stream_pending);
    dev->stream_pending.clear();
    for (auto* sw : sw_copy) {
        sw->completed = true;
        stream_open_timers_.remove(&sw->timeout_timer);
        if (sw->res) {
            send_http_response(sw->res, 503, err, nullptr, true);
            sw->res = nullptr;
        }
        delete sw;
    }

    auto streams_copy = std::move(dev->streams);
    dev->streams.clear();
    for (auto& st : streams_copy) {
        stream_heartbeat_timers_.remove(&st->heartbeat_timer);
        if (active_streams_count_ > 0) active_streams_count_--;
        LOG_INFO("[stream] closed digest=" << dev->digest << " channel=" << st->channel_id << " reason=\"" << reason << "\"");
        if (st->ws) {
            TcpStreamWS* ws = st->ws;
            ws->getUserData()->stream = nullptr;
            st->ws = nullptr;
            ws->end(1001, reason);
        }
    }

    complete_pairing_session(dev, 503, err);
    finish_pairing_token_waiters(dev, 503, err);
}

void DeviceManager::handle_pairing_session(HttpRes* res, std::string_view digest, std::string_view name) {
    auto* dev = find_device(digest);
    if (dev) validate_device_ws(dev, now_ms());
    if (!dev || !dev->ws_session || !dev->ws_session->authenticated) {
        send_json_error(res, 503, "device offline");
        return;
    }
    if (dev->pairing_session_waiter) {
        send_json_error(res, 409, "a pairing session is already active");
        return;
    }

    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "type", "pairingSessionRequest");
    yyjson_mut_obj_add_strn(doc, root, "name", name.data(), name.size());
    size_t len = 0;
    char* json = yyjson_mut_write(doc, 0, &len);
    yyjson_mut_doc_free(doc);
    if (!json) {
        send_json_error(res, 500, "serialization failed");
        return;
    }

    auto* waiter = new PairingSessionWaiter();
    waiter->device = dev;
    waiter->res = res;
    int64_t dl = now_ms() + cfg_.request_timeout_ms;
    pairing_session_timers_.push_back(&waiter->timeout_timer, dl);
    schedule_timer_at(dl);
    dev->pairing_session_waiter = waiter;

    res->onAborted([waiter]() {
        waiter->res = nullptr;
    });

    auto status = dev->ws_session->ws->send(std::string_view(json, len), uWS::OpCode::TEXT);
    free(json);
    LOG_INFO("[pairing] session requested digest=" << dev->digest << " name=\"" << name << "\"");
    if (status == DeviceWS::DROPPED) {
        complete_pairing_session(dev, 503, json_error_body("device offline"));
    }
}

PairingState* DeviceManager::live_pairing(DeviceRuntime* dev, int64_t now) {
    if (!dev || !dev->pairing) return nullptr;
    if (static_cast<double>(now) / 1000.0 >= static_cast<double>(dev->pairing->expired_at_s)) {
        dev->pairing.reset();
        finish_pairing_token_waiters(dev, 401, json_error_body("session token is invalid or expired"));
        return nullptr;
    }
    return &(*dev->pairing);
}

void DeviceManager::handle_pairing_token(HttpRes* res, std::string_view digest, std::string_view session_token) {
    auto* dev = find_device(digest);
    int64_t now = now_ms();
    if (dev) validate_device_ws(dev, now);

    PairingState* pairing = live_pairing(dev, now);
    if (!pairing || pairing->session_token != session_token) {
        send_json_error(res, 401, "session token is invalid or expired");
        return;
    }

    if (!pairing->grant_secret.empty()) {
        auto rt = seal_routing_token(routing_key_, RoutingTokenPayload{dev->digest, pairing->grant_secret});
        if (!rt) {
            send_json_error(res, 500, "failed to seal routing token");
            return;
        }
        yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
        yyjson_mut_val* root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
        yyjson_mut_obj_add_strn(doc, root, "routingToken", rt->data(), rt->size());
        size_t len = 0;
        char* json = yyjson_mut_write(doc, 0, &len);
        yyjson_mut_doc_free(doc);
        send_http_response(res, 200, std::string_view(json, len), nullptr, false);
        free(json);
        return;
    }

    if (!dev->ws_session || !dev->ws_session->authenticated) {
        send_json_error(res, 503, "device offline");
        return;
    }
    if (dev->pairing_token_waiters.size() >= kMaxPairingWaiters) {
        send_json_error(res, 409, "a pairing session is already active");
        return;
    }

    auto* waiter = new PairingTokenWaiter();
    waiter->device = dev;
    waiter->res = res;
    waiter->deadline_ms = std::max<int64_t>(now, pairing->expired_at_s * 1000);
    dev->pairing_token_waiters.push_back(waiter);
    pairing_token_waiters_.push_back(waiter);
    schedule_timer_at(waiter->deadline_ms);

    res->onAborted([waiter]() {
        waiter->res = nullptr;
    });
}

void DeviceManager::handle_pairing_session_response(DeviceRuntime* dev, const PairingSessionResponseMsg& msg) {
    if (msg.success) {
        bool same_token = dev->pairing && dev->pairing->session_token == msg.session_token;
        std::string prev_grant = same_token ? dev->pairing->grant_secret : std::string{};
        dev->pairing = PairingState{msg.session_token, msg.expired_at, std::move(prev_grant)};
        if (!same_token) {
            finish_pairing_token_waiters(dev, 401, json_error_body("session token is invalid or expired"));
        }
        yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
        yyjson_mut_val* root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
        yyjson_mut_obj_add_strn(doc, root, "sessionToken", msg.session_token.data(), msg.session_token.size());
        yyjson_mut_obj_add_sint(doc, root, "expiredAt", msg.expired_at);
        size_t len = 0;
        char* json = yyjson_mut_write(doc, 0, &len);
        yyjson_mut_doc_free(doc);
        complete_pairing_session(dev, 200, std::string_view(json, len));
        free(json);
        return;
    }
    complete_pairing_session(dev, pairing_error_status(msg.reason), json_error_body(msg.reason));
}

void DeviceManager::handle_pairing_token_response(DeviceRuntime* dev, const PairingSessionTokenResponseMsg& msg) {
    PairingState* pairing = live_pairing(dev, now_ms());
    if (!pairing) return;
    if (msg.success) {
        pairing->grant_secret = msg.grant_secret;
        auto rt = seal_routing_token(routing_key_, RoutingTokenPayload{dev->digest, msg.grant_secret});
        if (!rt) {
            finish_pairing_token_waiters(dev, 502, json_error_body("invalid device response"));
            return;
        }
        yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
        yyjson_mut_val* root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
        yyjson_mut_obj_add_strn(doc, root, "routingToken", rt->data(), rt->size());
        size_t len = 0;
        char* json = yyjson_mut_write(doc, 0, &len);
        yyjson_mut_doc_free(doc);
        LOG_INFO("[pairing] token granted digest=" << dev->digest);
        finish_pairing_token_waiters(dev, 200, std::string_view(json, len));
        free(json);
        return;
    }
    finish_pairing_token_waiters(dev, pairing_error_status(msg.reason), json_error_body(msg.reason));
}

void DeviceManager::complete_pairing_session(DeviceRuntime* dev, int status, std::string_view body) {
    auto* pw = dev->pairing_session_waiter;
    if (!pw || pw->completed) return;
    pw->completed = true;
    dev->pairing_session_waiter = nullptr;
    pairing_session_timers_.remove(&pw->timeout_timer);
    if (pw->res) {
        send_http_response(pw->res, status, body, nullptr, true);
        pw->res = nullptr;
    }
    delete pw;
}

void DeviceManager::finish_pairing_token_waiters(DeviceRuntime* dev, int status, std::string_view body) {
    auto waiters = std::move(dev->pairing_token_waiters);
    dev->pairing_token_waiters.clear();
    for (auto* w : waiters) {
        w->completed = true;
        auto it = std::find(pairing_token_waiters_.begin(), pairing_token_waiters_.end(), w);
        if (it != pairing_token_waiters_.end()) {
            *it = pairing_token_waiters_.back();
            pairing_token_waiters_.pop_back();
        }
        if (w->res) {
            send_http_response(w->res, status, body, nullptr, true);
            w->res = nullptr;
        }
        delete w;
    }
}

void DeviceManager::handle_tcp_stream_upgrade(
    HttpRes* res,
    us_socket_context_t* ws_ctx,
    std::string_view digest,
    std::string_view service,
    std::string_view grant_secret,
    std::string_view sec_ws_key,
    std::string_view sec_ws_protocol,
    std::string_view sec_ws_extensions
) {
    if (service != "dap" && service != "rfc2217") {
        send_json_error(res, 400, "invalid service");
        return;
    }
    if (grant_secret.empty()) {
        send_json_error(res, 400, "invalid grantSecret");
        return;
    }
    auto* dev = find_device(digest);
    if (dev) validate_device_ws(dev, now_ms());
    if (!dev || !dev->ws_session || !dev->ws_session->authenticated) {
        send_json_error(res, 503, "device offline");
        return;
    }
    if (find_stream_by_service(dev, service) || stream_open_in_flight(dev, service)) {
        send_json_error(res, 409, "stream already open");
        return;
    }

    std::string req_id = next_request_id(dev);
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "type", "tcpStreamOpenRequest");
    yyjson_mut_obj_add_strn(doc, root, "requestId", req_id.data(), req_id.size());
    yyjson_mut_obj_add_strn(doc, root, "service", service.data(), service.size());
    yyjson_mut_obj_add_strn(doc, root, "grantSecret", grant_secret.data(), grant_secret.size());
    size_t len = 0;
    char* json = yyjson_mut_write(doc, 0, &len);
    yyjson_mut_doc_free(doc);
    if (!json) {
        send_json_error(res, 500, "serialization failed");
        return;
    }

    auto* sw = new StreamWaiter();
    sw->device = dev;
    sw->request_id = std::move(req_id);
    sw->service.assign(service);
    sw->grant_secret.assign(grant_secret);
    sw->res = res;
    sw->ws_ctx = ws_ctx;
    sw->sec_ws_key.assign(sec_ws_key);
    sw->sec_ws_protocol.assign(sec_ws_protocol);
    sw->sec_ws_extensions.assign(sec_ws_extensions);

    int64_t dl = now_ms() + cfg_.request_timeout_ms;
    stream_open_timers_.push_back(&sw->timeout_timer, dl);
    schedule_timer_at(dl);
    dev->stream_pending.push_back(sw);

    res->onAborted([sw]() {
        sw->res = nullptr;
    });

    auto status = dev->ws_session->ws->send(std::string_view(json, len), uWS::OpCode::TEXT);
    free(json);
    if (status == DeviceWS::DROPPED) {
        sw->completed = true;
        stream_open_timers_.remove(&sw->timeout_timer);
        dev->stream_pending.erase(std::remove(dev->stream_pending.begin(), dev->stream_pending.end(), sw),
                                  dev->stream_pending.end());
        if (sw->res) {
            send_json_error(sw->res, 503, "device offline");
            sw->res = nullptr;
        }
        delete sw;
    }
}

void DeviceManager::handle_tcp_stream_open_response(DeviceRuntime* dev, const TcpStreamOpenResponseMsg& msg) {
    StreamWaiter* sw = nullptr;
    for (auto* item : dev->stream_pending) {
        if (item->request_id == msg.request_id) {
            sw = item;
            break;
        }
    }
    if (!sw) return;

    sw->completed = true;
    stream_open_timers_.remove(&sw->timeout_timer);
    dev->stream_pending.erase(std::remove(dev->stream_pending.begin(), dev->stream_pending.end(), sw),
                              dev->stream_pending.end());

    if (!msg.success) {
        LOG_WARN("[stream] open failed digest=" << dev->digest << " service=" << sw->service << " reason=\"" << msg.reason << "\"");
        if (sw->res) {
            int status = (msg.reason == "device offline") ? 503 :
                         (msg.reason == "device timeout") ? 504 : 502;
            send_http_response(sw->res, status, json_error_body(msg.reason), nullptr, true);
            sw->res = nullptr;
        }
        delete sw;
        return;
    }

    if (!sw->res) {
        // HTTP client aborted before upgrade completed -> close channel on device
        send_tcp_stream_close(dev, msg.channel_id, sw->grant_secret);
        delete sw;
        return;
    }

    int64_t now = now_ms();
    auto stream = std::make_unique<TcpStreamSession>();
    stream->device = dev;
    stream->channel_id = msg.channel_id;
    stream->service = std::move(sw->service);
    stream->grant_secret = std::move(sw->grant_secret);
    stream->byte_credits = msg.byte_credits;
    stream->capacity = msg.byte_credits;
    stream->connected_at_ms = now;
    stream->last_seen_ms = now;

    TcpStreamSession* stream_ptr = stream.get();
    dev->streams.push_back(std::move(stream));
    active_streams_count_++;
    LOG_INFO("[stream] opened digest=" << dev->digest << " service=" << stream_ptr->service
             << " channel=" << stream_ptr->channel_id << " credits=" << stream_ptr->byte_credits);
    int64_t hb_dl = now + kHeartbeatTimeoutMs + 1;
    stream_heartbeat_timers_.push_back(&stream_ptr->heartbeat_timer, hb_dl);
    schedule_timer_at(hb_dl);

    HttpRes* res = sw->res;
    sw->res = nullptr;
    TcpStreamWsData user_data{stream_ptr, this};
    res->cork([&]() {
        res->template upgrade<TcpStreamWsData>(
            std::move(user_data),
            sw->sec_ws_key,
            sw->sec_ws_protocol,
            sw->sec_ws_extensions,
            sw->ws_ctx
        );
    });
    delete sw;
}

void DeviceManager::on_tcp_stream_ws_open(TcpStreamWS* ws) {
    auto* stream = ws->getUserData()->stream;
    if (stream) {
        stream->ws = ws;
    }
}

void DeviceManager::on_tcp_stream_ws_message(TcpStreamWS* ws, std::string_view msg, uWS::OpCode op) {
    auto* stream = ws->getUserData()->stream;
    if (!stream || !stream->device) {
        ws->getUserData()->stream = nullptr;
        ws->end(1011, "stream closed");
        return;
    }
    DeviceRuntime* dev = stream->device;
    int64_t now = now_ms();
    if (now - stream->last_seen_ms > kHeartbeatTimeoutMs) {
        drop_stream(dev, stream, 1001, "heartbeat timeout", true);
        return;
    }

    if (op == uWS::OpCode::TEXT) {
        if (msg == "ping" || msg == "pong") {
            if (msg == "ping") {
                stream->last_seen_ms = now;
                stream_heartbeat_timers_.push_back(&stream->heartbeat_timer, now + kHeartbeatTimeoutMs + 1);
            }
            return;
        }
        drop_stream(dev, stream, 1003, "binary frames only", true);
        return;
    }
}

void DeviceManager::on_device_binary_fragment(
    DeviceWS* ws,
    std::string_view chunk,
    unsigned int remaining_bytes,
    bool fin
) {
    auto* sess = ws->getUserData()->session;
    if (!sess || !sess->device || sess->device->ws_session.get() != sess) {
        ws->getUserData()->session = nullptr;
        ws->end(1008, "stale connection");
        return;
    }
    DeviceRuntime* dev = sess->device;
    int64_t now = now_ms();
    if (now - sess->last_seen_ms > kHeartbeatTimeoutMs) {
        close_device_session(dev, 1001, "heartbeat timeout");
        maybe_gc_device(dev);
        return;
    }
    if (!sess->authenticated) {
        fail_auth(sess, "expected text frame");
        return;
    }

    if (!sess->in_binary_frame) {
        if (chunk.empty()) {
            return;
        }
        sess->binary_channel_id = static_cast<uint8_t>(chunk[0]);
        sess->in_binary_frame = true;
        chunk.remove_prefix(1);
    }
    int channel_id = sess->binary_channel_id;
    if (remaining_bytes == 0 && fin) {
        sess->in_binary_frame = false;
        sess->binary_channel_id = -1;
    }
    if (chunk.empty() || channel_id < 0) return;

    TcpStreamSession* target = find_stream_by_channel(dev, channel_id);
    if (!target || !target->ws) return;

    size_t off = 0;
    while (off < chunk.size()) {
        size_t n = std::min(kStreamChunkBytes, chunk.size() - off);
        auto st = target->ws->send(chunk.substr(off, n), uWS::OpCode::BINARY);
        if (st == TcpStreamWS::DROPPED) {
            drop_stream(dev, target, 1011, "forward failed", true);
            return;
        }
        off += n;
    }
}

void DeviceManager::on_tcp_stream_binary_fragment(
    TcpStreamWS* ws,
    std::string_view chunk,
    unsigned int remaining_bytes,
    bool fin
) {
    auto* stream = ws->getUserData()->stream;
    if (!stream || !stream->device) {
        ws->getUserData()->stream = nullptr;
        ws->end(1011, "stream closed");
        return;
    }
    DeviceRuntime* dev = stream->device;
    int64_t now = now_ms();
    if (now - stream->last_seen_ms > kHeartbeatTimeoutMs) {
        drop_stream(dev, stream, 1001, "heartbeat timeout", true);
        return;
    }
    if (!dev->ws_session || !dev->ws_session->authenticated || !dev->ws_session->ws) {
        drop_stream(dev, stream, 1011, "device offline", false);
        return;
    }

    size_t buffered = stream->pending.size() - stream->pending_offset;
    if (!stream->in_binary_frame) {
        if (buffered + chunk.size() + static_cast<size_t>(remaining_bytes) > kTcpStreamClientBufferMax) {
            drop_stream(dev, stream, 1009, "stream buffer full", true);
            return;
        }
        stream->in_binary_frame = true;
    } else if (buffered + chunk.size() > kTcpStreamClientBufferMax) {
        drop_stream(dev, stream, 1009, "stream buffer full", true);
        return;
    }

    if (remaining_bytes == 0 && fin) {
        stream->in_binary_frame = false;
    }
    if (chunk.empty()) return;

    size_t sent_direct = 0;
    if (buffered == 0 && stream->byte_credits > 0) {
        size_t total_send = std::min(static_cast<size_t>(stream->byte_credits), chunk.size());
        char stack_buf[kStreamChunkBytes + 1];
        size_t off = 0;
        while (off < total_send) {
            size_t n = std::min(kStreamChunkBytes, total_send - off);
            stack_buf[0] = static_cast<char>(static_cast<uint8_t>(stream->channel_id));
            std::memcpy(stack_buf + 1, chunk.data() + off, n);
            if (dev->ws_session->ws->send(std::string_view(stack_buf, n + 1), uWS::OpCode::BINARY) == DeviceWS::DROPPED) {
                drop_stream(dev, stream, 1011, "device offline", false);
                return;
            }
            off += n;
        }
        stream->byte_credits -= static_cast<int>(total_send);
        sent_direct = total_send;
    }

    if (sent_direct < chunk.size()) {
        if (stream->pending_offset == stream->pending.size()) {
            stream->pending.clear();
            stream->pending_offset = 0;
        } else if (stream->pending_offset > 65536) {
            stream->pending.erase(0, stream->pending_offset);
            stream->pending_offset = 0;
        }
        stream->pending.append(chunk.data() + sent_direct, chunk.size() - sent_direct);
        flush_stream(dev, stream);
    }
}

void DeviceManager::on_tcp_stream_ws_close(TcpStreamWS* ws) {
    auto* stream = ws->getUserData()->stream;
    ws->getUserData()->stream = nullptr;
    if (!stream || !stream->device) return;
    DeviceRuntime* dev = stream->device;
    int channel_id = stream->channel_id;
    std::string grant_secret = stream->grant_secret;

    stream_heartbeat_timers_.remove(&stream->heartbeat_timer);
    if (active_streams_count_ > 0) active_streams_count_--;
    LOG_INFO("[stream] closed digest=" << dev->digest << " channel=" << channel_id << " reason=\"client closed\"");
    stream->ws = nullptr;
    auto& vec = dev->streams;
    vec.erase(std::remove_if(vec.begin(), vec.end(),
                             [stream](const auto& p) { return p.get() == stream; }),
              vec.end());
    send_tcp_stream_close(dev, channel_id, grant_secret);
    maybe_gc_device(dev);
}

void DeviceManager::handle_tcp_stream_ack(DeviceRuntime* dev, const TcpStreamAckMsg& msg) {
    TcpStreamSession* stream = find_stream_by_channel(dev, msg.channel_id);
    if (!stream) return;
    if (stream->byte_credits + msg.length > stream->capacity) {
        drop_stream(dev, stream, 1009, "stream credit exceeded", true);
        return;
    }
    stream->byte_credits += msg.length;
    flush_stream(dev, stream);
}

bool DeviceManager::flush_stream(DeviceRuntime* dev, TcpStreamSession* stream) {
    if (!dev->ws_session || !dev->ws_session->authenticated || !dev->ws_session->ws) {
        drop_stream(dev, stream, 1011, "device offline", false);
        return false;
    }
    while (stream->byte_credits > 0 && stream->pending_offset < stream->pending.size()) {
        size_t avail = stream->pending.size() - stream->pending_offset;
        size_t cap = std::min(static_cast<size_t>(stream->byte_credits), avail);
        size_t n = std::min(kStreamChunkBytes, cap);

        char stack_buf[kStreamChunkBytes + 1];
        stack_buf[0] = static_cast<char>(static_cast<uint8_t>(stream->channel_id));
        std::memcpy(stack_buf + 1, stream->pending.data() + stream->pending_offset, n);
        if (dev->ws_session->ws->send(std::string_view(stack_buf, n + 1), uWS::OpCode::BINARY) == DeviceWS::DROPPED) {
            drop_stream(dev, stream, 1011, "device offline", false);
            return false;
        }
        stream->byte_credits -= static_cast<int>(n);
        stream->pending_offset += n;
    }
    if (stream->pending_offset == stream->pending.size()) {
        stream->pending.clear();
        stream->pending_offset = 0;
        if (stream->pending.capacity() > 65536) {
            stream->pending.shrink_to_fit();
        }
    }
    return true;
}

void DeviceManager::drop_stream(
    DeviceRuntime* dev,
    TcpStreamSession* stream,
    int code,
    std::string_view reason,
    bool send_close_to_device
) {
    if (!dev || !stream) return;
    int channel_id = stream->channel_id;
    std::string grant_secret = stream->grant_secret;
    TcpStreamWS* ws = stream->ws;

    stream_heartbeat_timers_.remove(&stream->heartbeat_timer);
    if (active_streams_count_ > 0) active_streams_count_--;
    LOG_INFO("[stream] dropped digest=" << dev->digest << " channel=" << channel_id
             << " code=" << code << " reason=\"" << reason << "\"");
    if (ws) {
        ws->getUserData()->stream = nullptr;
        stream->ws = nullptr;
    }
    auto& vec = dev->streams;
    vec.erase(std::remove_if(vec.begin(), vec.end(),
                             [stream](const auto& p) { return p.get() == stream; }),
              vec.end());

    if (ws) {
        ws->end(code, reason);
    }
    if (send_close_to_device) {
        send_tcp_stream_close(dev, channel_id, grant_secret);
    }
    maybe_gc_device(dev);
}

void DeviceManager::send_tcp_stream_close(DeviceRuntime* dev, int channel_id, std::string_view grant_secret) {
    if (!dev || !dev->ws_session || !dev->ws_session->authenticated || !dev->ws_session->ws) return;
    std::string req_id = next_request_id(dev);
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "type", "tcpStreamCloseRequest");
    yyjson_mut_obj_add_strn(doc, root, "requestId", req_id.data(), req_id.size());
    yyjson_mut_obj_add_sint(doc, root, "channelId", channel_id);
    yyjson_mut_obj_add_strn(doc, root, "grantSecret", grant_secret.data(), grant_secret.size());
    size_t len = 0;
    char* json = yyjson_mut_write(doc, 0, &len);
    yyjson_mut_doc_free(doc);
    if (json) {
        dev->ws_session->ws->send(std::string_view(json, len), uWS::OpCode::TEXT);
        free(json);
    }
}

TcpStreamSession* DeviceManager::find_stream_by_service(DeviceRuntime* dev, std::string_view service) {
    for (const auto& s : dev->streams) {
        if (s->service == service) return s.get();
    }
    return nullptr;
}

TcpStreamSession* DeviceManager::find_stream_by_channel(DeviceRuntime* dev, int channel_id) {
    for (const auto& s : dev->streams) {
        if (s->channel_id == channel_id) return s.get();
    }
    return nullptr;
}

bool DeviceManager::stream_open_in_flight(DeviceRuntime* dev, std::string_view service) {
    for (const auto* sw : dev->stream_pending) {
        if (sw->service == service) return true;
    }
    return false;
}

std::string DeviceManager::next_request_id(DeviceRuntime* dev) {
    dev->seq += 1;
    uint8_t rnd[8];
    random_bytes(rnd);
    char hex_buf[16];
    int n = std::snprintf(hex_buf, sizeof(hex_buf), "%x", dev->seq);
    std::string out(hex_buf, static_cast<size_t>(n));
    out.push_back('-');
    out += bytes_to_base64url(rnd);
    return out;
}

void DeviceManager::send_json_error(HttpRes* res, int status, std::string_view reason) {
    send_http_response(res, status, json_error_body(reason), nullptr, false);
}

void DeviceManager::send_http_response(
    HttpRes* res,
    int status,
    std::string_view body,
    const std::vector<std::pair<std::string, std::string>>* headers,
    bool corked
) {
    auto write_fn = [&]() {
        res->writeStatus(format_status_line(status));
        bool has_ct = false;
        if (headers) {
            for (const auto& [k, v] : *headers) {
                if (iequals(k, "content-type")) has_ct = true;
                res->writeHeader(k, v);
            }
        }
        if (!has_ct && !body.empty()) {
            res->writeHeader("content-type", "application/json");
        }
        res->end(body);
    };
    if (corked) {
        res->cork(std::move(write_fn));
    } else {
        write_fn();
    }
}

} // namespace saihub
