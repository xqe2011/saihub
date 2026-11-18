#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace saihub {

std::string bytes_to_base64url(std::span<const uint8_t> bytes);
bool base64url_to_bytes(std::string_view value, std::vector<uint8_t>& out);

std::array<uint8_t, 32> sha256_bytes(std::span<const uint8_t> data);
bool random_bytes(std::span<uint8_t> out);

std::optional<std::string> base58check_encode(std::span<const uint8_t> payload);
std::optional<std::string> public_key_digest(std::span<const uint8_t> public_key);

bool constant_time_equal(std::string_view a, std::string_view b);

struct AuthVerifyResult {
    bool ok = false;
    std::string reason;
};

AuthVerifyResult verify_device_auth(
    std::string_view public_key_b64,
    std::string_view signature_b64,
    std::span<const uint8_t> challenge,
    std::string_view expected_digest,
    std::string_view claimed_digest
);

struct RoutingTokenPayload {
    std::string device_public_key_digest;
    std::string grant_secret;
};

struct WebsocketTokenPayload {
    int64_t expire_at = 0;
    std::string device_public_key_digest;
    std::string path;
    std::string grant_secret;
};

// Caches SHA-256(secret) AES-256-GCM key & reusable cipher contexts for zero-alloc token ops.
class AesGcmKey {
public:
    AesGcmKey() = default;
    explicit AesGcmKey(std::string_view secret);
    ~AesGcmKey();

    AesGcmKey(const AesGcmKey&) = delete;
    AesGcmKey& operator=(const AesGcmKey&) = delete;
    AesGcmKey(AesGcmKey&&) noexcept;
    AesGcmKey& operator=(AesGcmKey&&) noexcept;

    bool valid() const noexcept { return valid_; }

    std::optional<std::string> seal(std::span<const uint8_t> plaintext) const;
    bool open(std::string_view token_b64, std::vector<uint8_t>& plaintext_out) const;

private:
    bool valid_ = false;
    std::array<uint8_t, 32> key_{};
};

std::optional<std::string> seal_routing_token(const AesGcmKey& key, const RoutingTokenPayload& payload);
std::optional<RoutingTokenPayload> open_routing_token(const AesGcmKey& key, std::string_view token);

std::optional<std::string> seal_websocket_token(const AesGcmKey& key, const WebsocketTokenPayload& payload);
std::optional<WebsocketTokenPayload> open_websocket_token(
    const AesGcmKey& key,
    std::string_view token,
    double now_sec
);

} // namespace saihub
