// High-performance async WebSocket + TCP-stream load & latency generator using libusockets
#include "crypto.hpp"
#include "protocol.hpp"

#include <libusockets.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <yyjson.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace saihub;

namespace {

struct BenchConfig {
    std::string host = "127.0.0.1";
    std::string device_host;
    std::string stream_host;
    int port = 8787;
    int devices = 10000;
    int streams = 50;
    int duration_s = 15;
    int stream_rate_hz = 20;
    int stream_frame_bytes = 512;
    int connect_batch = 50;
    bool echo_mode = false;
    bool echo_seq = false;
    std::string ws_secret = "local-dev-websocket-token-secret";
    std::string admin_token = "local-dev-admin-token";
};

struct DeviceIdentity {
    EVP_PKEY* pkey = nullptr;
    std::string pub_b64;
    std::string digest;
};

enum class ConnRole {
    Device,
    Stream,
};

struct ClientConn {
    ConnRole role = ConnRole::Device;
    int index = 0;
    bool ws_upgraded = false;
    bool authenticated = false;
    bool closed = false;
    size_t rx_msg_bytes = 0;
    int64_t rx_msg_sent_ns = 0;
    std::string digest;
    std::string host;
    std::string ws_path;
    std::string rx_buf;
    std::string ws_payload_buf;
    uint8_t ws_opcode = 0;
    us_socket_t* s = nullptr;
};

struct LatencyStats {
    double min_ms = 0;
    double p50_ms = 0;
    double p95_ms = 0;
    double p99_ms = 0;
    double max_ms = 0;
    double mean_ms = 0;
};

struct BenchState {
    BenchConfig cfg;
    us_loop_t* loop = nullptr;
    us_socket_context_t* ctx = nullptr;
    us_timer_t* ramp_timer = nullptr;
    us_timer_t* ping_timer = nullptr;
    us_timer_t* stream_timer = nullptr;
    us_timer_t* stop_timer = nullptr;

    AesGcmKey ws_key;
    std::vector<DeviceIdentity> identities;
    std::vector<ClientConn*> devices;
    std::vector<ClientConn*> streams;

    std::string stream_client_payload;
    std::string stream_device_payload;

    int next_device_to_connect = 0;
    int next_stream_to_connect = 0;
    int connected_devices = 0;
    int authed_devices = 0;
    int connected_streams = 0;
    int ping_cursor = 0;
    uint64_t pings_sent = 0;
    uint64_t pongs_recv = 0;
    uint64_t stream_frames_sent = 0;
    uint64_t stream_bytes_sent = 0;
    uint64_t stream_frames_recv = 0;
    uint64_t stream_bytes_recv = 0;
    uint64_t connect_errors = 0;
    int64_t start_ms = 0;
    int64_t connect_start_ns = 0;
    int64_t all_ws_upgraded_ns = 0;
    int64_t all_authed_ns = 0;
    int64_t steady_start_ms = 0;
    bool stopping = false;

    std::vector<double> c2d_latencies_ms; // client -> server -> device (first chunk)
    std::vector<double> c2d_full_ms;      // client -> server -> device (full message)
    std::vector<double> d2c_latencies_ms; // device -> server -> client (first chunk)
    std::vector<double> d2c_full_ms;      // device -> server -> client (full message)
    std::vector<double> rtt_latencies_ms; // echo first chunk RTT
    std::vector<double> rtt_full_ms;      // echo full message RTT
    size_t min_rx_frame_bytes = SIZE_MAX;
    size_t max_rx_frame_bytes = 0;
};

BenchState* g_state = nullptr;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void write_all(us_socket_t* s, std::string_view data) {
    if (!s || us_socket_is_closed(0, s)) return;
    us_socket_write(0, s, data.data(), static_cast<int>(data.size()), 0);
}

void send_ws_frame(us_socket_t* s, uint8_t opcode, std::string_view payload) {
    if (!s || us_socket_is_closed(0, s)) return;
    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    std::string frame;
    frame.reserve(14 + payload.size());
    frame.push_back(static_cast<char>(0x80 | (opcode & 0x0F)));
    if (payload.size() < 126) {
        frame.push_back(static_cast<char>(0x80 | payload.size()));
    } else if (payload.size() <= 65535) {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
        frame.push_back(static_cast<char>(payload.size() & 0xFF));
    } else {
        frame.push_back(static_cast<char>(0x80 | 127));
        uint64_t len = payload.size();
        for (int b = 7; b >= 0; --b) {
            frame.push_back(static_cast<char>((len >> (b * 8)) & 0xFF));
        }
    }
    frame.append(reinterpret_cast<const char*>(mask), 4);
    size_t hdr_len = frame.size();
    frame.resize(hdr_len + payload.size());
    for (size_t i = 0; i < payload.size(); ++i) {
        frame[hdr_len + i] = static_cast<char>(static_cast<uint8_t>(payload[i]) ^ mask[i & 3]);
    }
    write_all(s, frame);
}

std::string sign_challenge_fast(EVP_PKEY* pkey, std::string_view challenge_b64) {
    std::vector<uint8_t> ch;
    base64url_to_bytes(challenge_b64, ch);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, pkey);
    EVP_DigestSignUpdate(ctx, kAuthDomainPrefix.data(), kAuthDomainPrefix.size());
    EVP_DigestSignUpdate(ctx, ch.data(), ch.size());
    size_t sig_len = 0;
    EVP_DigestSignFinal(ctx, nullptr, &sig_len);
    std::vector<uint8_t> der(sig_len);
    EVP_DigestSignFinal(ctx, der.data(), &sig_len);
    EVP_MD_CTX_free(ctx);

    const unsigned char* p = der.data();
    ECDSA_SIG* esig = d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(sig_len));
    const BIGNUM *r = nullptr, *s = nullptr;
    ECDSA_SIG_get0(esig, &r, &s);
    uint8_t raw[64];
    BN_bn2binpad(r, raw, 32);
    BN_bn2binpad(s, raw + 32, 32);
    ECDSA_SIG_free(esig);
    return bytes_to_base64url(raw);
}

void on_stop_timer(us_timer_t*);

void handle_ws_message(ClientConn* c, uint8_t opcode, std::string_view payload) {
    size_t msg_target = static_cast<size_t>(g_state->cfg.stream_frame_bytes);
    if (c->role == ConnRole::Stream) {
        if (opcode == 2) {
            g_state->stream_frames_recv++;
            g_state->stream_bytes_recv += payload.size();
            g_state->min_rx_frame_bytes = std::min(g_state->min_rx_frame_bytes, payload.size());
            g_state->max_rx_frame_bytes = std::max(g_state->max_rx_frame_bytes, payload.size());
            int64_t now = now_ns();
            if (c->rx_msg_bytes == 0 && payload.size() >= sizeof(int64_t)) {
                std::memcpy(&c->rx_msg_sent_ns, payload.data(), sizeof(int64_t));
                double lat_ms = static_cast<double>(now - c->rx_msg_sent_ns) / 1e6;
                if (g_state->cfg.echo_mode) g_state->rtt_latencies_ms.push_back(lat_ms);
                else g_state->d2c_latencies_ms.push_back(lat_ms);
            }
            c->rx_msg_bytes += payload.size();
            if (c->rx_msg_bytes >= msg_target) {
                double full_ms = static_cast<double>(now - c->rx_msg_sent_ns) / 1e6;
                if (g_state->cfg.echo_mode) {
                    g_state->rtt_full_ms.push_back(full_ms);
                    if (g_state->cfg.echo_seq && !g_state->stopping && c->s) {
                        int64_t ts = now_ns();
                        std::memcpy(g_state->stream_client_payload.data(), &ts, sizeof(int64_t));
                        send_ws_frame(c->s, 2, g_state->stream_client_payload);
                        g_state->stream_frames_sent++;
                        g_state->stream_bytes_sent += g_state->stream_client_payload.size();
                    }
                } else {
                    g_state->d2c_full_ms.push_back(full_ms);
                }
                c->rx_msg_bytes -= msg_target;
            }
        }
        return;
    }

    if (opcode == 2) {
        g_state->stream_frames_recv++;
        if (payload.size() > 1) {
            size_t data_len = payload.size() - 1;
            g_state->stream_bytes_recv += data_len;
            g_state->min_rx_frame_bytes = std::min(g_state->min_rx_frame_bytes, data_len);
            g_state->max_rx_frame_bytes = std::max(g_state->max_rx_frame_bytes, data_len);
            int64_t now = now_ns();
            if (c->rx_msg_bytes == 0 && data_len >= sizeof(int64_t)) {
                std::memcpy(&c->rx_msg_sent_ns, payload.data() + 1, sizeof(int64_t));
                g_state->c2d_latencies_ms.push_back(static_cast<double>(now - c->rx_msg_sent_ns) / 1e6);
            }
            c->rx_msg_bytes += data_len;
            if (c->rx_msg_bytes >= msg_target) {
                g_state->c2d_full_ms.push_back(static_cast<double>(now - c->rx_msg_sent_ns) / 1e6);
                c->rx_msg_bytes -= msg_target;
            }
            if (g_state->cfg.echo_mode) {
                send_ws_frame(c->s, 2, payload);
                g_state->stream_frames_sent++;
                g_state->stream_bytes_sent += data_len;
            }
            char ack_buf[128];
            int n = std::snprintf(ack_buf, sizeof(ack_buf),
                                  R"({"type":"tcpStreamAck","channelId":1,"length":%zu})",
                                  data_len);
            send_ws_frame(c->s, 1, std::string_view(ack_buf, static_cast<size_t>(n)));
        }
        return;
    }
    if (opcode != 1) return;
    if (payload == "pong") {
        g_state->pongs_recv++;
        return;
    }

    yyjson_doc* doc = yyjson_read(payload.data(), payload.size(), 0);
    if (!doc) return;
    yyjson_val* root = yyjson_doc_get_root(doc);
    yyjson_val* type_v = yyjson_obj_get(root, "type");
    std::string_view type = yyjson_is_str(type_v)
        ? std::string_view(yyjson_get_str(type_v), yyjson_get_len(type_v))
        : std::string_view{};

    if (type == "authRequest") {
        yyjson_val* ch_v = yyjson_obj_get(root, "challenge");
        std::string_view ch(yyjson_get_str(ch_v), yyjson_get_len(ch_v));
        const auto& id = g_state->identities[static_cast<size_t>(c->index)];
        std::string sig = sign_challenge_fast(id.pkey, ch);

        yyjson_mut_doc* mdoc = yyjson_mut_doc_new(nullptr);
        yyjson_mut_val* mroot = yyjson_mut_obj(mdoc);
        yyjson_mut_doc_set_root(mdoc, mroot);
        yyjson_mut_obj_add_str(mdoc, mroot, "type", "authResponse");
        yyjson_mut_obj_add_strn(mdoc, mroot, "devicePublicKey", id.pub_b64.data(), id.pub_b64.size());
        yyjson_mut_obj_add_strn(mdoc, mroot, "devicePublicKeyDigest", id.digest.data(), id.digest.size());
        yyjson_mut_obj_add_str(mdoc, mroot, "version", "bench-1.0.0");
        yyjson_mut_obj_add_strn(mdoc, mroot, "response", sig.data(), sig.size());
        size_t len = 0;
        char* json = yyjson_mut_write(mdoc, 0, &len);
        yyjson_mut_doc_free(mdoc);
        send_ws_frame(c->s, 1, std::string_view(json, len));
        free(json);
    } else if (type == "authResult") {
        yyjson_val* ok_v = yyjson_obj_get(root, "success");
        if (yyjson_is_bool(ok_v) && yyjson_get_bool(ok_v)) {
            c->authenticated = true;
            g_state->authed_devices++;
            if (g_state->authed_devices == g_state->cfg.devices && g_state->all_authed_ns == 0) {
                g_state->all_authed_ns = now_ns();
                double sec = static_cast<double>(g_state->all_authed_ns - g_state->connect_start_ns) / 1e9;
                double ws_sec = static_cast<double>(g_state->all_ws_upgraded_ns - g_state->connect_start_ns) / 1e9;
                std::cout << std::fixed << std::setprecision(3)
                          << "CONNECT_ESTABLISH devices=" << g_state->cfg.devices
                          << " ws_upgrade_s=" << ws_sec
                          << " ws_rate_per_s=" << (ws_sec > 0 ? g_state->cfg.devices / ws_sec : 0.0)
                          << " full_auth_s=" << sec
                          << " auth_rate_per_s=" << (sec > 0 ? g_state->cfg.devices / sec : 0.0)
                          << std::endl;
                if (g_state->cfg.streams == 0 && g_state->steady_start_ms == 0) {
                    g_state->steady_start_ms = now_ms();
                    std::cout << "STEADY_START devices=" << g_state->authed_devices << " streams=0" << std::endl;
                    us_timer_set(g_state->stop_timer, on_stop_timer, g_state->cfg.duration_s * 1000, 0);
                }
            }
        }
    } else if (type == "tcpStreamOpenRequest") {
        yyjson_val* req_id_v = yyjson_obj_get(root, "requestId");
        std::string_view req_id(yyjson_get_str(req_id_v), yyjson_get_len(req_id_v));
        yyjson_mut_doc* mdoc = yyjson_mut_doc_new(nullptr);
        yyjson_mut_val* mroot = yyjson_mut_obj(mdoc);
        yyjson_mut_doc_set_root(mdoc, mroot);
        yyjson_mut_obj_add_str(mdoc, mroot, "type", "tcpStreamOpenResponse");
        yyjson_mut_obj_add_strn(mdoc, mroot, "requestId", req_id.data(), req_id.size());
        yyjson_mut_obj_add_bool(mdoc, mroot, "success", true);
        yyjson_mut_obj_add_int(mdoc, mroot, "channelId", 1);
        yyjson_mut_obj_add_int(mdoc, mroot, "byteCredits", 1048576);
        size_t len = 0;
        char* json = yyjson_mut_write(mdoc, 0, &len);
        yyjson_mut_doc_free(mdoc);
        send_ws_frame(c->s, 1, std::string_view(json, len));
        free(json);
    }
    yyjson_doc_free(doc);
}

us_socket_t* on_open(us_socket_t* s, int, char*, int) {
    auto* c = *reinterpret_cast<ClientConn**>(us_socket_ext(0, s));
    c->s = s;
    std::string req = "GET " + c->ws_path + " HTTP/1.1\r\n"
                      "Host: " + c->host + ":" + std::to_string(g_state->cfg.port) + "\r\n"
                      "Upgrade: websocket\r\n"
                      "Connection: Upgrade\r\n"
                      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                      "Sec-WebSocket-Version: 13\r\n\r\n";
    write_all(s, req);
    return s;
}

us_socket_t* on_data(us_socket_t* s, char* data, int length) {
    auto* c = *reinterpret_cast<ClientConn**>(us_socket_ext(0, s));
    c->rx_buf.append(data, static_cast<size_t>(length));

    if (!c->ws_upgraded) {
        size_t hdr_end = c->rx_buf.find("\r\n\r\n");
        if (hdr_end == std::string::npos) return s;
        if (c->rx_buf.find("101 ") == std::string::npos) {
            us_socket_close(0, s, 0, nullptr);
            return s;
        }
        c->rx_buf.erase(0, hdr_end + 4);
        c->ws_upgraded = true;
        if (c->role == ConnRole::Device) {
            g_state->connected_devices++;
            if (g_state->connected_devices == g_state->cfg.devices && g_state->all_ws_upgraded_ns == 0) {
                g_state->all_ws_upgraded_ns = now_ns();
            }
        } else {
            g_state->connected_streams++;
            if (g_state->connected_streams == g_state->cfg.streams && g_state->steady_start_ms == 0) {
                g_state->steady_start_ms = now_ms();
                std::cout << "STEADY_START devices=" << g_state->authed_devices
                          << " streams=" << g_state->connected_streams << std::endl;
                us_timer_set(g_state->stop_timer, on_stop_timer, g_state->cfg.duration_s * 1000, 0);
                if (g_state->cfg.echo_mode && g_state->cfg.echo_seq) {
                    int64_t ts = now_ns();
                    std::memcpy(g_state->stream_client_payload.data(), &ts, sizeof(int64_t));
                    for (auto* sc : g_state->streams) {
                        if (sc->ws_upgraded && sc->s) {
                            send_ws_frame(sc->s, 2, g_state->stream_client_payload);
                            g_state->stream_frames_sent++;
                            g_state->stream_bytes_sent += g_state->stream_client_payload.size();
                        }
                    }
                }
            }
        }
    }

    size_t pos = 0;
    while (pos + 2 <= c->rx_buf.size()) {
        uint8_t b0 = static_cast<uint8_t>(c->rx_buf[pos]);
        uint8_t b1 = static_cast<uint8_t>(c->rx_buf[pos + 1]);
        bool fin = (b0 & 0x80) != 0;
        uint8_t op = b0 & 0x0F;
        uint64_t plen = b1 & 0x7F;
        size_t hdr = 2;
        if (plen == 126) {
            if (pos + 4 > c->rx_buf.size()) break;
            plen = (uint64_t(static_cast<uint8_t>(c->rx_buf[pos + 2])) << 8) |
                    uint64_t(static_cast<uint8_t>(c->rx_buf[pos + 3]));
            hdr = 4;
        } else if (plen == 127) {
            if (pos + 10 > c->rx_buf.size()) break;
            plen = 0;
            for (int i = 0; i < 8; ++i) {
                plen = (plen << 8) | uint64_t(static_cast<uint8_t>(c->rx_buf[pos + 2 + i]));
            }
            hdr = 10;
        }
        if (pos + hdr + plen > c->rx_buf.size()) break;
        std::string_view body(c->rx_buf.data() + pos + hdr, plen);
        pos += hdr + plen;

        if (op == 0x8) {
            us_socket_close(0, s, 0, nullptr);
            return s;
        } else if (op == 0x9) {
            send_ws_frame(s, 0xA, body);
        } else if (op == 0xA) {
        } else {
            if (op != 0) {
                c->ws_opcode = op;
                c->ws_payload_buf.assign(body);
            } else {
                c->ws_payload_buf.append(body);
            }
            if (fin) {
                handle_ws_message(c, c->ws_opcode, c->ws_payload_buf);
                c->ws_payload_buf.clear();
            }
        }
    }
    if (pos > 0) {
        c->rx_buf.erase(0, pos);
    }
    return s;
}

us_socket_t* on_close(us_socket_t* s, int, void*) {
    auto* c = *reinterpret_cast<ClientConn**>(us_socket_ext(0, s));
    if (c) {
        c->closed = true;
        c->s = nullptr;
    }
    return s;
}

us_socket_t* on_writable(us_socket_t* s) { return s; }
us_socket_t* on_timeout(us_socket_t* s) { return s; }
us_socket_t* on_end(us_socket_t* s) { return us_socket_close(0, s, 0, nullptr); }
us_socket_t* on_connect_error(us_socket_t* s, int) {
    auto* c = *reinterpret_cast<ClientConn**>(us_socket_ext(0, s));
    if (c) {
        c->closed = true;
        c->s = nullptr;
    }
    g_state->connect_errors++;
    return s;
}

bool http_seed_whitelist(const BenchConfig& cfg, const std::vector<DeviceIdentity>& ids) {
    const std::string& target_host = cfg.stream_host;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(cfg.port));
    inet_pton(AF_INET, target_host.c_str(), &addr.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return false;
    }

    std::string out;
    std::string recv_buf;
    char tmp[16384];
    size_t sent_cnt = 0;
    size_t resp_cnt = 0;
    for (size_t i = 0; i < ids.size(); ++i) {
        std::string body = "{\"digest\":\"" + ids[i].digest + "\"}";
        out += "POST /admin/devices/whitelist HTTP/1.1\r\n";
        out += "Host: " + target_host + "\r\n";
        out += "Authorization: Bearer " + cfg.admin_token + "\r\n";
        out += "Content-Type: application/json\r\n";
        out += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
        out += body;
        sent_cnt++;

        if (out.size() >= 32768 || i + 1 == ids.size()) {
            size_t off = 0;
            while (off < out.size()) {
                ssize_t w = send(fd, out.data() + off, out.size() - off, 0);
                if (w <= 0) { close(fd); return false; }
                off += static_cast<size_t>(w);
            }
            out.clear();
            while (resp_cnt < sent_cnt) {
                ssize_t r = recv(fd, tmp, sizeof(tmp), 0);
                if (r <= 0) { close(fd); return false; }
                recv_buf.append(tmp, static_cast<size_t>(r));
                while (true) {
                    size_t hdr_end = recv_buf.find("\r\n\r\n");
                    if (hdr_end == std::string::npos) break;
                    size_t cl_pos = recv_buf.find("Content-Length: ");
                    size_t cl = 0;
                    if (cl_pos != std::string::npos && cl_pos < hdr_end) {
                        cl = std::strtoul(recv_buf.c_str() + cl_pos + 16, nullptr, 10);
                    }
                    if (recv_buf.size() < hdr_end + 4 + cl) break;
                    recv_buf.erase(0, hdr_end + 4 + cl);
                    resp_cnt++;
                }
            }
        }
    }
    close(fd);
    return resp_cnt == ids.size();
}

void start_connect(ClientConn* c) {
    us_socket_t* s = us_socket_context_connect(
        0, g_state->ctx, c->host.c_str(), g_state->cfg.port, nullptr, 0, sizeof(ClientConn*));
    if (!s) {
        g_state->connect_errors++;
        return;
    }
    *reinterpret_cast<ClientConn**>(us_socket_ext(0, s)) = c;
    c->s = s;
}

void on_ramp_timer(us_timer_t*) {
    auto* st = g_state;
    int started = 0;
    while (st->next_device_to_connect < st->cfg.devices && started < st->cfg.connect_batch) {
        start_connect(st->devices[static_cast<size_t>(st->next_device_to_connect)]);
        st->next_device_to_connect++;
        started++;
    }
    if (st->next_device_to_connect >= st->cfg.devices &&
        st->authed_devices >= st->cfg.devices &&
        st->next_stream_to_connect < st->cfg.streams) {
        while (st->next_stream_to_connect < st->cfg.streams) {
            start_connect(st->streams[static_cast<size_t>(st->next_stream_to_connect)]);
            st->next_stream_to_connect++;
        }
    }
    if (st->next_device_to_connect >= st->cfg.devices &&
        st->next_stream_to_connect >= st->cfg.streams &&
        st->connected_streams >= st->cfg.streams) {
        if (st->ramp_timer) {
            us_timer_close(st->ramp_timer);
            st->ramp_timer = nullptr;
        }
    }
}

void on_ping_timer(us_timer_t*) {
    auto* st = g_state;
    if (st->devices.empty()) return;
    int per_tick = std::max(1, (st->cfg.devices + 199) / 200);
    for (int i = 0; i < per_tick; ++i) {
        int idx = st->ping_cursor % st->cfg.devices;
        st->ping_cursor++;
        auto* c = st->devices[static_cast<size_t>(idx)];
        if (c->ws_upgraded && c->authenticated && c->s) {
            send_ws_frame(c->s, 1, "ping");
            st->pings_sent++;
        }
    }
}

void on_stream_timer(us_timer_t*) {
    auto* st = g_state;
    if (st->connected_streams < st->cfg.streams) return;
    if (st->cfg.echo_mode && st->cfg.echo_seq) return;
    int64_t ts = now_ns();
    std::memcpy(st->stream_client_payload.data(), &ts, sizeof(int64_t));
    std::memcpy(st->stream_device_payload.data() + 1, &ts, sizeof(int64_t));

    for (int i = 0; i < st->cfg.streams; ++i) {
        auto* sc = st->streams[static_cast<size_t>(i)];
        auto* dc = st->devices[static_cast<size_t>(i)];
        if (sc->ws_upgraded && sc->s) {
            send_ws_frame(sc->s, 2, st->stream_client_payload);
            st->stream_frames_sent++;
            st->stream_bytes_sent += st->stream_client_payload.size();
        }
        if (!st->cfg.echo_mode && dc->ws_upgraded && dc->authenticated && dc->s) {
            send_ws_frame(dc->s, 2, st->stream_device_payload);
            st->stream_frames_sent++;
            st->stream_bytes_sent += st->stream_client_payload.size();
        }
    }
}

LatencyStats summarize_latencies(std::vector<double>& samples) {
    LatencyStats st;
    if (samples.empty()) return st;
    std::sort(samples.begin(), samples.end());
    double sum = 0.0;
    for (double x : samples) sum += x;
    auto pct = [&](double p) {
        size_t idx = static_cast<size_t>(p * static_cast<double>(samples.size() - 1));
        return samples[idx];
    };
    st.min_ms = samples.front();
    st.p50_ms = pct(0.50);
    st.p95_ms = pct(0.95);
    st.p99_ms = pct(0.99);
    st.max_ms = samples.back();
    st.mean_ms = sum / static_cast<double>(samples.size());
    return st;
}

void on_stop_timer(us_timer_t*) {
    auto* st = g_state;
    st->stopping = true;
    int64_t elapsed_ms = now_ms() - st->start_ms;
    std::cout << "BENCH_SUMMARY"
              << " elapsed_ms=" << elapsed_ms
              << " devices=" << st->cfg.devices
              << " authed=" << st->authed_devices
              << " streams=" << st->connected_streams
              << " pings_sent=" << st->pings_sent
              << " pongs_recv=" << st->pongs_recv
              << " stream_frames_sent=" << st->stream_frames_sent
              << " stream_frames_recv=" << st->stream_frames_recv
              << " stream_bytes_sent=" << st->stream_bytes_sent
              << " stream_bytes_recv=" << st->stream_bytes_recv
              << " connect_errors=" << st->connect_errors
              << std::endl;

    auto c2d = summarize_latencies(st->c2d_latencies_ms);
    auto c2d_full = summarize_latencies(st->c2d_full_ms);
    auto d2c = summarize_latencies(st->d2c_latencies_ms);
    auto d2c_full = summarize_latencies(st->d2c_full_ms);
    auto rtt = summarize_latencies(st->rtt_latencies_ms);
    auto rtt_full = summarize_latencies(st->rtt_full_ms);
    size_t min_fb = st->min_rx_frame_bytes == SIZE_MAX ? 0 : st->min_rx_frame_bytes;
    double avg_fb = st->stream_frames_recv ? static_cast<double>(st->stream_bytes_recv) / static_cast<double>(st->stream_frames_recv) : 0.0;
    std::cout << std::fixed << std::setprecision(3)
              << "RX_FRAME_BYTES min=" << min_fb << " max=" << st->max_rx_frame_bytes << " avg=" << avg_fb << "\n"
              << "LATENCY_C2D_FIRST samples=" << st->c2d_latencies_ms.size()
              << " min_ms=" << c2d.min_ms << " p50_ms=" << c2d.p50_ms
              << " p95_ms=" << c2d.p95_ms << " max_ms=" << c2d.max_ms << " mean_ms=" << c2d.mean_ms << "\n"
              << "LATENCY_C2D_FULL  samples=" << st->c2d_full_ms.size()
              << " min_ms=" << c2d_full.min_ms << " p50_ms=" << c2d_full.p50_ms
              << " p95_ms=" << c2d_full.p95_ms << " max_ms=" << c2d_full.max_ms << " mean_ms=" << c2d_full.mean_ms << "\n"
              << "LATENCY_D2C_FIRST samples=" << st->d2c_latencies_ms.size()
              << " min_ms=" << d2c.min_ms << " p50_ms=" << d2c.p50_ms
              << " p95_ms=" << d2c.p95_ms << " max_ms=" << d2c.max_ms << " mean_ms=" << d2c.mean_ms << "\n"
              << "LATENCY_D2C_FULL  samples=" << st->d2c_full_ms.size()
              << " min_ms=" << d2c_full.min_ms << " p50_ms=" << d2c_full.p50_ms
              << " p95_ms=" << d2c_full.p95_ms << " max_ms=" << d2c_full.max_ms << " mean_ms=" << d2c_full.mean_ms << "\n"
              << "LATENCY_ECHO_FIRST samples=" << st->rtt_latencies_ms.size()
              << " min_ms=" << rtt.min_ms << " p50_ms=" << rtt.p50_ms
              << " p95_ms=" << rtt.p95_ms << " max_ms=" << rtt.max_ms << " mean_ms=" << rtt.mean_ms << "\n"
              << "LATENCY_ECHO_FULL  samples=" << st->rtt_full_ms.size()
              << " min_ms=" << rtt_full.min_ms << " p50_ms=" << rtt_full.p50_ms
              << " p95_ms=" << rtt_full.p95_ms << " max_ms=" << rtt_full.max_ms << " mean_ms=" << rtt_full.mean_ms
              << std::endl;

    if (st->ramp_timer) { us_timer_close(st->ramp_timer); st->ramp_timer = nullptr; }
    if (st->ping_timer) { us_timer_close(st->ping_timer); st->ping_timer = nullptr; }
    if (st->stream_timer) { us_timer_close(st->stream_timer); st->stream_timer = nullptr; }
    if (st->stop_timer) { us_timer_close(st->stop_timer); st->stop_timer = nullptr; }
    us_socket_context_close(0, st->ctx);
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    BenchState state;
    g_state = &state;

    for (int i = 1; i < argc; ++i) {
        std::string_view a(argv[i]);
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
        if (a == "--host") { if (auto* v = next()) state.cfg.host = v; }
        else if (a == "--device-host") { if (auto* v = next()) state.cfg.device_host = v; }
        else if (a == "--stream-host") { if (auto* v = next()) state.cfg.stream_host = v; }
        else if (a == "--port") { if (auto* v = next()) state.cfg.port = std::atoi(v); }
        else if (a == "--devices") { if (auto* v = next()) state.cfg.devices = std::atoi(v); }
        else if (a == "--streams") { if (auto* v = next()) state.cfg.streams = std::atoi(v); }
        else if (a == "--duration") { if (auto* v = next()) state.cfg.duration_s = std::atoi(v); }
        else if (a == "--stream-hz") { if (auto* v = next()) state.cfg.stream_rate_hz = std::atoi(v); }
        else if (a == "--stream-bytes") { if (auto* v = next()) state.cfg.stream_frame_bytes = std::atoi(v); }
        else if (a == "--batch") { if (auto* v = next()) state.cfg.connect_batch = std::atoi(v); }
        else if (a == "--echo") { state.cfg.echo_mode = true; }
        else if (a == "--echo-seq") { state.cfg.echo_mode = true; state.cfg.echo_seq = true; }
        else if (a == "--admin-token") { if (auto* v = next()) state.cfg.admin_token = v; }
        else if (a == "--ws-secret") { if (auto* v = next()) state.cfg.ws_secret = v; }
    }
    if (state.cfg.device_host.empty()) state.cfg.device_host = state.cfg.host;
    if (state.cfg.stream_host.empty()) state.cfg.stream_host = state.cfg.host;
    if (state.cfg.stream_frame_bytes < static_cast<int>(sizeof(int64_t))) {
        state.cfg.stream_frame_bytes = static_cast<int>(sizeof(int64_t));
    }

    state.ws_key = AesGcmKey(state.cfg.ws_secret);
    state.stream_client_payload.assign(static_cast<size_t>(state.cfg.stream_frame_bytes), 'A');
    state.stream_device_payload.push_back(static_cast<char>(1)); // channelId = 1
    state.stream_device_payload.append(static_cast<size_t>(state.cfg.stream_frame_bytes), 'B');

    std::cout << "Generating " << state.cfg.devices << " P-256 device identities..." << std::endl;
    state.identities.resize(static_cast<size_t>(state.cfg.devices));
    for (int i = 0; i < state.cfg.devices; ++i) {
        auto& id = state.identities[static_cast<size_t>(i)];
        id.pkey = EVP_EC_gen("P-256");
        size_t len = 0;
        EVP_PKEY_get_octet_string_param(id.pkey, "pub", nullptr, 0, &len);
        std::vector<uint8_t> pub_raw(len);
        EVP_PKEY_get_octet_string_param(id.pkey, "pub", pub_raw.data(), len, &len);
        id.pub_b64 = bytes_to_base64url(pub_raw);
        id.digest = *public_key_digest(pub_raw);
    }

    std::cout << "Whitelisting " << state.cfg.devices << " digests..." << std::endl;
    if (!http_seed_whitelist(state.cfg, state.identities)) {
        std::cerr << "Failed to seed whitelist\n";
        return 1;
    }

    state.devices.reserve(static_cast<size_t>(state.cfg.devices));
    for (int i = 0; i < state.cfg.devices; ++i) {
        auto* c = new ClientConn();
        c->role = ConnRole::Device;
        c->index = i;
        c->digest = state.identities[static_cast<size_t>(i)].digest;
        c->host = state.cfg.device_host;
        c->ws_path = "/cloud/device/" + c->digest;
        state.devices.push_back(c);
    }

    int64_t expire_at = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count() +
                        3600;
    state.streams.reserve(static_cast<size_t>(state.cfg.streams));
    for (int i = 0; i < state.cfg.streams; ++i) {
        auto* c = new ClientConn();
        c->role = ConnRole::Stream;
        c->index = i;
        c->digest = state.identities[static_cast<size_t>(i)].digest;
        c->host = state.cfg.stream_host;
        auto tok = seal_websocket_token(state.ws_key, WebsocketTokenPayload{
            expire_at,
            c->digest,
            "/tcp-stream/dap",
            "bench-grant-secret-01234567890",
        });
        c->ws_path = "/device/" + c->digest + "/tcp-stream/dap?token=" + *tok;
        state.streams.push_back(c);
    }

    state.loop = us_create_loop(nullptr, [](us_loop_t*) {}, [](us_loop_t*) {}, [](us_loop_t*) {}, 0);
    us_socket_context_options_t opts{};
    state.ctx = us_create_socket_context(0, state.loop, 0, opts);
    us_socket_context_on_open(0, state.ctx, on_open);
    us_socket_context_on_data(0, state.ctx, on_data);
    us_socket_context_on_writable(0, state.ctx, on_writable);
    us_socket_context_on_close(0, state.ctx, on_close);
    us_socket_context_on_timeout(0, state.ctx, on_timeout);
    us_socket_context_on_end(0, state.ctx, on_end);
    us_socket_context_on_connect_error(0, state.ctx, on_connect_error);

    state.start_ms = now_ms();
    state.connect_start_ns = now_ns();
    state.ramp_timer = us_create_timer(state.loop, 0, 0);
    us_timer_set(state.ramp_timer, on_ramp_timer, 20, 20);

    state.ping_timer = us_create_timer(state.loop, 0, 0);
    us_timer_set(state.ping_timer, on_ping_timer, 100, 100);

    int stream_period_ms = std::max(1, 1000 / std::max(1, state.cfg.stream_rate_hz));
    state.stream_timer = us_create_timer(state.loop, 0, 0);
    us_timer_set(state.stream_timer, on_stream_timer, stream_period_ms, stream_period_ms);

    state.stop_timer = us_create_timer(state.loop, 0, 0);
    us_timer_set(state.stop_timer, on_stop_timer, (state.cfg.duration_s + 60) * 1000, 0);

    us_loop_run(state.loop);
    return 0;
}
