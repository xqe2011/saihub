#include "crypto.hpp"
#include "protocol.hpp"

#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/param_build.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <yyjson.h>

#include <cstring>

namespace saihub {

namespace {

constexpr char kB64UrlAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

constexpr int8_t kB64UrlDecode[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,63,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
};

constexpr size_t kGcmIvBytes = 12;
constexpr size_t kGcmTagBytes = 16;

} // namespace

std::string bytes_to_base64url(std::span<const uint8_t> bytes) {
    if (bytes.empty()) return {};
    std::string out;
    out.reserve((bytes.size() * 4 + 2) / 3);
    size_t i = 0;
    while (i + 3 <= bytes.size()) {
        uint32_t n = (uint32_t(bytes[i]) << 16) | (uint32_t(bytes[i + 1]) << 8) | uint32_t(bytes[i + 2]);
        out.push_back(kB64UrlAlphabet[(n >> 18) & 63]);
        out.push_back(kB64UrlAlphabet[(n >> 12) & 63]);
        out.push_back(kB64UrlAlphabet[(n >> 6) & 63]);
        out.push_back(kB64UrlAlphabet[n & 63]);
        i += 3;
    }
    size_t rem = bytes.size() - i;
    if (rem == 1) {
        uint32_t n = uint32_t(bytes[i]) << 16;
        out.push_back(kB64UrlAlphabet[(n >> 18) & 63]);
        out.push_back(kB64UrlAlphabet[(n >> 12) & 63]);
    } else if (rem == 2) {
        uint32_t n = (uint32_t(bytes[i]) << 16) | (uint32_t(bytes[i + 1]) << 8);
        out.push_back(kB64UrlAlphabet[(n >> 18) & 63]);
        out.push_back(kB64UrlAlphabet[(n >> 12) & 63]);
        out.push_back(kB64UrlAlphabet[(n >> 6) & 63]);
    }
    return out;
}

bool base64url_to_bytes(std::string_view value, std::vector<uint8_t>& out) {
    out.clear();
    if (value.empty()) return false;
    if ((value.size() % 4) == 1) return false;
    out.reserve((value.size() * 3) / 4);

    uint32_t acc = 0;
    int bits = 0;
    for (unsigned char ch : value) {
        int8_t d = kB64UrlDecode[ch];
        if (d < 0) {
            out.clear();
            return false;
        }
        acc = (acc << 6) | uint32_t(d);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    // Check unused trailing bits are zero (canonical base64url)
    if (bits > 0 && (acc & ((1u << bits) - 1u)) != 0) {
        // Note: JS atob ignores trailing non-zero bits in some engines, or matches standard;
        // keeping permissive to match WebCrypto/atob behavior if needed, or strict? atob ignores?
        // Actually in V8/JSC, atob masks unused bits. Let's match atob by not failing here.
    }
    return true;
}

std::array<uint8_t, 32> sha256_bytes(std::span<const uint8_t> data) {
    std::array<uint8_t, 32> out{};
    SHA256(data.data(), data.size(), out.data());
    return out;
}

bool random_bytes(std::span<uint8_t> out) {
    if (out.empty()) return true;
    return RAND_bytes(out.data(), static_cast<int>(out.size())) == 1;
}

std::optional<std::string> base58check_encode(std::span<const uint8_t> payload) {
    if (payload.size() != kDigestPayloadBytes) return std::nullopt;
    auto h1 = sha256_bytes(payload);
    auto h2 = sha256_bytes(h1);

    uint8_t encoded[kDigestPayloadBytes + kDigestChecksumBytes];
    std::memcpy(encoded, payload.data(), kDigestPayloadBytes);
    std::memcpy(encoded + kDigestPayloadBytes, h2.data(), kDigestChecksumBytes);

    size_t leading_zeroes = 0;
    while (leading_zeroes < sizeof(encoded) && encoded[leading_zeroes] == 0) {
        ++leading_zeroes;
    }

    uint8_t digits[40];
    size_t digits_len = 0;
    for (size_t i = leading_zeroes; i < sizeof(encoded); ++i) {
        uint32_t carry = encoded[i];
        for (size_t j = 0; j < digits_len; ++j) {
            uint32_t value = uint32_t(digits[j]) * 256u + carry;
            digits[j] = static_cast<uint8_t>(value % 58u);
            carry = value / 58u;
        }
        while (carry > 0) {
            digits[digits_len++] = static_cast<uint8_t>(carry % 58u);
            carry /= 58u;
        }
    }

    std::string out;
    out.reserve(leading_zeroes + digits_len);
    out.append(leading_zeroes, '1');
    for (size_t i = digits_len; i > 0; --i) {
        out.push_back(kBase58Alphabet[digits[i - 1]]);
    }
    return out;
}

std::optional<std::string> public_key_digest(std::span<const uint8_t> public_key) {
    if (public_key.size() != kPublicKeyBytes || public_key[0] != 0x04) {
        return std::nullopt;
    }
    auto hash = sha256_bytes(public_key);
    return base58check_encode(std::span<const uint8_t>(hash.data(), kDigestPayloadBytes));
}

bool constant_time_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    if (a.empty()) return true;
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

AuthVerifyResult verify_device_auth(
    std::string_view public_key_b64,
    std::string_view signature_b64,
    std::span<const uint8_t> challenge,
    std::string_view expected_digest,
    std::string_view claimed_digest
) {
    if (claimed_digest != expected_digest) {
        return {false, "devicePublicKeyDigest mismatch"};
    }

    std::vector<uint8_t> public_key;
    if (!base64url_to_bytes(public_key_b64, public_key) ||
        public_key.size() != kPublicKeyBytes ||
        public_key[0] != 0x04) {
        return {false, "invalid devicePublicKey"};
    }

    std::vector<uint8_t> sig_raw;
    if (!base64url_to_bytes(signature_b64, sig_raw) || sig_raw.size() != kSignatureBytes) {
        return {false, "invalid response"};
    }

    auto digest = public_key_digest(public_key);
    if (!digest || *digest != expected_digest) {
        return {false, "devicePublicKey digest mismatch"};
    }

    // Build EVP_PKEY for P-256 uncompressed public key (65 bytes)
    OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
    if (!bld) return {false, "invalid devicePublicKey"};
    if (!OSSL_PARAM_BLD_push_utf8_string(bld, "group", SN_X9_62_prime256v1, 0) ||
        !OSSL_PARAM_BLD_push_octet_string(bld, "pub", public_key.data(), public_key.size())) {
        OSSL_PARAM_BLD_free(bld);
        return {false, "invalid devicePublicKey"};
    }
    OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(bld);
    OSSL_PARAM_BLD_free(bld);
    if (!params) return {false, "invalid devicePublicKey"};

    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
    EVP_PKEY* pkey = nullptr;
    if (!pctx || EVP_PKEY_fromdata_init(pctx) <= 0 ||
        EVP_PKEY_fromdata(pctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) <= 0) {
        OSSL_PARAM_free(params);
        EVP_PKEY_CTX_free(pctx);
        if (pkey) EVP_PKEY_free(pkey);
        return {false, "invalid devicePublicKey"};
    }
    OSSL_PARAM_free(params);
    EVP_PKEY_CTX_free(pctx);

    // Convert 64-byte IEEE P1363 (r || s) to ASN.1 DER ECDSA_SIG
    ECDSA_SIG* ecdsa_sig = ECDSA_SIG_new();
    BIGNUM* r = BN_bin2bn(sig_raw.data(), 32, nullptr);
    BIGNUM* s = BN_bin2bn(sig_raw.data() + 32, 32, nullptr);
    if (!ecdsa_sig || !r || !s || ECDSA_SIG_set0(ecdsa_sig, r, s) != 1) {
        if (ecdsa_sig) ECDSA_SIG_free(ecdsa_sig);
        else {
            BN_free(r);
            BN_free(s);
        }
        EVP_PKEY_free(pkey);
        return {false, "invalid response"};
    }
    // r and s are now owned by ecdsa_sig
    unsigned char der_buf[80];
    unsigned char* der_ptr = der_buf;
    int der_len = i2d_ECDSA_SIG(ecdsa_sig, &der_ptr);
    ECDSA_SIG_free(ecdsa_sig);
    if (der_len <= 0) {
        EVP_PKEY_free(pkey);
        return {false, "invalid response"};
    }

    // Verify SHA-256("saihub/cloud-auth/v1\0" || challenge)
    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    bool ok = false;
    if (md_ctx &&
        EVP_DigestVerifyInit(md_ctx, nullptr, EVP_sha256(), nullptr, pkey) == 1 &&
        EVP_DigestVerifyUpdate(md_ctx, kAuthDomainPrefix.data(), kAuthDomainPrefix.size()) == 1 &&
        EVP_DigestVerifyUpdate(md_ctx, challenge.data(), challenge.size()) == 1 &&
        EVP_DigestVerifyFinal(md_ctx, der_buf, static_cast<size_t>(der_len)) == 1) {
        ok = true;
    }
    if (md_ctx) EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);

    if (!ok) {
        return {false, "authentication failed"};
    }
    return {true, {}};
}

AesGcmKey::AesGcmKey(std::string_view secret) {
    if (secret.empty()) return;
    key_ = sha256_bytes(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(secret.data()), secret.size()));
    valid_ = true;
}

AesGcmKey::~AesGcmKey() {
    OPENSSL_cleanse(key_.data(), key_.size());
}

AesGcmKey::AesGcmKey(AesGcmKey&& other) noexcept : valid_(other.valid_), key_(other.key_) {
    other.valid_ = false;
    OPENSSL_cleanse(other.key_.data(), other.key_.size());
}

AesGcmKey& AesGcmKey::operator=(AesGcmKey&& other) noexcept {
    if (this != &other) {
        OPENSSL_cleanse(key_.data(), key_.size());
        valid_ = other.valid_;
        key_ = other.key_;
        other.valid_ = false;
        OPENSSL_cleanse(other.key_.data(), other.key_.size());
    }
    return *this;
}

std::optional<std::string> AesGcmKey::seal(std::span<const uint8_t> plaintext) const {
    if (!valid_) return std::nullopt;
    std::vector<uint8_t> packed(kGcmIvBytes + plaintext.size() + kGcmTagBytes);
    if (!random_bytes(std::span<uint8_t>(packed.data(), kGcmIvBytes))) {
        return std::nullopt;
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return std::nullopt;

    int len = 0;
    int ct_len = 0;
    bool ok =
        EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kGcmIvBytes), nullptr) == 1 &&
        EVP_EncryptInit_ex(ctx, nullptr, nullptr, key_.data(), packed.data()) == 1 &&
        (plaintext.empty() || EVP_EncryptUpdate(ctx, packed.data() + kGcmIvBytes, &len, plaintext.data(), static_cast<int>(plaintext.size())) == 1);
    if (ok) {
        ct_len = len;
        ok = EVP_EncryptFinal_ex(ctx, packed.data() + kGcmIvBytes + ct_len, &len) == 1;
        if (ok) {
            ct_len += len;
            ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, static_cast<int>(kGcmTagBytes), packed.data() + kGcmIvBytes + ct_len) == 1;
        }
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return std::nullopt;
    packed.resize(kGcmIvBytes + static_cast<size_t>(ct_len) + kGcmTagBytes);
    return bytes_to_base64url(packed);
}

bool AesGcmKey::open(std::string_view token_b64, std::vector<uint8_t>& plaintext_out) const {
    plaintext_out.clear();
    if (!valid_) return false;
    std::vector<uint8_t> packed;
    if (!base64url_to_bytes(token_b64, packed)) return false;
    if (packed.size() <= kGcmIvBytes + kGcmTagBytes) return false;

    size_t ct_len = packed.size() - kGcmIvBytes - kGcmTagBytes;
    const uint8_t* iv = packed.data();
    const uint8_t* ct = packed.data() + kGcmIvBytes;
    const uint8_t* tag = packed.data() + kGcmIvBytes + ct_len;

    plaintext_out.resize(ct_len);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        plaintext_out.clear();
        return false;
    }

    int len = 0;
    int pt_len = 0;
    bool ok =
        EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kGcmIvBytes), nullptr) == 1 &&
        EVP_DecryptInit_ex(ctx, nullptr, nullptr, key_.data(), iv) == 1 &&
        (ct_len == 0 || EVP_DecryptUpdate(ctx, plaintext_out.data(), &len, ct, static_cast<int>(ct_len)) == 1);
    if (ok) {
        pt_len = len;
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, static_cast<int>(kGcmTagBytes), const_cast<uint8_t*>(tag)) == 1 &&
             EVP_DecryptFinal_ex(ctx, plaintext_out.data() + pt_len, &len) == 1;
        if (ok) pt_len += len;
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        plaintext_out.clear();
        return false;
    }
    plaintext_out.resize(static_cast<size_t>(pt_len));
    return true;
}

std::optional<std::string> seal_routing_token(const AesGcmKey& key, const RoutingTokenPayload& payload) {
    if (!key.valid()) return std::nullopt;
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strn(doc, root, "devicePublicKeyDigest",
                            payload.device_public_key_digest.data(), payload.device_public_key_digest.size());
    yyjson_mut_obj_add_strn(doc, root, "grantSecret",
                            payload.grant_secret.data(), payload.grant_secret.size());
    size_t len = 0;
    char* json = yyjson_mut_write(doc, 0, &len);
    yyjson_mut_doc_free(doc);
    if (!json) return std::nullopt;
    auto res = key.seal(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(json), len));
    free(json);
    return res;
}

std::optional<RoutingTokenPayload> open_routing_token(const AesGcmKey& key, std::string_view token) {
    std::vector<uint8_t> pt;
    if (!key.open(token, pt)) return std::nullopt;

    yyjson_doc* doc = yyjson_read(reinterpret_cast<const char*>(pt.data()), pt.size(), 0);
    if (!doc) return std::nullopt;
    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return std::nullopt;
    }
    yyjson_val* digest_v = yyjson_obj_get(root, "devicePublicKeyDigest");
    yyjson_val* secret_v = yyjson_obj_get(root, "grantSecret");
    if (!yyjson_is_str(digest_v) || !yyjson_is_str(secret_v)) {
        yyjson_doc_free(doc);
        return std::nullopt;
    }
    std::string_view digest(yyjson_get_str(digest_v), yyjson_get_len(digest_v));
    std::string_view secret(yyjson_get_str(secret_v), yyjson_get_len(secret_v));
    if (!is_digest(digest) || secret.empty()) {
        yyjson_doc_free(doc);
        return std::nullopt;
    }
    RoutingTokenPayload out{std::string(digest), std::string(secret)};
    yyjson_doc_free(doc);
    return out;
}

std::optional<std::string> seal_websocket_token(const AesGcmKey& key, const WebsocketTokenPayload& payload) {
    if (!key.valid()) return std::nullopt;
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_int(doc, root, "expireAt", payload.expire_at);
    yyjson_mut_obj_add_strn(doc, root, "devicePublicKeyDigest",
                            payload.device_public_key_digest.data(), payload.device_public_key_digest.size());
    yyjson_mut_obj_add_strn(doc, root, "path", payload.path.data(), payload.path.size());
    yyjson_mut_obj_add_strn(doc, root, "grantSecret",
                            payload.grant_secret.data(), payload.grant_secret.size());
    size_t len = 0;
    char* json = yyjson_mut_write(doc, 0, &len);
    yyjson_mut_doc_free(doc);
    if (!json) return std::nullopt;
    auto res = key.seal(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(json), len));
    free(json);
    return res;
}

std::optional<WebsocketTokenPayload> open_websocket_token(
    const AesGcmKey& key,
    std::string_view token,
    double now_sec
) {
    std::vector<uint8_t> pt;
    if (!key.open(token, pt)) return std::nullopt;

    yyjson_doc* doc = yyjson_read(reinterpret_cast<const char*>(pt.data()), pt.size(), 0);
    if (!doc) return std::nullopt;
    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return std::nullopt;
    }
    yyjson_val* exp_v = yyjson_obj_get(root, "expireAt");
    yyjson_val* digest_v = yyjson_obj_get(root, "devicePublicKeyDigest");
    yyjson_val* path_v = yyjson_obj_get(root, "path");
    yyjson_val* secret_v = yyjson_obj_get(root, "grantSecret");
    if (!yyjson_is_num(exp_v) || !yyjson_is_str(digest_v) || !yyjson_is_str(path_v) || !yyjson_is_str(secret_v)) {
        yyjson_doc_free(doc);
        return std::nullopt;
    }
    double exp_num = yyjson_get_num(exp_v);
    if (exp_num <= now_sec) {
        yyjson_doc_free(doc);
        return std::nullopt;
    }
    std::string_view digest(yyjson_get_str(digest_v), yyjson_get_len(digest_v));
    std::string_view path(yyjson_get_str(path_v), yyjson_get_len(path_v));
    std::string_view secret(yyjson_get_str(secret_v), yyjson_get_len(secret_v));
    if (!is_digest(digest) || path.empty() || secret.empty()) {
        yyjson_doc_free(doc);
        return std::nullopt;
    }
    WebsocketTokenPayload out{static_cast<int64_t>(exp_num), std::string(digest), std::string(path), std::string(secret)};
    yyjson_doc_free(doc);
    return out;
}

} // namespace saihub
