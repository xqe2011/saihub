#pragma once

#include <yyjson.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace saihub {

inline constexpr std::string_view kBase58Alphabet =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

inline constexpr size_t kMaxVersionLen = 64;
inline constexpr size_t kMaxHeaderNameLen = 64;
inline constexpr size_t kMaxHeaderValueLen = 512;
inline constexpr size_t kMaxHeaders = 16;
inline constexpr size_t kMaxPathLen = 1024;
inline constexpr size_t kMaxBodyBytes = 64 * 1024;
inline constexpr size_t kMaxResponseBodyBytes = 1024 * 1024;
inline constexpr size_t kMaxPairingNameLen = 32;
inline constexpr size_t kPublicKeyBytes = 65;
inline constexpr size_t kSignatureBytes = 64;
inline constexpr size_t kChallengeBytes = 32;
inline constexpr size_t kTcpStreamClientBufferMax = 1024 * 1024;
inline constexpr size_t kDigestPayloadBytes = 20;
inline constexpr size_t kDigestChecksumBytes = 4;
inline constexpr std::string_view kAuthDomainPrefix("saihub/cloud-auth/v1\0", 21);

inline constexpr std::string_view kWebsocketTokenPlaceholder = "$WEBSOCKET_TOKEN$";
inline constexpr std::string_view kTcpStreamDapPath = "/tcp-stream/dap";
inline constexpr std::string_view kTcpStreamRfc2217Path = "/tcp-stream/rfc2217";

inline constexpr std::array<std::string_view, 6> kForwardedHeaders = {
    "content-type",
    "accept",
    "x-lock-id",
    "mcp-protocol-version",
    "mcp-session-id",
    "origin",
};

bool is_digest(std::string_view value);
bool is_json_content_type(std::string_view value);
bool accepts_json(std::string_view accept);
std::string unsupported_content_type_json(std::string_view value, bool has_value);
std::string json_error_body(std::string_view reason);

int pairing_error_status(std::string_view reason);
bool is_tcp_stream_path(std::string_view path);
std::string_view tcp_stream_service(std::string_view path);

bool iequals(std::string_view a, std::string_view b);
std::string_view trim_ascii(std::string_view sv);

struct AddWebsocketToken {
    std::vector<std::string> json_pathes;
    std::string websocket_path;
    std::vector<std::string> expired_at_json_pathes;
};

struct AuthResponseMsg {
    std::string device_public_key;
    std::string device_public_key_digest;
    std::string version;
    std::string response;
};

struct ResponseMsg {
    std::string request_id;
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    yyjson_val* body = nullptr; // points into ParsedDeviceMessage::doc (null if JSON null)
    std::optional<AddWebsocketToken> add_websocket_token;
};

struct PairingSessionResponseMsg {
    bool success = false;
    std::string session_token;
    int64_t expired_at = 0;
    std::string reason;
};

struct PairingSessionTokenResponseMsg {
    bool success = false;
    std::string grant_secret;
    std::string reason;
};

struct TcpStreamOpenResponseMsg {
    std::string request_id;
    bool success = false;
    int channel_id = 0;
    int byte_credits = 0;
    std::string reason;
};

struct TcpStreamCloseResponseMsg {
    std::string request_id;
    bool success = false;
    std::string reason;
};

struct TcpStreamAckMsg {
    int channel_id = 0;
    int length = 0;
};

struct ParsedDeviceMessage {
    enum class Type {
        Invalid,
        AuthResponse,
        Response,
        PairingSessionResponse,
        PairingSessionTokenResponse,
        TcpStreamOpenResponse,
        TcpStreamCloseResponse,
        TcpStreamAck,
    };

    Type type = Type::Invalid;
    std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> doc{nullptr, yyjson_doc_free};

    AuthResponseMsg auth_response;
    ResponseMsg response;
    PairingSessionResponseMsg pairing_session_response;
    PairingSessionTokenResponseMsg pairing_token_response;
    TcpStreamOpenResponseMsg tcp_stream_open_response;
    TcpStreamCloseResponseMsg tcp_stream_close_response;
    TcpStreamAckMsg tcp_stream_ack;
};

ParsedDeviceMessage parse_device_message(std::string_view raw);

// JSON pointer mutations on mutable yyjson tree (matches protocol.ts injectWebsocketToken / injectExpiredAt)
void inject_websocket_token(yyjson_mut_doc* doc, yyjson_mut_val* body, const std::vector<std::string>& pointers, std::string_view token);
void inject_expired_at(yyjson_mut_doc* doc, yyjson_mut_val* body, const std::vector<std::string>& pointers, int64_t expired_at);

std::string serialize_json_error(std::string_view reason);

} // namespace saihub
