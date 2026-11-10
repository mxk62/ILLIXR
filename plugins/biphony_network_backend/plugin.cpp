#include "plugin.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <stdexcept>

using namespace ILLIXR;

const std::string biphony_network_backend::CONTROL_TOPIC = "illixr_control";
const std::string biphony_network_backend::DELIMITER     = ":";

namespace {
// Reserved topic name for the handshake "hello" the client sends immediately
// after connecting, so the server knows it's safe to start sending (see the
// peer_connected_ comment in plugin.hpp).
const std::string HELLO_TOPIC = "illixr_hello";

std::vector<std::string> split_comma_separated(const std::string& value) {
    std::vector<std::string> parts;
    size_t                   start = 0;
    while (start <= value.size()) {
        size_t comma = value.find(',', start);
        size_t end   = (comma == std::string::npos) ? value.size() : comma;
        if (end > start) {
            parts.push_back(value.substr(start, end - start));
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return parts;
}
} // namespace

biphony_network_backend::biphony_network_backend(const std::string& name_, phonebook* pb_)
    : plugin(name_, pb_)
    , switchboard_{pb_->lookup_impl<switchboard>()} {
    // Surfaces engine-internal errors (e.g. stream_send failures) that would
    // otherwise be silently discarded, since Biphony's FFI never installs a
    // log backend on its own. Controlled by RUST_LOG (e.g. RUST_LOG=quiche_apps=debug).
    biphony::init_logging();

    is_client_ =
        switchboard_->get_env_char("ILLIXR_IS_CLIENT") ? std::stoi(switchboard_->get_env_char("ILLIXR_IS_CLIENT")) != 0 : false;

    if (switchboard_->get_env_char("ILLIXR_BIPHONY_SERVER_ADDRESS")) {
        server_address_ = switchboard_->get_env_char("ILLIXR_BIPHONY_SERVER_ADDRESS");
    }
    if (switchboard_->get_env_char("ILLIXR_BIPHONY_CERT_PATH")) {
        cert_path_ = switchboard_->get_env_char("ILLIXR_BIPHONY_CERT_PATH");
    }
    if (switchboard_->get_env_char("ILLIXR_BIPHONY_KEY_PATH")) {
        key_path_ = switchboard_->get_env_char("ILLIXR_BIPHONY_KEY_PATH");
    }
    if (switchboard_->get_env_char("ILLIXR_BIPHONY_NO_VERIFY")) {
        no_verify_ = std::stoi(switchboard_->get_env_char("ILLIXR_BIPHONY_NO_VERIFY")) != 0;
    }
    if (switchboard_->get_env_char("ILLIXR_BIPHONY_STEERING_SCHEME")) {
        steering_scheme_ = switchboard_->get_env_char("ILLIXR_BIPHONY_STEERING_SCHEME");
    }
    if (switchboard_->get_env_char("ILLIXR_BIPHONY_INITIAL_MAX_PATH_ID")) {
        initial_max_path_id_ = std::stoull(switchboard_->get_env_char("ILLIXR_BIPHONY_INITIAL_MAX_PATH_ID"));
    }
    if (switchboard_->get_env_char("ILLIXR_BIPHONY_CLIENT_ADDRESSES")) {
        client_addresses_ = split_comma_separated(switchboard_->get_env_char("ILLIXR_BIPHONY_CLIENT_ADDRESSES"));
    }
    if (switchboard_->get_env_char("ILLIXR_BIPHONY_IDLE_TIMEOUT_MS")) {
        idle_timeout_ms_ = std::stoull(switchboard_->get_env_char("ILLIXR_BIPHONY_IDLE_TIMEOUT_MS"));
    }

    // Started before connecting, so that a client blocked in connect still reports.
    diag_reporter_.start(std::chrono::milliseconds(1000), [this]() {
        diag_report();
    });

    if (is_client_) {
        start_client();
    } else {
        start_server();
    }
}

void biphony_network_backend::diag_report() {
    auto                        log = spdlog::get("illixr");
    std::lock_guard<std::mutex> lock(stats_mutex_);
    log->info(
        "[DIAG][biphony] role={} peer_connected={} peer_wait_ms={} recv_ok={} recv_empty={} recv_not_fin={} recv_malformed={} "
        "recv_other_rc={} last_client_id={} client_id_flips={} running={}",
        is_client_ ? "client" : "server", peer_connected_.load(), peer_wait_ms_.load(), recv_calls_ok_, recv_empty_,
        recv_not_fin_, recv_malformed_, recv_other_rc_, last_client_id_, client_id_flips_, running_.load());
    for (const auto& [topic, s] : stats_) {
        log->info("[DIAG][biphony] topic={} sent={} refused={} bytes_sent={} received={} bytes_received={} out_of_order={} "
                  "duplicate={} missing={} no_topic={} highest_seq={} awaiting_seq={} held={}",
                  topic, s.sent, s.refused, s.bytes_sent, s.received, s.bytes_received, s.out_of_order, s.duplicate, s.missing,
                  s.no_topic, s.highest_seq, s.awaiting_seq, s.held);
    }
}

void biphony_network_backend::start_client() {
    client          = true;
    next_stream_id_ = 0; // client-initiated bidi streams: 0, 4, 8, ...

    // biphony::ClientConfig expects a URL, not a bare socket address.
    std::string           connect_url = "https://" + server_address_ + "/";
    biphony::ClientConfig cfg(connect_url);
    cfg.idle_timeout(idle_timeout_ms_);
    if (!steering_scheme_.empty()) {
        cfg.steering_scheme(steering_scheme_);
    }
    if (initial_max_path_id_ != 0) {
        cfg.initial_max_path_id(initial_max_path_id_);
    }
    if (no_verify_) {
        cfg.no_verify(true);
    }
    // Additional local addresses to bind for extra multipath QUIC paths
    // (path 0 is implicit/default; each entry here adds one more path,
    // mirroring the example clients' repeatable -A/--address flag).
    for (const std::string& addr : client_addresses_) {
        cfg.address(addr);
    }
    if (!cfg.valid()) {
        spdlog::get("illixr")->critical("[biphony_network_backend] invalid client config: {}", biphony::last_error_message());
        throw std::runtime_error("biphony_network_backend: invalid client config");
    }

    client_engine_ = std::make_unique<biphony::Client>(std::move(cfg)); // blocks until all paths validated
    if (!client_engine_->valid()) {
        spdlog::get("illixr")->critical("[biphony_network_backend] connect failed: {}", biphony::last_error_message());
        throw std::runtime_error("biphony_network_backend: connect failed");
    }
    spdlog::get("illixr")->info("[biphony_network_backend] connected to {}", connect_url);

    read_thread_ = std::thread([this]() {
        run_read_loop(*client_engine_);
    });

    // Tell the server we're here so it's safe for it to start sending
    // (see peer_connected_ comment in plugin.hpp). Framed the same way
    // as any other message so run_read_loop's ordinary unframing parses
    // out HELLO_TOPIC and short-circuits before reaching topic_receive.
    send_raw(*client_engine_, /*peer_client_id=*/0, HELLO_TOPIC, /*message=*/"", /*biphony_priority=*/0);
}

void biphony_network_backend::start_server() {
    client          = false;
    next_stream_id_ = 1; // server-initiated bidi streams: 1, 5, 9, ...

    biphony::ServerConfig cfg(server_address_, cert_path_, key_path_);
    cfg.idle_timeout(idle_timeout_ms_);
    if (!steering_scheme_.empty()) {
        cfg.steering_scheme(steering_scheme_);
    }
    if (initial_max_path_id_ != 0) {
        cfg.initial_max_path_id(initial_max_path_id_);
    }
    if (!cfg.valid()) {
        spdlog::get("illixr")->critical("[biphony_network_backend] invalid server config: {}", biphony::last_error_message());
        throw std::runtime_error("biphony_network_backend: invalid server config");
    }

    server_engine_ = std::make_unique<biphony::Server>(std::move(cfg)); // blocks until socket bound
    if (!server_engine_->valid()) {
        spdlog::get("illixr")->critical("[biphony_network_backend] listen failed: {}", biphony::last_error_message());
        throw std::runtime_error("biphony_network_backend: listen failed");
    }
    spdlog::get("illixr")->info("[biphony_network_backend] listening on {}", server_address_);

    read_thread_ = std::thread([this]() {
        run_read_loop(*server_engine_);
    });
}

biphony_network_backend::~biphony_network_backend() {
    if (running_) {
        stop();
    }
}

uint8_t biphony_network_backend::map_priority(network::topic_config::priority_type p) {
    return static_cast<uint8_t>(network::topic_config::HIGHEST - p);
}

template<typename Engine>
bool biphony_network_backend::send_raw(Engine& engine, uint64_t peer_client_id, const std::string& topic_name,
                                       const std::string& message, uint8_t biphony_priority) {
    // Wire payload: topic_name_length:4B (uint32) | topic_name | seq:8B (uint64, per topic) | message
    // The receiver delivers each topic in seq order (see deliver_in_order), so
    // a seq is consumed only when try_send() accepts the message; a refused
    // message must not leave a gap the receiver would wait on forever.
    // No outer total_length prefix is needed (unlike tcp_network_backend's
    // hand-rolled byte-stream framing): Biphony already delivers one complete
    // logical message per recv() call.
    // (DIAG: that claim is checked by counting recv deliveries with fin == false.)
    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
    topic_stats&                stats = stats_[topic_name];
    uint64_t                    seq   = stats.next_send_seq;

    std::vector<uint8_t> packet(4 + topic_name.size() + 8 + message.size());
    uint32_t             topic_name_length = static_cast<uint32_t>(topic_name.size());
    std::memcpy(packet.data(), &topic_name_length, 4);
    std::memcpy(packet.data() + 4, topic_name.data(), topic_name.size());
    std::memcpy(packet.data() + 4 + topic_name.size(), &seq, 8);
    std::memcpy(packet.data() + 4 + topic_name.size() + 8, message.data(), message.size());

    uint64_t stream_id = next_stream_id_.fetch_add(4);
    bool     ok        = engine.try_send(peer_client_id, stream_id, packet, /*fin=*/true, biphony_priority);
    if (ok) {
        stats.next_send_seq++;
        stats.sent++;
        stats.bytes_sent += packet.size();
    } else {
        stats.refused++;
        spdlog::get("illixr")->warn(
            "[DIAG][biphony] try_send refused: topic={} seq={} size={} stream_id={} client_id={} error='{}'", topic_name, seq,
            packet.size(), stream_id, peer_client_id, biphony::last_error_message());
    }
    return ok;
}

void biphony_network_backend::send_to_peer(const std::string& topic_name, std::string&& message,
                                           network::topic_config::priority_type priority) {
    uint8_t prio = map_priority(priority);
    if (client) {
        // biphony::Client only ever talks to one server; the client_id
        // argument is ignored on the client side.
        send_raw(*client_engine_, /*peer_client_id=*/0, topic_name, message, prio);
    } else {
        while (!peer_connected_ && running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            peer_wait_ms_ += 100;
        }
        if (!running_) {
            return;
        }
        send_raw(*server_engine_, SERVER_CLIENT_ID, topic_name, message, prio);
    }
}

template<typename Engine>
void biphony_network_backend::run_read_loop(Engine& engine) {
    while (running_) {
        biphony::Message msg;
        int              rc = engine.recv(msg); // blocks indefinitely
        if (rc == BIPHONY_ERR_DISCONNECTED) {
            spdlog::get("illixr")->warn("[biphony_network_backend] peer disconnected");
            running_ = false;
            break;
        }
        if (rc != BIPHONY_OK) {
            std::lock_guard<std::mutex> stats_lock(stats_mutex_);
            recv_other_rc_++;
            continue;
        }

        {
            std::lock_guard<std::mutex> stats_lock(stats_mutex_);
            recv_calls_ok_++;
            if (recv_calls_ok_ > 1 && msg.client_id != last_client_id_) {
                client_id_flips_++;
                spdlog::get("illixr")->warn("[DIAG][biphony] client_id changed {} -> {}", last_client_id_, msg.client_id);
            }
            last_client_id_ = msg.client_id;
            if (!msg.fin) {
                recv_not_fin_++;
                if (recv_not_fin_ <= 20) {
                    spdlog::get("illixr")->warn("[DIAG][biphony] recv delivered fin=false: stream_id={} size={} client_id={}",
                                                msg.stream_id, msg.data.size(), msg.client_id);
                }
            }
        }

        if (msg.data.empty()) {
            // The peer closing its write side of one of our streams (Biphony
            // stream retirement) arrives as an empty message; not app data.
            std::lock_guard<std::mutex> stats_lock(stats_mutex_);
            recv_empty_++;
            continue;
        }

        uint32_t topic_name_length = 0;
        if (msg.data.size() >= 4) {
            std::memcpy(&topic_name_length, msg.data.data(), 4);
        }
        if (msg.data.size() < 4 || msg.data.size() < 4 + static_cast<uint64_t>(topic_name_length) + 8) {
            std::lock_guard<std::mutex> stats_lock(stats_mutex_);
            recv_malformed_++;
            if (recv_malformed_ <= 20) {
                spdlog::get("illixr")->warn(
                    "[DIAG][biphony] malformed message: stream_id={} size={} fin={} topic_name_length={}", msg.stream_id,
                    msg.data.size(), msg.fin, topic_name_length);
            }
            continue;
        }

        std::string topic_name(msg.data.begin() + 4, msg.data.begin() + 4 + topic_name_length);
        uint64_t    seq = 0;
        std::memcpy(&seq, msg.data.data() + 4 + topic_name_length, 8);
        {
            std::lock_guard<std::mutex> stats_lock(stats_mutex_);
            topic_stats&                stats = stats_[topic_name];
            stats.received++;
            stats.bytes_received += msg.data.size();
            auto s = static_cast<int64_t>(seq);
            if (s < stats.highest_seq) {
                stats.out_of_order++;
                if (stats.out_of_order <= 20) {
                    spdlog::get("illixr")->warn("[DIAG][biphony] out of order: topic={} seq={} highest_seq={} stream_id={}",
                                                topic_name, seq, stats.highest_seq, msg.stream_id);
                }
            } else if (s == stats.highest_seq) {
                stats.duplicate++;
            } else {
                stats.missing += static_cast<uint64_t>(s - stats.highest_seq - 1);
                stats.highest_seq = s;
            }
            if (stats.received <= 5) {
                spdlog::get("illixr")->info("[DIAG][biphony] recv: topic={} seq={} size={} stream_id={} fin={} client_id={}",
                                            topic_name, seq, msg.data.size(), msg.stream_id, msg.fin, msg.client_id);
            }
        }

        if (!client && topic_name == HELLO_TOPIC) {
            peer_connected_ = true;
            spdlog::get("illixr")->info("[biphony_network_backend] client connected");
            continue;
        }

        deliver_in_order(topic_name, seq, std::vector<char>(msg.data.begin() + 4 + topic_name_length + 8, msg.data.end()));
    }
}

void biphony_network_backend::deliver_in_order(const std::string& topic_name, uint64_t seq, std::vector<char>&& message) {
    // Biphony sends every message on its own QUIC stream, and QUIC does not
    // order data across streams, so messages of one topic can arrive out of
    // order (observed: the first compressed_imu_cam frames arrived as seq 7,
    // 61, 127, ..., 1, ...). Consumers rely on the in-order delivery that
    // tcp_network_backend's single byte stream gives them -- e.g. the H.264
    // decoder in offload_vio.server_rx hangs forever if its first frame is not
    // the first encoded frame. So hold early messages until the gap fills.
    // Only the read thread calls this.
    reorder_state& state = reorder_[topic_name];
    if (seq < state.next_seq) {
        spdlog::get("illixr")->warn("[biphony_network_backend] dropping stale message: topic={} seq={} expected={}", topic_name,
                                    seq, state.next_seq);
        return;
    }
    if (seq > state.next_seq) {
        state.held.emplace(seq, std::move(message));
    } else {
        topic_receive(topic_name, message);
        state.next_seq++;
        for (auto it = state.held.begin(); it != state.held.end() && it->first == state.next_seq; it = state.held.erase(it)) {
            topic_receive(topic_name, it->second);
            state.next_seq++;
        }
    }
    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
    topic_stats&                stats = stats_[topic_name];
    stats.held                        = state.held.size();
    stats.awaiting_seq                = state.next_seq;
}

void biphony_network_backend::topic_create(std::string topic_name, network::topic_config& config) {
    {
        std::lock_guard<std::mutex> lock(topics_mutex_);
        networked_topics_.push_back(topic_name);
        networked_topics_configs_[topic_name] = config;
    }
    std::string serialization = config.serialization_method == network::topic_config::BOOST ? "BOOST" : "PROTOBUF";
    std::string message       = "create_topic" + topic_name + DELIMITER + serialization;
    send_to_peer(CONTROL_TOPIC, std::move(message), network::topic_config::MEDIUM);
}

bool biphony_network_backend::is_topic_networked_locked(const std::string& topic_name) {
    return std::find(networked_topics_.begin(), networked_topics_.end(), topic_name) != networked_topics_.end();
}

bool biphony_network_backend::is_topic_networked(std::string topic_name) {
    std::lock_guard<std::mutex> lock(topics_mutex_);
    return is_topic_networked_locked(topic_name);
}

void biphony_network_backend::topic_send(std::string topic_name, std::string&& message) {
    network::topic_config::priority_type priority = network::topic_config::MEDIUM;
    {
        std::lock_guard<std::mutex> lock(topics_mutex_);
        if (!is_topic_networked_locked(topic_name)) {
            spdlog::get("illixr")->warn("[biphony_network_backend] topic {} not networked", topic_name);
            return;
        }
        priority = networked_topics_configs_[topic_name].priority;
    }
    send_to_peer(topic_name, std::move(message), priority);
}

void biphony_network_backend::topic_receive(const std::string& topic_name, std::vector<char>& message) {
    if (topic_name == CONTROL_TOPIC) {
        std::string message_str(message.begin(), message.end());
        if (message_str.find("create_topic") == 0) {
            size_t d_pos = message_str.find(DELIMITER);
            assert(d_pos != std::string::npos);
            std::string l_topic_name  = message_str.substr(12, d_pos - 12);
            std::string serialization = message_str.substr(d_pos + 1);

            network::topic_config config;
            config.serialization_method =
                (serialization == "BOOST") ? network::topic_config::BOOST : network::topic_config::PROTOBUF;
            config.transport_method = network::topic_config::BIPHONY;
            {
                std::lock_guard<std::mutex> lock(topics_mutex_);
                networked_topics_.push_back(l_topic_name);
                networked_topics_configs_[l_topic_name] = config;
            }
            spdlog::get("illixr")->info("[biphony_network_backend] received create_topic for {}", l_topic_name);
        }
        return;
    }

    if (!switchboard_->topic_exists(topic_name)) {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        topic_stats&                stats = stats_[topic_name];
        stats.no_topic++;
        if (stats.no_topic <= 5) {
            spdlog::get("illixr")->warn("[DIAG][biphony] dropped message: no local switchboard topic '{}'", topic_name);
        }
        return;
    }

    network::topic_config cfg;
    {
        std::lock_guard<std::mutex> lock(topics_mutex_);
        cfg = networked_topics_configs_[topic_name];
    }
    switchboard_->get_topic(topic_name).deserialize_and_put(message, cfg);
}

void biphony_network_backend::stop() {
    diag_report();
    diag_reporter_.stop();
    running_ = false;
    // close() signals shutdown AND unblocks the in-flight blocking recv() in
    // read_thread_, and joins Biphony's own internal engine thread before
    // returning -- so the subsequent read_thread_.join() below is guaranteed
    // to return promptly. This is an improvement over tcp_network_backend,
    // whose detached read thread is never joined.
    if (client_engine_) {
        client_engine_->close();
    }
    if (server_engine_) {
        server_engine_->close();
    }
    if (read_thread_.joinable()) {
        read_thread_.join();
    }
}

extern "C" MY_EXPORT_API plugin* this_plugin_factory(phonebook* pb) {
    auto* obj = new biphony_network_backend("biphony_network_backend", pb);
    // The runtime owns the plugin returned by this factory. Register a non-owning
    // service alias so the phonebook does not try to delete the same object again.
    pb->register_impl<network::biphony_backend>(std::shared_ptr<network::biphony_backend>(
        static_cast<network::biphony_backend*>(obj), [](network::biphony_backend*) {}));
    return obj;
}
