#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "config.hpp"
#include "crypto.hpp"
#include "device_manager.hpp"
#include "html.hpp"
#include "protocol.hpp"
#include "store.hpp"

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <yyjson.h>
#include <zlib.h>

#include <cstdio>
#include <unistd.h>
#include <vector>

using namespace saihub;

namespace {

struct TestIdentity {
    EVP_PKEY* pkey = nullptr;
    std::vector<uint8_t> pub_raw;
    std::string pub_b64;
    std::string digest;

    ~TestIdentity() {
        if (pkey) EVP_PKEY_free(pkey);
    }
};

TestIdentity generate_identity() {
    TestIdentity id;
    id.pkey = EVP_EC_gen("P-256");
    REQUIRE(id.pkey != nullptr);

    size_t len = 0;
    REQUIRE(EVP_PKEY_get_octet_string_param(id.pkey, "pub", nullptr, 0, &len) == 1);
    REQUIRE(len == kPublicKeyBytes);
    id.pub_raw.resize(len);
    REQUIRE(EVP_PKEY_get_octet_string_param(id.pkey, "pub", id.pub_raw.data(), len, &len) == 1);
    REQUIRE(id.pub_raw[0] == 0x04);

    id.pub_b64 = bytes_to_base64url(id.pub_raw);
    auto dg = public_key_digest(id.pub_raw);
    REQUIRE(dg.has_value());
    id.digest = *dg;
    return id;
}

std::string sign_challenge(EVP_PKEY* pkey, std::span<const uint8_t> challenge, bool with_prefix = true) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    REQUIRE(ctx != nullptr);
    REQUIRE(EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) == 1);
    if (with_prefix) {
        REQUIRE(EVP_DigestSignUpdate(ctx, kAuthDomainPrefix.data(), kAuthDomainPrefix.size()) == 1);
    }
    REQUIRE(EVP_DigestSignUpdate(ctx, challenge.data(), challenge.size()) == 1);
    size_t sig_len = 0;
    REQUIRE(EVP_DigestSignFinal(ctx, nullptr, &sig_len) == 1);
    std::vector<uint8_t> der(sig_len);
    REQUIRE(EVP_DigestSignFinal(ctx, der.data(), &sig_len) == 1);
    EVP_MD_CTX_free(ctx);

    const unsigned char* p = der.data();
    ECDSA_SIG* esig = d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(sig_len));
    REQUIRE(esig != nullptr);
    const BIGNUM* r = nullptr;
    const BIGNUM* s = nullptr;
    ECDSA_SIG_get0(esig, &r, &s);
    std::vector<uint8_t> raw(kSignatureBytes, 0);
    BN_bn2binpad(r, raw.data(), 32);
    BN_bn2binpad(s, raw.data() + 32, 32);
    ECDSA_SIG_free(esig);
    return bytes_to_base64url(raw);
}

} // namespace

TEST_CASE("base64url roundtrip") {
    std::vector<uint8_t> src = {0x00, 0x01, 0x02, 0xfe, 0xff, 0x42};
    std::string enc = bytes_to_base64url(src);
    std::vector<uint8_t> dec;
    CHECK(base64url_to_bytes(enc, dec));
    CHECK(dec == src);
    CHECK_FALSE(base64url_to_bytes("", dec));
    CHECK_FALSE(base64url_to_bytes("abc+def", dec));
}

TEST_CASE("digest validation and publicKeyDigest") {
    auto id = generate_identity();
    CHECK(is_digest(id.digest));
    CHECK_FALSE(is_digest("short"));
    CHECK_FALSE(is_digest("0OIl" + std::string(26, '1'))); // invalid base58 chars

    // Known placeholder digest from tests/cloud/cloudflare/client.test.ts
    CHECK(is_digest("KUCzSr49wPWckUDouJLybJuRYtVpMajPX"));
}

TEST_CASE("ECDSA P-256 auth verification") {
    auto id = generate_identity();
    std::vector<uint8_t> challenge(32, 0xab);
    std::string sig = sign_challenge(id.pkey, challenge, true);

    auto ok = verify_device_auth(id.pub_b64, sig, challenge, id.digest, id.digest);
    CHECK(ok.ok);

    // Without domain prefix must fail
    std::string bare = sign_challenge(id.pkey, challenge, false);
    auto bad_prefix = verify_device_auth(id.pub_b64, bare, challenge, id.digest, id.digest);
    CHECK_FALSE(bad_prefix.ok);

    // Digest mismatch must fail
    auto bad_digest = verify_device_auth(id.pub_b64, sig, challenge, id.digest, "KUCzSr49wPWckUDouJLybJuRYtVpMajPX");
    CHECK_FALSE(bad_digest.ok);
}

TEST_CASE("AES-256-GCM routing and websocket tokens") {
    AesGcmKey rkey("local-dev-routing-token-secret");
    auto id = generate_identity();
    auto sealed = seal_routing_token(rkey, RoutingTokenPayload{id.digest, "grant-secret"});
    REQUIRE(sealed.has_value());
    auto opened = open_routing_token(rkey, *sealed);
    REQUIRE(opened.has_value());
    CHECK(opened->device_public_key_digest == id.digest);
    CHECK(opened->grant_secret == "grant-secret");

    AesGcmKey wkey("local-dev-websocket-token-secret");
    auto ws_tok = seal_websocket_token(wkey, WebsocketTokenPayload{2000, id.digest, "/tcp-stream/dap", "gs"});
    REQUIRE(ws_tok.has_value());
    CHECK(open_websocket_token(wkey, *ws_tok, 1000.0).has_value());
    CHECK_FALSE(open_websocket_token(wkey, *ws_tok, 2000.0).has_value());
}

TEST_CASE("content-type and accept header matching") {
    CHECK(is_json_content_type("application/json"));
    CHECK(is_json_content_type("Application/JSON; charset=utf-8"));
    CHECK_FALSE(is_json_content_type("text/plain"));
    CHECK_FALSE(is_json_content_type("application/jsonp"));

    CHECK(accepts_json(""));
    CHECK(accepts_json("application/json"));
    CHECK(accepts_json("application/json, text/event-stream"));
    CHECK(accepts_json("*/*"));
    CHECK(accepts_json("application/*; q=0.8"));
    CHECK_FALSE(accepts_json("text/event-stream"));
    CHECK_FALSE(accepts_json("application/json; q=0"));
    CHECK_FALSE(accepts_json("application/json; q=0.00"));
}

TEST_CASE("injectWebsocketToken and injectExpiredAt") {
    const char* raw = R"({"prompt":"p $WEBSOCKET_TOKEN$ $WEBSOCKET_TOKEN$","url":"ws://x?token=$WEBSOCKET_TOKEN$","expiredAt":0,"nested":{"content":[{"text":"{\"url\":\"ws://y?t=$WEBSOCKET_TOKEN$\",\"expiredAt\":0}"}]}})";
    yyjson_doc* doc = yyjson_read(raw, strlen(raw), 0);
    REQUIRE(doc != nullptr);
    yyjson_mut_doc* mdoc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_val_mut_copy(mdoc, yyjson_doc_get_root(doc));
    yyjson_mut_doc_set_root(mdoc, root);
    yyjson_doc_free(doc);

    inject_websocket_token(mdoc, root, {"/prompt", "/url", "/nested/content/0/text"}, "TOK123");
    inject_expired_at(mdoc, root, {"/expiredAt", "/nested/content/0/text"}, 1700000001);

    yyjson_mut_val* prompt = yyjson_mut_obj_get(root, "prompt");
    CHECK(std::string_view(yyjson_mut_get_str(prompt), yyjson_mut_get_len(prompt)) == "p TOK123 $WEBSOCKET_TOKEN$");
    yyjson_mut_val* exp = yyjson_mut_obj_get(root, "expiredAt");
    CHECK(yyjson_mut_get_sint(exp) == 1700000001);
    yyjson_mut_val* nested_text = yyjson_mut_arr_get(yyjson_mut_obj_get(yyjson_mut_obj_get(root, "nested"), "content"), 0);
    yyjson_mut_val* txt = yyjson_mut_obj_get(nested_text, "text");
    CHECK(std::string_view(yyjson_mut_get_str(txt), yyjson_mut_get_len(txt)) == R"({"url":"ws://y?t=TOK123","expiredAt":1700000001})");

    yyjson_mut_doc_free(mdoc);
}

TEST_CASE("parseDeviceMessage variants") {
    auto msg = parse_device_message(R"({"type":"response","requestId":"1-a","status":200,"headers":{"content-type":"application/json"},"body":{"ok":true}})");
    CHECK(msg.type == ParsedDeviceMessage::Type::Response);
    CHECK(msg.response.status == 200);

    auto bad = parse_device_message(R"({"type":"response","requestId":"1-a","status":199,"headers":{},"body":null})");
    CHECK(bad.type == ParsedDeviceMessage::Type::Invalid);

    auto ack = parse_device_message(R"({"type":"tcpStreamAck","channelId":4,"length":1024})");
    CHECK(ack.type == ParsedDeviceMessage::Type::TcpStreamAck);
    CHECK(ack.tcp_stream_ack.channel_id == 4);
    CHECK(ack.tcp_stream_ack.length == 1024);
}

TEST_CASE("MemoryStore whitelist and file cache") {
    std::string db_file = "/tmp/saihub-unit-crud-" + std::to_string(getpid()) + ".db";
    std::remove(db_file.c_str());
    std::remove((db_file + "-wal").c_str());
    std::remove((db_file + "-shm").c_str());

    MemoryStore empty_store;
    CHECK_FALSE(empty_store.init_db(""));

    {
        MemoryStore store;
        REQUIRE(store.init_db(db_file));
        CHECK_FALSE(store.is_whitelisted("KUCzSr49wPWckUDouJLybJuRYtVpMajPX"));
        auto [created1, ts1] = store.insert_whitelist("KUCzSr49wPWckUDouJLybJuRYtVpMajPX", 1000);
        CHECK(created1);
        auto [created2, ts2] = store.insert_whitelist("KUCzSr49wPWckUDouJLybJuRYtVpMajPX", 2000);
        CHECK_FALSE(created2);
        CHECK(ts2 == ts1);
        CHECK(store.is_whitelisted("KUCzSr49wPWckUDouJLybJuRYtVpMajPX"));
        CHECK(store.list_whitelist(1).total == 1);
        CHECK(store.delete_whitelist("KUCzSr49wPWckUDouJLybJuRYtVpMajPX"));
        CHECK_FALSE(store.is_whitelisted("KUCzSr49wPWckUDouJLybJuRYtVpMajPX"));

        CHECK(store.put_cached_file("1.0.0", "mcp.json", R"({"tools":[]})"));
        CHECK_FALSE(store.put_cached_file("1.0.0", "mcp.json", R"({"tools":[{"name":"a"}]})"));
        CHECK(store.get_cached_file("1.0.0", "mcp.json").has_value());
        CHECK(store.list_cached_files(1).total == 1);
        CHECK(store.delete_cached_file("1.0.0", "mcp.json"));
        CHECK_FALSE(store.get_cached_file("1.0.0", "mcp.json").has_value());
    }

    std::remove(db_file.c_str());
    std::remove((db_file + "-wal").c_str());
    std::remove((db_file + "-shm").c_str());
}

namespace {
std::string gunzip(std::string_view gz) {
    z_stream zs{};
    REQUIRE(inflateInit2(&zs, 15 + 16) == Z_OK);
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(gz.data()));
    zs.avail_in = static_cast<uInt>(gz.size());
    std::string out;
    char buf[4096];
    int rc = Z_OK;
    do {
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        rc = inflate(&zs, 0);
        if (out.size() < zs.total_out) {
            out.append(buf, zs.total_out - out.size());
        }
    } while (rc == Z_OK);
    CHECK(rc == Z_STREAM_END);
    inflateEnd(&zs);
    return out;
}
} // namespace

TEST_CASE("HTML pages are pre-calculated and valid gzip") {
    auto admin_gz = admin_page_gzip();
    CHECK(admin_gz.size() < admin_page_html().size() / 2);
    CHECK(gunzip(admin_gz) == admin_page_html());

    auto echo_gz = oauth_echo_page_gzip();
    CHECK(echo_gz.size() < oauth_echo_page_html().size() / 2);
    CHECK(gunzip(echo_gz) == oauth_echo_page_html());

    auto landing_gz = landing_page_gzip("http://127.0.0.1:8787", "KUCzSr49wPWckUDouJLybJuRYtVpMajPX");
    CHECK(gunzip(landing_gz) == landing_page_html("http://127.0.0.1:8787", "KUCzSr49wPWckUDouJLybJuRYtVpMajPX"));

    auto pairing_gz = pairing_page_gzip("KUCzSr49wPWckUDouJLybJuRYtVpMajPX", "https://client.example/cb", "st1");
    CHECK(gunzip(pairing_gz) == pairing_page_html("KUCzSr49wPWckUDouJLybJuRYtVpMajPX", "https://client.example/cb", "st1"));
}

TEST_CASE("MemoryStore SQLite persistence when DB_PATH is set") {
    std::string db_file = "/tmp/saihub-unit-test-" + std::to_string(getpid()) + ".db";
    std::remove(db_file.c_str());
    std::remove((db_file + "-wal").c_str());
    std::remove((db_file + "-shm").c_str());

    {
        MemoryStore s1;
        REQUIRE(s1.init_db(db_file));
        auto [c1, ts1] = s1.insert_whitelist("KUCzSr49wPWckUDouJLybJuRYtVpMajPX", 12345);
        CHECK(c1);
        CHECK(ts1 == 12345);
        auto [c2, ts2] = s1.insert_whitelist("6fZuj9x4tozLd6CAQ7AhTLd9RYXHqq42x", 67890);
        CHECK(c2);
        CHECK(s1.delete_whitelist("6fZuj9x4tozLd6CAQ7AhTLd9RYXHqq42x"));
        CHECK(s1.put_cached_file("1.2.3", "mcp.json", R"({"tools":[]})"));
        CHECK_FALSE(s1.put_cached_file("1.2.3", "mcp.json", R"({"tools":[{"name":"x"}]})"));
    }

    {
        MemoryStore s2;
        REQUIRE(s2.init_db(db_file));
        CHECK(s2.whitelist_count() == 1);
        CHECK(s2.is_whitelisted("KUCzSr49wPWckUDouJLybJuRYtVpMajPX"));
        CHECK_FALSE(s2.is_whitelisted("6fZuj9x4tozLd6CAQ7AhTLd9RYXHqq42x"));
        auto wl = s2.list_whitelist(1);
        REQUIRE(wl.items.size() == 1);
        CHECK(wl.items[0].created_at == 12345);
        auto fc = s2.get_cached_file("1.2.3", "mcp.json");
        REQUIRE(fc.has_value());
        CHECK(*fc == R"({"tools":[{"name":"x"}]})");
    }

    std::remove(db_file.c_str());
    std::remove((db_file + "-wal").c_str());
    std::remove((db_file + "-shm").c_str());
}
