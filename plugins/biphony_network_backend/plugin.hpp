#pragma once

#include "illixr/network/network_backend.hpp"
#include "illixr/network/topic_config.hpp"
#include "illixr/plugin.hpp"
#include "illixr/switchboard.hpp"

#include <biphony.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ILLIXR {

/**
 * Implements network::network_backend on top of Biphony, a QUIC-based
 * multipath transport, as a drop-in alternative transport to
 * tcp_network_backend. Exactly one of the two may be registered per process
 * (phonebook::register_impl asserts against duplicate registration of the
 * same service interface).
 */
class MY_EXPORT_API biphony_network_backend
    : public plugin
    , public network::network_backend {
public:
    explicit biphony_network_backend(const std::string& name_, phonebook* pb_);
    ~biphony_network_backend() override;

    void topic_create(std::string topic_name, network::topic_config& config) override;
    bool is_topic_networked(std::string topic_name) override;
    void topic_send(std::string topic_name, std::string&& message) override;
    void topic_receive(const std::string& topic_name, std::vector<char>& message);
    void stop() override;

    bool client;

private:
    template <typename Engine>
    void run_read_loop(Engine& engine);

    template <typename Engine>
    bool send_raw(Engine& engine, uint64_t peer_client_id, const std::string& topic_name,
                  const std::string& message, uint8_t biphony_priority);

    void send_to_peer(const std::string& topic_name, std::string&& message,
                       network::topic_config::priority_type priority);

    // Maps ILLIXR's topic_config::priority_type (higher enum value = more
    // urgent) onto Biphony/QUIC's HTTP/3 extensible-priority "urgency"
    // (RFC 9218, forwarded directly to quiche::Connection::stream_priority),
    // where LOWER numeric value = more urgent. The scale must be inverted, or
    // low-priority traffic would silently be sent ahead of high-priority
    // traffic.
    static uint8_t map_priority(network::topic_config::priority_type p);

    // Non-locking helper: callable both from the public, locking
    // is_topic_networked() and from topic_send() while topics_mutex_ is
    // already held (std::mutex is not recursive).
    bool is_topic_networked_locked(const std::string& topic_name);

    std::shared_ptr<switchboard> switchboard_;
    std::atomic<bool>            running_{true};

    // biphony::Server's constructor only blocks until its socket is bound,
    // not until a peer connects, and its try_send() silently drops (without
    // an error) any message addressed to a client_id with no live
    // connection. Since topic_create() fires immediately at construction
    // time on both roles, the server must wait for an explicit handshake
    // message from the client before sending its own first message, or its
    // initial "illixr_control" messages can be lost permanently.
    std::atomic<bool> peer_connected_{false};

    std::unique_ptr<biphony::Client> client_engine_;
    std::unique_ptr<biphony::Server> server_engine_;
    std::thread                      read_thread_;

    // Fresh stream id per outgoing message (never reused): client seeds 0
    // (client-initiated bidi streams: 0, 4, 8, ...), server seeds 1
    // (server-initiated bidi streams: 1, 5, 9, ...). Every topic_send() is a
    // one-shot, fire-and-forget push sent with fin=true immediately, so a
    // fresh stream id per message sidesteps Biphony's hard constraint that
    // reusing a stream_id before its prior message completes aborts the
    // engine thread.
    std::atomic<uint64_t> next_stream_id_{0};

    // Server hands out client_id sequentially starting at 0 per new
    // connection; this backend is strictly 1:1 (no multi-client support, matching
    // tcp_network_backend's single-peer design), so the server side can
    // hardcode the peer's client_id.
    static constexpr uint64_t SERVER_CLIENT_ID = 0;

    static const std::string CONTROL_TOPIC;
    static const std::string DELIMITER;

    std::mutex                                              topics_mutex_;
    std::vector<std::string>                                networked_topics_;
    std::unordered_map<std::string, network::topic_config>  networked_topics_configs_;

    // env-derived configuration
    bool        is_client_ = false;
    // Bare "ip:port" of the server, used by both roles: the server binds to
    // it directly, and the client wraps it into a connect URL
    // ("https://<server_address_>/") since biphony::ClientConfig expects a
    // URL rather than a bare socket address.
    std::string server_address_;
    std::string cert_path_;
    std::string key_path_;
    bool        no_verify_ = false;
    std::string steering_scheme_;
    // Only call biphony::{Client,Server}Config::initial_max_path_id() when
    // this is > 0. Biphony's multipath transport-parameter negotiation
    // (quiche's set_initial_max_path_id) checks only for the *presence* of a
    // value on both peers, not whether it's positive -- so calling it with 0
    // still negotiates the multipath extension on ("capped" at path 0 in
    // theory), rather than leaving multipath off entirely. The only way to
    // get genuinely single-path behavior with this API is to never call the
    // setter at all (leave the transport parameter unset on both sides), so
    // 0 here must mean "don't call it", not "call it with 0".
    uint64_t initial_max_path_id_ = 0;
    // Biphony's own hardcoded default (5000ms) is too short for ILLIXR: the
    // VIO/encoder pipeline can go several seconds between topic_create's
    // handshake messages and the first real data send while it warms up
    // (dataset loading, GStreamer encoder init, etc.), which is long enough
    // for quiche to tear the connection down as idle before any real traffic
    // ever flows. Default to a much longer timeout; override via env if
    // needed.
    uint64_t idle_timeout_ms_ = 60000;

    // Extra local source addresses to bind for additional multipath QUIC
    // paths (client-only; mirrors the repeatable `-A`/`--address` CLI flag
    // of the Biphony example clients and biphony::ClientConfig::address()).
    // Parsed from a comma-separated ILLIXR_BIPHONY_CLIENT_ADDRESSES env var.
    std::vector<std::string> client_addresses_;
};

} // namespace ILLIXR
