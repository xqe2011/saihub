#pragma once

#include "config.hpp"
#include "crypto.hpp"
#include "protocol.hpp"
#include "store.hpp"
#include "timers.hpp"

#include <uwebsockets/App.h>
#include <ankerl/unordered_dense.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace saihub {

inline constexpr size_t kMaxPendingRequests = 8;
inline constexpr size_t kMaxPairingWaiters = 8;
inline constexpr int64_t kHeartbeatTimeoutMs = 33000;
inline constexpr size_t kStreamChunkBytes = 1024;

struct DeviceRuntime;
struct DeviceSession;
struct TcpStreamSession;

class DeviceManager;

struct DeviceWsData {
    DeviceSession* session = nullptr;
    DeviceManager* mgr = nullptr;
};

struct TcpStreamWsData {
    TcpStreamSession* stream = nullptr;
    DeviceManager* mgr = nullptr;
};

using DeviceWS = uWS::WebSocket<false, true, DeviceWsData>;
using TcpStreamWS = uWS::WebSocket<false, true, TcpStreamWsData>;
using HttpRes = uWS::HttpResponse<false>;

struct DeviceSession {
    DeviceRuntime* device = nullptr;
    DeviceWS* ws = nullptr;
    bool authenticated = false;
    std::vector<uint8_t> challenge;
    std::string version;
    int64_t connected_at_ms = 0;
    int64_t last_seen_ms = 0;
    TimerNode auth_timer;
    TimerNode heartbeat_timer;
    bool in_binary_frame = false;
    int binary_channel_id = -1;
    std::string text_frag_buf;
    std::string control_frag_buf;
};

struct HeadroomRequest {
    std::string request_id;
    std::string serialized_message;
    std::string grant_secret;
    HttpRes* res = nullptr;
    bool aborted = false;
};

struct PendingRequest {
    DeviceRuntime* device = nullptr;
    std::string request_id;
    std::string grant_secret;
    HttpRes* res = nullptr;
    bool completed = false;
    TimerNode timeout_timer;
};

struct PairingState {
    std::string session_token;
    int64_t expired_at_s = 0;
    std::string grant_secret;
};

struct PairingSessionWaiter {
    DeviceRuntime* device = nullptr;
    HttpRes* res = nullptr;
    bool completed = false;
    TimerNode timeout_timer;
};

struct PairingTokenWaiter {
    DeviceRuntime* device = nullptr;
    HttpRes* res = nullptr;
    int64_t deadline_ms = 0;
    bool completed = false;
};

struct StreamWaiter {
    DeviceRuntime* device = nullptr;
    std::string request_id;
    std::string service;
    std::string grant_secret;
    HttpRes* res = nullptr;
    us_socket_context_t* ws_ctx = nullptr;
    std::string sec_ws_key;
    std::string sec_ws_protocol;
    std::string sec_ws_extensions;
    bool completed = false;
    TimerNode timeout_timer;
};

struct TcpStreamSession {
    DeviceRuntime* device = nullptr;
    TcpStreamWS* ws = nullptr;
    int channel_id = 0;
    std::string service;
    std::string grant_secret;
    int byte_credits = 0;
    int capacity = 0;
    std::string pending;
    size_t pending_offset = 0;
    int64_t connected_at_ms = 0;
    int64_t last_seen_ms = 0;
    TimerNode heartbeat_timer;
    bool in_binary_frame = false;
    std::string text_frag_buf;
    std::string control_frag_buf;
};

struct DeviceRuntime {
    std::string digest;
    std::unique_ptr<DeviceSession> ws_session;
    std::vector<PendingRequest*> pending;
    std::deque<HeadroomRequest> headroom;
    std::vector<StreamWaiter*> stream_pending;
    std::vector<std::unique_ptr<TcpStreamSession>> streams;
    std::optional<PairingState> pairing;
    PairingSessionWaiter* pairing_session_waiter = nullptr;
    std::vector<PairingTokenWaiter*> pairing_token_waiters;
    uint32_t seq = 0;
};

class DeviceManager {
public:
    DeviceManager(const Config& cfg, MemoryStore& store, const AesGcmKey& routing_key, const AesGcmKey& ws_key);
    ~DeviceManager();

    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;

    void attach_loop(us_loop_t* loop);
    void close_timer();

    // HTTP / WS entry points
    bool is_device_connected(std::string_view digest);
    bool is_device_online(std::string_view digest);
    void disconnect_device(std::string_view digest, int code, std::string_view reason);

    // Device WS lifecycle
    void on_device_ws_open(DeviceWS* ws, std::string_view digest);
    void on_device_ws_message(DeviceWS* ws, std::string_view msg, uWS::OpCode op);
    void on_device_binary_fragment(DeviceWS* ws, std::string_view chunk, unsigned int remaining_bytes, bool fin);
    void on_device_ws_close(DeviceWS* ws);

    // HTTP Proxy
    void handle_proxy(
        HttpRes* res,
        std::string_view digest,
        std::string_view method,
        std::string_view path,
        std::vector<std::pair<std::string, std::string>> headers,
        std::string_view body_str,
        bool has_content_type,
        std::string_view content_type,
        std::string_view grant_secret
    );

    // Pairing
    void handle_pairing_session(HttpRes* res, std::string_view digest, std::string_view name);
    void handle_pairing_token(HttpRes* res, std::string_view digest, std::string_view session_token);

    // TCP stream WS
    void handle_tcp_stream_upgrade(
        HttpRes* res,
        us_socket_context_t* ws_ctx,
        std::string_view digest,
        std::string_view service,
        std::string_view grant_secret,
        std::string_view sec_ws_key,
        std::string_view sec_ws_protocol,
        std::string_view sec_ws_extensions
    );
    void on_tcp_stream_ws_open(TcpStreamWS* ws);
    void on_tcp_stream_ws_message(TcpStreamWS* ws, std::string_view msg, uWS::OpCode op);
    void on_tcp_stream_binary_fragment(TcpStreamWS* ws, std::string_view chunk, unsigned int remaining_bytes, bool fin);
    void on_tcp_stream_ws_close(TcpStreamWS* ws);

    // Timer processing (also called directly in unit tests)
    void process_timers(int64_t now_ms);

    size_t connected_devices_count() const noexcept { return connected_devices_count_; }
    size_t authenticated_devices_count() const noexcept { return authenticated_devices_count_; }
    size_t active_streams_count() const noexcept { return active_streams_count_; }

    static int64_t now_ms();

private:
    DeviceRuntime* find_device(std::string_view digest);
    DeviceRuntime* get_or_create_device(std::string_view digest);
    void maybe_gc_device(DeviceRuntime* dev);

    void validate_device_ws(DeviceRuntime* dev, int64_t now);
    void fail_auth(DeviceSession* sess, std::string_view reason);
    void close_device_session(DeviceRuntime* dev, int code, std::string_view reason);
    void reject_all_pending(DeviceRuntime* dev, std::string_view reason);
    void drain_headroom(DeviceRuntime* dev);

    void handle_auth_response(DeviceSession* sess, const AuthResponseMsg& msg);
    void handle_device_response(DeviceRuntime* dev, const ResponseMsg& msg);
    void handle_pairing_session_response(DeviceRuntime* dev, const PairingSessionResponseMsg& msg);
    void handle_pairing_token_response(DeviceRuntime* dev, const PairingSessionTokenResponseMsg& msg);
    void handle_tcp_stream_open_response(DeviceRuntime* dev, const TcpStreamOpenResponseMsg& msg);
    void handle_tcp_stream_ack(DeviceRuntime* dev, const TcpStreamAckMsg& msg);

    void complete_pending(PendingRequest* req, int status, std::string_view body,
                          const std::vector<std::pair<std::string, std::string>>* headers = nullptr);
    void complete_pairing_session(DeviceRuntime* dev, int status, std::string_view body);
    void finish_pairing_token_waiters(DeviceRuntime* dev, int status, std::string_view body);
    PairingState* live_pairing(DeviceRuntime* dev, int64_t now);

    bool flush_stream(DeviceRuntime* dev, TcpStreamSession* stream);
    void drop_stream(DeviceRuntime* dev, TcpStreamSession* stream, int code, std::string_view reason, bool send_close_to_device);
    void send_tcp_stream_close(DeviceRuntime* dev, int channel_id, std::string_view grant_secret);
    TcpStreamSession* find_stream_by_service(DeviceRuntime* dev, std::string_view service);
    TcpStreamSession* find_stream_by_channel(DeviceRuntime* dev, int channel_id);
    bool stream_open_in_flight(DeviceRuntime* dev, std::string_view service);

    std::string next_request_id(DeviceRuntime* dev);
    void send_json_error(HttpRes* res, int status, std::string_view reason);
    void send_http_response(HttpRes* res, int status, std::string_view body,
                            const std::vector<std::pair<std::string, std::string>>* headers,
                            bool corked = true);

    void schedule_timer_at(int64_t deadline_ms);
    void arm_next_timer(int64_t now);
    static void on_us_timer(us_timer_t* t);

    const Config& cfg_;
    MemoryStore& store_;
    const AesGcmKey& routing_key_;
    const AesGcmKey& ws_key_;

    ankerl::unordered_dense::map<std::string, std::unique_ptr<DeviceRuntime>, StringHash, StringEq> devices_;

    TimerList auth_timers_;
    TimerList device_heartbeat_timers_;
    TimerList stream_heartbeat_timers_;
    TimerList request_timers_;
    TimerList stream_open_timers_;
    TimerList pairing_session_timers_;
    std::vector<PairingTokenWaiter*> pairing_token_waiters_;

    us_timer_t* timer_ = nullptr;
    int64_t armed_deadline_ms_ = INT64_MAX;
    bool in_timer_cb_ = false;
    size_t connected_devices_count_ = 0;
    size_t authenticated_devices_count_ = 0;
    size_t active_streams_count_ = 0;
};

} // namespace saihub
