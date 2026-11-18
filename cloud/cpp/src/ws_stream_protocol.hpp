#pragma once

#include "device_manager.hpp"

#include <uwebsockets/AsyncSocket.h>
#include <uwebsockets/WebSocket.h>
#include <uwebsockets/WebSocketContextData.h>
#include <uwebsockets/WebSocketData.h>
#include <uwebsockets/WebSocketProtocol.h>

#include <string>
#include <string_view>

namespace saihub {

// Custom uWS WebSocketProtocol handler that streams BINARY frames immediately
// as TCP chunks arrive, while preserving full reassembly + UTF-8 validation
// for TEXT frames and standard handling for CLOSE/PING/PONG control frames.
template <typename UserData, typename BinaryHandler>
struct BinaryStreamWsProtocol {
    using WS = uWS::WebSocket<false, true, UserData>;
    using CtxData = uWS::WebSocketContextData<false, UserData>;

    static bool setCompressed(uWS::WebSocketState<true>*, void*) {
        return false;
    }

    static void forceClose(uWS::WebSocketState<true>*, void* s, std::string_view reason = {}) {
        us_socket_close(0, static_cast<us_socket_t*>(s), static_cast<int>(reason.length()), const_cast<char*>(reason.data()));
    }

    static bool refusePayloadLength(uint64_t length, uWS::WebSocketState<true>*, void* s) {
        auto* ctx_data = static_cast<CtxData*>(
            us_socket_context_ext(0, us_socket_context(0, static_cast<us_socket_t*>(s)))
        );
        return ctx_data->maxPayloadLength < length;
    }

    static auto* get_target(UserData* ud) {
        if constexpr (std::is_same_v<UserData, DeviceWsData>) {
            return ud->session;
        } else {
            return ud->stream;
        }
    }

    static bool handleFragment(
        char* data,
        size_t length,
        unsigned int remaining_bytes,
        int op_code,
        bool fin,
        uWS::WebSocketState<true>*,
        void* s
    ) {
        auto* ws = reinterpret_cast<WS*>(s);
        auto* ud = ws->getUserData();
        auto* target = get_target(ud);
        if (!target || !ud->mgr) {
            return true;
        }
        auto* ctx_data = static_cast<CtxData*>(
            us_socket_context_ext(0, us_socket_context(0, static_cast<us_socket_t*>(s)))
        );

        if (op_code == uWS::OpCode::BINARY) {
            BinaryHandler::on_binary(ud->mgr, ws, std::string_view(data, length), remaining_bytes, fin);
            return !get_target(ud) || us_socket_is_closed(0, static_cast<us_socket_t*>(s)) ||
                   us_socket_is_shut_down(0, static_cast<us_socket_t*>(s));
        }

        if (op_code == uWS::OpCode::TEXT) {
            if (!remaining_bytes && fin && target->text_frag_buf.empty()) {
                if (!uWS::protocol::isValidUtf8(reinterpret_cast<unsigned char*>(data), length)) {
                    forceClose(nullptr, s, uWS::ERR_INVALID_TEXT);
                    return true;
                }
                if (ctx_data->messageHandler) {
                    ctx_data->messageHandler(ws, std::string_view(data, length), uWS::OpCode::TEXT);
                    if (!get_target(ud) || us_socket_is_closed(0, static_cast<us_socket_t*>(s)) ||
                        us_socket_is_shut_down(0, static_cast<us_socket_t*>(s))) {
                        return true;
                    }
                }
            } else {
                if (target->text_frag_buf.empty()) {
                    target->text_frag_buf.reserve(length + remaining_bytes);
                }
                if (refusePayloadLength(length + target->text_frag_buf.size(), nullptr, s)) {
                    forceClose(nullptr, s, uWS::ERR_TOO_BIG_MESSAGE);
                    return true;
                }
                target->text_frag_buf.append(data, length);
                if (!remaining_bytes && fin) {
                    if (!uWS::protocol::isValidUtf8(
                            reinterpret_cast<unsigned char*>(target->text_frag_buf.data()),
                            target->text_frag_buf.size())) {
                        forceClose(nullptr, s, uWS::ERR_INVALID_TEXT);
                        return true;
                    }
                    std::string msg = std::move(target->text_frag_buf);
                    target->text_frag_buf.clear();
                    if (ctx_data->messageHandler) {
                        ctx_data->messageHandler(ws, msg, uWS::OpCode::TEXT);
                        if (!get_target(ud) || us_socket_is_closed(0, static_cast<us_socket_t*>(s)) ||
                            us_socket_is_shut_down(0, static_cast<us_socket_t*>(s))) {
                            return true;
                        }
                    }
                }
            }
            return false;
        }

        if (!remaining_bytes && fin && target->control_frag_buf.empty()) {
            if (op_code == uWS::OpCode::CLOSE) {
                auto cf = uWS::protocol::parseClosePayload(data, length);
                ws->end(cf.code, std::string_view(cf.message, cf.length));
                return true;
            }
            if (op_code == uWS::OpCode::PING) {
                ws->send(std::string_view(data, length), uWS::OpCode::PONG);
                if (ctx_data->pingHandler) {
                    ctx_data->pingHandler(ws, std::string_view(data, length));
                    if (!get_target(ud) || us_socket_is_closed(0, static_cast<us_socket_t*>(s)) ||
                        us_socket_is_shut_down(0, static_cast<us_socket_t*>(s))) {
                        return true;
                    }
                }
            } else if (op_code == uWS::OpCode::PONG && ctx_data->pongHandler) {
                ctx_data->pongHandler(ws, std::string_view(data, length));
                if (!get_target(ud) || us_socket_is_closed(0, static_cast<us_socket_t*>(s)) ||
                    us_socket_is_shut_down(0, static_cast<us_socket_t*>(s))) {
                    return true;
                }
            }
        } else {
            target->control_frag_buf.append(data, length);
            if (target->control_frag_buf.size() > 125) {
                forceClose(nullptr, s, uWS::ERR_TOO_BIG_MESSAGE);
                return true;
            }
            if (!remaining_bytes && fin) {
                std::string ctrl = std::move(target->control_frag_buf);
                target->control_frag_buf.clear();
                if (op_code == uWS::OpCode::CLOSE) {
                    auto cf = uWS::protocol::parseClosePayload(ctrl.data(), ctrl.size());
                    ws->end(cf.code, std::string_view(cf.message, cf.length));
                    return true;
                }
                if (op_code == uWS::OpCode::PING) {
                    ws->send(ctrl, uWS::OpCode::PONG);
                }
            }
        }
        return false;
    }

    static void install(us_socket_context_t* ws_ctx) {
        us_socket_context_on_data(0, ws_ctx, [](us_socket_t* s, char* data, int length) -> us_socket_t* {
            auto* ws = reinterpret_cast<WS*>(s);
            auto* ud = ws->getUserData();
            if (!get_target(ud)) {
                return s;
            }
            auto* ws_data = static_cast<uWS::WebSocketData*>(us_socket_ext(0, s));
            auto* ctx_data = static_cast<CtxData*>(
                us_socket_context_ext(0, us_socket_context(0, s))
            );
            us_socket_timeout(0, s, ctx_data->idleTimeoutComponents.first);
            ws->cork([&]() {
                uWS::WebSocketProtocol<true, BinaryStreamWsProtocol>::consume(
                    data, static_cast<unsigned int>(length),
                    static_cast<uWS::WebSocketState<true>*>(ws_data), s);
            });
            if (!us_socket_is_closed(0, s) && !us_socket_is_shut_down(0, s) &&
                ws->getBufferedAmount() == 0 && !get_target(ud)) {
                us_socket_shutdown(0, s);
            }
            return s;
        });
    }
};

struct DeviceBinaryHandler {
    static void on_binary(DeviceManager* mgr, DeviceWS* ws, std::string_view chunk, unsigned int rem, bool fin) {
        mgr->on_device_binary_fragment(ws, chunk, rem, fin);
    }
};

struct TcpStreamBinaryHandler {
    static void on_binary(DeviceManager* mgr, TcpStreamWS* ws, std::string_view chunk, unsigned int rem, bool fin) {
        mgr->on_tcp_stream_binary_fragment(ws, chunk, rem, fin);
    }
};

} // namespace saihub
