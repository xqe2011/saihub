#pragma once

#include "config.hpp"
#include "crypto.hpp"
#include "device_manager.hpp"
#include "store.hpp"

#include <uwebsockets/App.h>

#include <string>
#include <string_view>

namespace saihub {

class CloudServer {
public:
    explicit CloudServer(const Config& cfg);
    ~CloudServer();

    CloudServer(const CloudServer&) = delete;
    CloudServer& operator=(const CloudServer&) = delete;

    bool init();
    bool listen(int& bound_port_out);
    void run();
    void stop();
    void close_all();

    // Exposed for direct inspection/testing
    MemoryStore& store() { return store_; }
    DeviceManager& devices() { return devices_; }

private:
    void setup_routes();

    std::string derive_origin(uWS::HttpRequest* req) const;
    void send_json(HttpRes* res, int status, std::string_view body);
    void send_json_error(HttpRes* res, int status, std::string_view reason);
    void send_html(HttpRes* res, std::string_view html);
    void send_unauthorized(HttpRes* res, std::string_view origin, std::string_view pathname);

    bool require_admin(HttpRes* res, uWS::HttpRequest* req);
    std::optional<std::string> require_routing_token(HttpRes* res, uWS::HttpRequest* req,
                                                     std::string_view digest,
                                                     std::string_view origin,
                                                     std::string_view pathname);

    void read_body_and_run(HttpRes* res, size_t max_bytes,
                           std::function<void(std::string&& body)> cb);

    void handle_http(HttpRes* res, uWS::HttpRequest* req);
    void handle_health(HttpRes* res, uWS::HttpRequest* req);
    void handle_well_known(HttpRes* res, uWS::HttpRequest* req);
    void handle_protected_resource(HttpRes* res, uWS::HttpRequest* req, std::string_view pathname);
    void handle_register(HttpRes* res, uWS::HttpRequest* req);
    void handle_token(HttpRes* res, uWS::HttpRequest* req);
    void handle_oauth_redirect(HttpRes* res, uWS::HttpRequest* req);
    void handle_oauth_echo(HttpRes* res, uWS::HttpRequest* req);
    void handle_pairing_session(HttpRes* res, uWS::HttpRequest* req);
    void handle_pairing_token(HttpRes* res, uWS::HttpRequest* req);
    void handle_admin(HttpRes* res, uWS::HttpRequest* req, std::string_view pathname);
    void handle_landing(HttpRes* res, uWS::HttpRequest* req, std::string_view digest, std::string_view kind);
    void handle_device_http(HttpRes* res, uWS::HttpRequest* req, std::string_view digest,
                            std::string_view suffix, std::string_view query);

    Config cfg_;
    AesGcmKey routing_key_;
    AesGcmKey ws_key_;
    MemoryStore store_;
    DeviceManager devices_;
    uWS::App app_;
    us_listen_socket_t* listen_socket_ = nullptr;
    std::string pending_upgrade_digest_;
    int actual_port_ = 0;
    bool device_ws_hooked_ = false;
    bool stream_ws_hooked_ = false;
};

} // namespace saihub
