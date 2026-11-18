#include "protocol.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>

namespace saihub {

namespace {

bool is_base58_char(char c) {
    return (c >= '1' && c <= '9') ||
           (c >= 'A' && c <= 'H') ||
           (c >= 'J' && c <= 'N') ||
           (c >= 'P' && c <= 'Z') ||
           (c >= 'a' && c <= 'k') ||
           (c >= 'm' && c <= 'z');
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

char to_lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

bool parse_array_index(std::string_view key, size_t len, size_t& idx_out) {
    // Matches JS Number(key) + Number.isInteger(index) + 0 <= index < len
    // Reject empty or whitespace-only? In JS Number("") === 0!
    // Wait: Number("0") === 0, Number("") === 0, Number(" 1 ") === 1, Number("abc") === NaN
    std::string_view trimmed = trim_ascii(key);
    if (trimmed.empty()) {
        if (len == 0) return false;
        idx_out = 0;
        return true;
    }
    size_t val = 0;
    auto [ptr, ec] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), val);
    if (ec != std::errc{} || ptr != trimmed.data() + trimmed.size() || val >= len) {
        return false;
    }
    idx_out = val;
    return true;
}

std::string unescape_json_pointer(std::string_view token) {
    std::string out;
    out.reserve(token.size());
    for (size_t i = 0; i < token.size(); ++i) {
        if (token[i] == '~' && i + 1 < token.size()) {
            if (token[i + 1] == '1') {
                out.push_back('/');
                ++i;
                continue;
            }
            if (token[i + 1] == '0') {
                out.push_back('~');
                ++i;
                continue;
            }
        }
        out.push_back(token[i]);
    }
    return out;
}

std::vector<std::string> split_json_pointer(std::string_view pointer) {
    std::vector<std::string> parts;
    if (pointer.empty() || pointer[0] != '/') return parts;
    size_t start = 1;
    while (start <= pointer.size()) {
        size_t slash = pointer.find('/', start);
        if (slash == std::string_view::npos) {
            parts.push_back(unescape_json_pointer(pointer.substr(start)));
            break;
        }
        parts.push_back(unescape_json_pointer(pointer.substr(start, slash - start)));
        start = slash + 1;
    }
    return parts;
}

yyjson_mut_val* mut_child_at(yyjson_mut_val* parent, std::string_view key) {
    if (!parent) return nullptr;
    if (yyjson_mut_is_arr(parent)) {
        size_t idx = 0;
        if (!parse_array_index(key, yyjson_mut_arr_size(parent), idx)) return nullptr;
        return yyjson_mut_arr_get(parent, idx);
    }
    if (yyjson_mut_is_obj(parent)) {
        return yyjson_mut_obj_getn(parent, key.data(), key.size());
    }
    return nullptr;
}

struct MutPointerTarget {
    yyjson_mut_val* parent = nullptr;
    std::string last;
};

std::optional<MutPointerTarget> resolve_mut_pointer(yyjson_mut_val* body, std::string_view pointer) {
    if (!body || (!yyjson_mut_is_obj(body) && !yyjson_mut_is_arr(body))) return std::nullopt;
    auto parts = split_json_pointer(pointer);
    if (parts.empty()) return std::nullopt;
    yyjson_mut_val* parent = body;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        parent = mut_child_at(parent, parts[i]);
        if (!parent || (!yyjson_mut_is_obj(parent) && !yyjson_mut_is_arr(parent))) {
            return std::nullopt;
        }
    }
    if (!parent || (!yyjson_mut_is_obj(parent) && !yyjson_mut_is_arr(parent))) {
        return std::nullopt;
    }
    return MutPointerTarget{parent, std::move(parts.back())};
}

yyjson_mut_val* mut_get_entry(yyjson_mut_val* parent, std::string_view key) {
    if (yyjson_mut_is_arr(parent)) {
        size_t idx = 0;
        if (!parse_array_index(key, yyjson_mut_arr_size(parent), idx)) return nullptr;
        return yyjson_mut_arr_get(parent, idx);
    }
    if (yyjson_mut_is_obj(parent)) {
        return yyjson_mut_obj_getn(parent, key.data(), key.size());
    }
    return nullptr;
}

bool has_zero_q_param(std::string_view params) {
    // Matches /;\s*q=0(?:\.0*)?\s*(?:;|$)/i
    size_t pos = 0;
    while (pos < params.size()) {
        size_t semi = params.find(';', pos);
        if (semi == std::string_view::npos) break;
        size_t next_semi = params.find(';', semi + 1);
        std::string_view seg = (next_semi == std::string_view::npos)
            ? params.substr(semi + 1)
            : params.substr(semi + 1, next_semi - semi - 1);
        seg = trim_ascii(seg);
        if (seg.size() >= 3 && (seg[0] == 'q' || seg[0] == 'Q') && seg[1] == '=' && seg[2] == '0') {
            std::string_view rest = seg.substr(3);
            if (rest.empty()) return true;
            if (rest[0] == '.') {
                bool all_zero = true;
                for (size_t k = 1; k < rest.size(); ++k) {
                    if (rest[k] != '0') {
                        all_zero = false;
                        break;
                    }
                }
                if (all_zero) return true;
            }
        }
        pos = semi + 1;
    }
    return false;
}

std::optional<AddWebsocketToken> parse_add_websocket_token(yyjson_val* cloud_v) {
    if (!yyjson_is_obj(cloud_v)) return std::nullopt;
    yyjson_val* spec = yyjson_obj_get(cloud_v, "addWebsocketToken");
    if (!yyjson_is_obj(spec)) return std::nullopt;
    yyjson_val* ws_path_v = yyjson_obj_get(spec, "websocketPath");
    yyjson_val* json_pathes_v = yyjson_obj_get(spec, "jsonPathes");
    if (!yyjson_is_str(ws_path_v) || !yyjson_is_arr(json_pathes_v)) return std::nullopt;

    std::string_view ws_path(yyjson_get_str(ws_path_v), yyjson_get_len(ws_path_v));
    if (ws_path.empty() || ws_path.size() > kMaxPathLen) return std::nullopt;

    AddWebsocketToken out;
    out.websocket_path.assign(ws_path);

    size_t idx = 0, max = 0;
    yyjson_val* item = nullptr;
    yyjson_arr_foreach(json_pathes_v, idx, max, item) {
        if (!yyjson_is_str(item)) return std::nullopt;
        std::string_view p(yyjson_get_str(item), yyjson_get_len(item));
        if (p.empty() || p[0] != '/' || p.size() > kMaxPathLen) return std::nullopt;
        out.json_pathes.emplace_back(p);
        if (out.json_pathes.size() >= 8) break;
    }
    if (out.json_pathes.empty()) return std::nullopt;

    yyjson_val* exp_pathes_v = yyjson_obj_get(spec, "expiredAtJsonPathes");
    if (yyjson_is_arr(exp_pathes_v)) {
        yyjson_arr_foreach(exp_pathes_v, idx, max, item) {
            if (!yyjson_is_str(item)) return out;
            std::string_view p(yyjson_get_str(item), yyjson_get_len(item));
            if (p.empty() || p[0] != '/' || p.size() > kMaxPathLen) {
                out.expired_at_json_pathes.clear();
                return out;
            }
            out.expired_at_json_pathes.emplace_back(p);
            if (out.expired_at_json_pathes.size() >= 8) break;
        }
    }
    return out;
}

bool parse_int_strict(yyjson_val* v, int64_t& out) {
    if (!v) return false;
    if (yyjson_is_sint(v)) {
        out = yyjson_get_sint(v);
        return true;
    }
    if (yyjson_is_uint(v)) {
        uint64_t u = yyjson_get_uint(v);
        if (u > static_cast<uint64_t>(INT64_MAX)) return false;
        out = static_cast<int64_t>(u);
        return true;
    }
    if (yyjson_is_real(v)) {
        double d = yyjson_get_real(v);
        if (!std::isfinite(d) || std::floor(d) != d) return false;
        if (d < -9007199254740992.0 || d > 9007199254740992.0) return false;
        out = static_cast<int64_t>(d);
        return true;
    }
    return false;
}

} // namespace

std::string_view trim_ascii(std::string_view sv) {
    while (!sv.empty() && is_space(sv.front())) sv.remove_prefix(1);
    while (!sv.empty() && is_space(sv.back())) sv.remove_suffix(1);
    return sv;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (to_lower_ascii(a[i]) != to_lower_ascii(b[i])) return false;
    }
    return true;
}

bool is_digest(std::string_view value) {
    if (value.size() < 26 || value.size() > 33) return false;
    for (char c : value) {
        if (!is_base58_char(c)) return false;
    }
    return true;
}

bool is_json_content_type(std::string_view value) {
    size_t semi = value.find(';');
    std::string_view media = trim_ascii(semi == std::string_view::npos ? value : value.substr(0, semi));
    return iequals(media, "application/json");
}

bool accepts_json(std::string_view accept) {
    if (accept.empty()) return true;
    size_t pos = 0;
    while (pos <= accept.size()) {
        size_t comma = accept.find(',', pos);
        std::string_view part = (comma == std::string_view::npos)
            ? accept.substr(pos)
            : accept.substr(pos, comma - pos);
        size_t semi = part.find(';');
        std::string_view media = trim_ascii(semi == std::string_view::npos ? part : part.substr(0, semi));
        std::string_view params = (semi == std::string_view::npos) ? std::string_view{} : part.substr(semi);
        if ((iequals(media, "application/json") || iequals(media, "application/*") || media == "*/*") &&
            !has_zero_q_param(params)) {
            return true;
        }
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
    return false;
}

std::string unsupported_content_type_json(std::string_view value, bool has_value) {
    std::string msg = "cloud relay not support ";
    if (!has_value || value.empty()) {
        msg += "missing content-type";
    } else {
        msg.append(value);
    }
    msg += " currently, use application/json instead";
    return serialize_json_error(msg);
}

std::string json_error_body(std::string_view reason) {
    return serialize_json_error(reason);
}

std::string serialize_json_error(std::string_view reason) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strn(doc, root, "reason", reason.data(), reason.size());
    size_t len = 0;
    char* out = yyjson_mut_write(doc, 0, &len);
    yyjson_mut_doc_free(doc);
    if (!out) return "{\"reason\":\"error\"}";
    std::string res(out, len);
    free(out);
    return res;
}

int pairing_error_status(std::string_view reason) {
    if (reason == "a pairing session is already active") return 409;
    if (reason == "grant secret limit reached (16)") return 422;
    if (reason == "session token is invalid or expired") return 401;
    return 400;
}

bool is_tcp_stream_path(std::string_view path) {
    return path == kTcpStreamDapPath || path == kTcpStreamRfc2217Path;
}

std::string_view tcp_stream_service(std::string_view path) {
    if (path == kTcpStreamDapPath) return "dap";
    if (path == kTcpStreamRfc2217Path) return "rfc2217";
    return {};
}

ParsedDeviceMessage parse_device_message(std::string_view raw) {
    ParsedDeviceMessage msg;
    yyjson_doc* doc = yyjson_read(raw.data(), raw.size(), 0);
    if (!doc) return msg;
    msg.doc.reset(doc);

    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) return msg;

    yyjson_val* type_v = yyjson_obj_get(root, "type");
    if (!yyjson_is_str(type_v)) return msg;
    std::string_view type(yyjson_get_str(type_v), yyjson_get_len(type_v));

    if (type == "authResponse") {
        yyjson_val* pk = yyjson_obj_get(root, "devicePublicKey");
        yyjson_val* dg = yyjson_obj_get(root, "devicePublicKeyDigest");
        yyjson_val* ver = yyjson_obj_get(root, "version");
        yyjson_val* resp = yyjson_obj_get(root, "response");
        if (!yyjson_is_str(pk) || !yyjson_is_str(dg) || !yyjson_is_str(ver) || !yyjson_is_str(resp)) {
            return msg;
        }
        std::string_view ver_sv(yyjson_get_str(ver), yyjson_get_len(ver));
        std::string_view dg_sv(yyjson_get_str(dg), yyjson_get_len(dg));
        if (ver_sv.empty() || ver_sv.size() > kMaxVersionLen || !is_digest(dg_sv)) {
            return msg;
        }
        msg.type = ParsedDeviceMessage::Type::AuthResponse;
        msg.auth_response.device_public_key.assign(yyjson_get_str(pk), yyjson_get_len(pk));
        msg.auth_response.device_public_key_digest.assign(dg_sv);
        msg.auth_response.version.assign(ver_sv);
        msg.auth_response.response.assign(yyjson_get_str(resp), yyjson_get_len(resp));
        return msg;
    }

    if (type == "response") {
        yyjson_val* req_id_v = yyjson_obj_get(root, "requestId");
        yyjson_val* status_v = yyjson_obj_get(root, "status");
        yyjson_val* headers_v = yyjson_obj_get(root, "headers");
        yyjson_val* body_v = yyjson_obj_get(root, "body");
        int64_t status = 0;
        if (!yyjson_is_str(req_id_v) || yyjson_get_len(req_id_v) == 0 ||
            !parse_int_strict(status_v, status) || status < 200 || status > 599 ||
            !yyjson_is_obj(headers_v) ||
            !body_v || !(yyjson_is_null(body_v) || yyjson_is_obj(body_v) || yyjson_is_arr(body_v))) {
            return msg;
        }
        if (yyjson_obj_size(headers_v) > kMaxHeaders) return msg;

        std::vector<std::pair<std::string, std::string>> hdrs;
        hdrs.reserve(yyjson_obj_size(headers_v));
        yyjson_val *hk, *hv;
        yyjson_obj_iter iter = yyjson_obj_iter_with(headers_v);
        while ((hk = yyjson_obj_iter_next(&iter))) {
            hv = yyjson_obj_iter_get_val(hk);
            std::string_view k_sv(yyjson_get_str(hk), yyjson_get_len(hk));
            if (k_sv.empty() || k_sv.size() > kMaxHeaderNameLen || !yyjson_is_str(hv)) {
                return msg;
            }
            std::string_view v_sv(yyjson_get_str(hv), yyjson_get_len(hv));
            if (v_sv.size() > kMaxHeaderValueLen) return msg;
            hdrs.emplace_back(std::string(k_sv), std::string(v_sv));
        }

        msg.type = ParsedDeviceMessage::Type::Response;
        msg.response.request_id.assign(yyjson_get_str(req_id_v), yyjson_get_len(req_id_v));
        msg.response.status = static_cast<int>(status);
        msg.response.headers = std::move(hdrs);
        msg.response.body = yyjson_is_null(body_v) ? nullptr : body_v;
        msg.response.add_websocket_token = parse_add_websocket_token(yyjson_obj_get(root, "cloud"));
        return msg;
    }

    if (type == "tcpStreamOpenResponse" || type == "tcpStreamCloseResponse") {
        yyjson_val* req_id_v = yyjson_obj_get(root, "requestId");
        yyjson_val* ok_v = yyjson_obj_get(root, "success");
        if (!yyjson_is_str(req_id_v) || yyjson_get_len(req_id_v) == 0 || !yyjson_is_bool(ok_v)) {
            return msg;
        }
        bool ok = yyjson_get_bool(ok_v);
        std::string_view req_id(yyjson_get_str(req_id_v), yyjson_get_len(req_id_v));
        if (type == "tcpStreamCloseResponse") {
            if (!ok) {
                yyjson_val* reason_v = yyjson_obj_get(root, "reason");
                if (!yyjson_is_str(reason_v) || yyjson_get_len(reason_v) == 0) return msg;
                msg.tcp_stream_close_response.reason.assign(yyjson_get_str(reason_v), yyjson_get_len(reason_v));
            }
            msg.type = ParsedDeviceMessage::Type::TcpStreamCloseResponse;
            msg.tcp_stream_close_response.request_id.assign(req_id);
            msg.tcp_stream_close_response.success = ok;
            return msg;
        }
        if (ok) {
            int64_t ch = 0, credits = 0;
            if (!parse_int_strict(yyjson_obj_get(root, "channelId"), ch) || ch < 0 || ch > 255 ||
                !parse_int_strict(yyjson_obj_get(root, "byteCredits"), credits) ||
                credits < 1 || credits > static_cast<int64_t>(kTcpStreamClientBufferMax)) {
                return msg;
            }
            msg.type = ParsedDeviceMessage::Type::TcpStreamOpenResponse;
            msg.tcp_stream_open_response.request_id.assign(req_id);
            msg.tcp_stream_open_response.success = true;
            msg.tcp_stream_open_response.channel_id = static_cast<int>(ch);
            msg.tcp_stream_open_response.byte_credits = static_cast<int>(credits);
            return msg;
        }
        yyjson_val* reason_v = yyjson_obj_get(root, "reason");
        if (!yyjson_is_str(reason_v) || yyjson_get_len(reason_v) == 0) return msg;
        msg.type = ParsedDeviceMessage::Type::TcpStreamOpenResponse;
        msg.tcp_stream_open_response.request_id.assign(req_id);
        msg.tcp_stream_open_response.success = false;
        msg.tcp_stream_open_response.reason.assign(yyjson_get_str(reason_v), yyjson_get_len(reason_v));
        return msg;
    }

    if (type == "tcpStreamAck") {
        int64_t ch = 0, len = 0;
        if (!parse_int_strict(yyjson_obj_get(root, "channelId"), ch) || ch < 0 || ch > 255 ||
            !parse_int_strict(yyjson_obj_get(root, "length"), len) ||
            len < 1 || len > static_cast<int64_t>(kTcpStreamClientBufferMax)) {
            return msg;
        }
        msg.type = ParsedDeviceMessage::Type::TcpStreamAck;
        msg.tcp_stream_ack.channel_id = static_cast<int>(ch);
        msg.tcp_stream_ack.length = static_cast<int>(len);
        return msg;
    }

    if (type == "pairingSessionResponse") {
        yyjson_val* ok_v = yyjson_obj_get(root, "success");
        if (!yyjson_is_bool(ok_v)) return msg;
        bool ok = yyjson_get_bool(ok_v);
        if (ok) {
            yyjson_val* st_v = yyjson_obj_get(root, "sessionToken");
            yyjson_val* exp_v = yyjson_obj_get(root, "expiredAt");
            if (!yyjson_is_str(st_v) || yyjson_get_len(st_v) == 0 || !yyjson_is_num(exp_v)) {
                return msg;
            }
            double exp_d = yyjson_get_num(exp_v);
            if (!std::isfinite(exp_d)) return msg;
            msg.type = ParsedDeviceMessage::Type::PairingSessionResponse;
            msg.pairing_session_response.success = true;
            msg.pairing_session_response.session_token.assign(yyjson_get_str(st_v), yyjson_get_len(st_v));
            msg.pairing_session_response.expired_at = static_cast<int64_t>(exp_d);
            return msg;
        }
        yyjson_val* reason_v = yyjson_obj_get(root, "reason");
        if (!yyjson_is_str(reason_v) || yyjson_get_len(reason_v) == 0) return msg;
        msg.type = ParsedDeviceMessage::Type::PairingSessionResponse;
        msg.pairing_session_response.success = false;
        msg.pairing_session_response.reason.assign(yyjson_get_str(reason_v), yyjson_get_len(reason_v));
        return msg;
    }

    if (type == "pairingSessionTokenResponse") {
        yyjson_val* ok_v = yyjson_obj_get(root, "success");
        if (!yyjson_is_bool(ok_v)) return msg;
        bool ok = yyjson_get_bool(ok_v);
        if (ok) {
            yyjson_val* gs_v = yyjson_obj_get(root, "grantSecret");
            if (!yyjson_is_str(gs_v) || yyjson_get_len(gs_v) == 0) return msg;
            msg.type = ParsedDeviceMessage::Type::PairingSessionTokenResponse;
            msg.pairing_token_response.success = true;
            msg.pairing_token_response.grant_secret.assign(yyjson_get_str(gs_v), yyjson_get_len(gs_v));
            return msg;
        }
        yyjson_val* reason_v = yyjson_obj_get(root, "reason");
        if (!yyjson_is_str(reason_v) || yyjson_get_len(reason_v) == 0) return msg;
        msg.type = ParsedDeviceMessage::Type::PairingSessionTokenResponse;
        msg.pairing_token_response.success = false;
        msg.pairing_token_response.reason.assign(yyjson_get_str(reason_v), yyjson_get_len(reason_v));
        return msg;
    }

    return msg;
}

void inject_websocket_token(
    yyjson_mut_doc* doc,
    yyjson_mut_val* body,
    const std::vector<std::string>& pointers,
    std::string_view token
) {
    if (!doc || !body) return;
    for (const auto& pointer : pointers) {
        auto resolved = resolve_mut_pointer(body, pointer);
        if (!resolved) continue;
        yyjson_mut_val* entry = mut_get_entry(resolved->parent, resolved->last);
        if (!entry || !yyjson_mut_is_str(entry)) continue;
        std::string_view cur(yyjson_mut_get_str(entry), yyjson_mut_get_len(entry));
        size_t pos = cur.find(kWebsocketTokenPlaceholder);
        if (pos == std::string_view::npos) continue;
        std::string updated;
        updated.reserve(cur.size() - kWebsocketTokenPlaceholder.size() + token.size());
        updated.append(cur.substr(0, pos));
        updated.append(token);
        updated.append(cur.substr(pos + kWebsocketTokenPlaceholder.size()));
        yyjson_mut_val* copied = yyjson_mut_strncpy(doc, updated.data(), updated.size());
        if (copied) yyjson_mut_set_strn(entry, yyjson_mut_get_str(copied), updated.size());
    }
}

void inject_expired_at(
    yyjson_mut_doc* doc,
    yyjson_mut_val* body,
    const std::vector<std::string>& pointers,
    int64_t expired_at
) {
    if (!doc || !body) return;
    static constexpr std::string_view kNeedle = "\"expiredAt\":0";
    std::string replacement = "\"expiredAt\":" + std::to_string(expired_at);
    for (const auto& pointer : pointers) {
        auto resolved = resolve_mut_pointer(body, pointer);
        if (!resolved) continue;
        yyjson_mut_val* entry = mut_get_entry(resolved->parent, resolved->last);
        if (!entry) continue;
        if (yyjson_mut_is_num(entry)) {
            yyjson_mut_set_sint(entry, expired_at);
        } else if (yyjson_mut_is_str(entry)) {
            std::string_view cur(yyjson_mut_get_str(entry), yyjson_mut_get_len(entry));
            size_t pos = cur.find(kNeedle);
            if (pos == std::string_view::npos) continue;
            std::string updated;
            updated.reserve(cur.size() - kNeedle.size() + replacement.size());
            updated.append(cur.substr(0, pos));
            updated.append(replacement);
            updated.append(cur.substr(pos + kNeedle.size()));
            yyjson_mut_val* copied = yyjson_mut_strncpy(doc, updated.data(), updated.size());
            if (copied) yyjson_mut_set_strn(entry, yyjson_mut_get_str(copied), updated.size());
        }
    }
}

} // namespace saihub
