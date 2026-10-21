#include "plugin.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
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

std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

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
    , switchboard_{pb_->lookup_impl<switchboard>()}
    , rtt_logger_{env_or_empty("ILLIXR_LATENCY_CSV")}
    , start_time_{std::chrono::steady_clock::now()} {
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

    if (is_client_) {
        start_client();
    } else {
        start_server();
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
uint64_t biphony_network_backend::send_raw(Engine& engine, uint64_t peer_client_id, const std::string& topic_name,
                                           const std::string& message, uint8_t biphony_priority) {
    // Wire payload: topic_name_length:4B (uint32) | topic_name | seq:8B |
    // message
    // No outer total_length prefix is needed (unlike tcp_network_backend's
    // hand-rolled byte-stream framing): Biphony already delivers one complete
    // logical message per recv() call.
    uint64_t seq = next_seq_.fetch_add(1);

    std::vector<uint8_t> packet(4 + topic_name.size() + network::SEQ_WIRE_SIZE + message.size());
    uint32_t             topic_name_length = static_cast<uint32_t>(topic_name.size());
    std::memcpy(packet.data(), &topic_name_length, 4);
    std::memcpy(packet.data() + 4, topic_name.data(), topic_name.size());
    std::memcpy(packet.data() + 4 + topic_name.size(), &seq, network::SEQ_WIRE_SIZE);
    std::memcpy(packet.data() + 4 + topic_name.size() + network::SEQ_WIRE_SIZE, message.data(), message.size());

    uint64_t stream_id = next_stream_id_.fetch_add(4);
    (void) engine.try_send(peer_client_id, stream_id, packet, /*fin=*/true, biphony_priority);
    return seq;
}

uint64_t biphony_network_backend::send_to_peer(const std::string& topic_name, std::string&& message,
                                               network::topic_config::priority_type priority) {
    uint8_t prio = map_priority(priority);
    if (client) {
        // biphony::Client only ever talks to one server; the client_id
        // argument is ignored on the client side.
        return send_raw(*client_engine_, /*peer_client_id=*/0, topic_name, message, prio);
    } else {
        while (!peer_connected_ && running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!running_) {
            return 0;
        }
        return send_raw(*server_engine_, SERVER_CLIENT_ID, topic_name, message, prio);
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
            continue;
        }

        if (msg.data.size() < 4) {
            continue;
        }
        uint32_t topic_name_length;
        std::memcpy(&topic_name_length, msg.data.data(), 4);
        if (msg.data.size() < 4 + topic_name_length + network::SEQ_WIRE_SIZE) {
            continue;
        }

        std::string topic_name(msg.data.begin() + 4, msg.data.begin() + 4 + topic_name_length);

        size_t   seq_offset = 4 + topic_name_length;
        uint64_t seq;
        std::memcpy(&seq, msg.data.data() + seq_offset, network::SEQ_WIRE_SIZE);
        size_t message_offset = seq_offset + network::SEQ_WIRE_SIZE;

        if (!client && topic_name == HELLO_TOPIC) {
            peer_connected_ = true;
            spdlog::get("illixr")->info("[biphony_network_backend] client connected");
            continue;
        }

        std::vector<char> message(msg.data.begin() + message_offset, msg.data.end());
        topic_receive(topic_name, message, seq);
    }
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

    bool     track   = rtt_logger_.enabled();
    auto     sent_at = std::chrono::steady_clock::now();
    size_t   bytes   = message.size();
    uint64_t seq     = send_to_peer(topic_name, std::move(message), priority);
    if (track) {
        pending_sends_.record(seq, network::pending_send_info{sent_at, topic_name, bytes});
    }
}

void biphony_network_backend::topic_receive(const std::string& topic_name, std::vector<char>& message, uint64_t seq) {
    if (topic_name == network::LATENCY_ACK_TOPIC) {
        if (message.size() >= 8) {
            uint64_t acked_seq;
            std::memcpy(&acked_seq, message.data(), 8);
            network::pending_send_info info;
            if (pending_sends_.complete(acked_seq, info)) {
                auto   now     = std::chrono::steady_clock::now();
                double rtt_ms  = std::chrono::duration<double, std::milli>(now - info.sent_at).count();
                double send_ms = std::chrono::duration<double, std::milli>(info.sent_at - start_time_).count();
                rtt_logger_.log(acked_seq, info.topic_name, info.bytes, send_ms, rtt_ms);
            }
        }
        return;
    }

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
        return;
    }

    if (rtt_logger_.enabled()) {
        std::string ack_payload;
        ack_payload.resize(8);
        std::memcpy(ack_payload.data(), &seq, 8);
        send_to_peer(network::LATENCY_ACK_TOPIC, std::move(ack_payload), network::topic_config::MEDIUM);
    }

    network::topic_config cfg;
    {
        std::lock_guard<std::mutex> lock(topics_mutex_);
        cfg = networked_topics_configs_[topic_name];
    }
    switchboard_->get_topic(topic_name).deserialize_and_put(message, cfg);
}

void biphony_network_backend::stop() {
    rtt_logger_.flush();
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
        static_cast<network::biphony_backend*>(obj), [](network::biphony_backend*) { }));
    return obj;
}
