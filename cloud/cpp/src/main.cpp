#include "config.hpp"
#include "log.hpp"
#include "server.hpp"

#include <libusockets.h>
#include <uwebsockets/Loop.h>

#include <atomic>
#include <csignal>
#include <iostream>

namespace {

std::atomic<bool> g_stop_requested{false};
std::atomic<int> g_stop_signal{0};
us_loop_t* g_loop = nullptr;

void handle_signal(int sig) {
    g_stop_signal.store(sig, std::memory_order_relaxed);
    g_stop_requested.store(true, std::memory_order_relaxed);
    if (g_loop) {
        us_wakeup_loop(g_loop);
    }
}

void install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

} // namespace

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    install_signal_handlers();

    saihub::Config cfg = saihub::load_config();
    saihub::set_log_level(cfg.log_level);

    if (cfg.db_path.empty()) {
        LOG_ERROR("[boot] DB_PATH must be configured");
        return 1;
    }

    LOG_INFO("[boot] starting host=" << cfg.host << " port=" << cfg.port
             << " authTimeoutMs=" << cfg.auth_timeout_ms
             << " requestTimeoutMs=" << cfg.request_timeout_ms
             << " wsTokenTtlS=" << cfg.websocket_token_ttl_s
             << " dbPath=" << cfg.db_path
             << " adminTokenConfigured=" << (!cfg.admin_token.empty() ? "true" : "false")
             << " routingSecretConfigured=" << (!cfg.routing_token_secret.empty() ? "true" : "false")
             << " wsSecretConfigured=" << (!cfg.websocket_token_secret.empty() ? "true" : "false"));

    saihub::CloudServer server(cfg);
    if (!server.init()) {
        return 1;
    }
    g_loop = reinterpret_cast<us_loop_t*>(uWS::Loop::get());

    bool closed = false;
    uWS::Loop::get()->addPostHandler(&server, [&](uWS::Loop*) {
        if (!closed && g_stop_requested.load(std::memory_order_relaxed)) {
            closed = true;
            LOG_INFO("[shutdown] signal=" << g_stop_signal.load(std::memory_order_relaxed) << " closing server");
            server.close_all();
            if (g_loop) {
                us_wakeup_loop(g_loop);
            }
        }
    });

    int bound_port = 0;
    if (!server.listen(bound_port)) {
        LOG_ERROR("[boot] failed to listen on " << cfg.host << ":" << cfg.port);
        return 1;
    }

    std::cout << "saihub-cloud listening on " << cfg.host << ":" << bound_port << std::endl;
    LOG_INFO("[boot] listening host=" << cfg.host << " port=" << bound_port);
    server.run();
    uWS::Loop::get()->removePostHandler(&server);
    g_loop = nullptr;
    LOG_INFO("[shutdown] stopped");
    return 0;
}
