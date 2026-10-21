#include "plugin.hpp"

#include <cstdlib>
#include <cstring>

static constexpr uint32_t MAX_PACKET_BYTES = 256u * 1024u * 1024u;

using namespace ILLIXR;

namespace {
std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}
} // namespace

tcp_network_backend::tcp_network_backend(const std::string& name_, phonebook* pb_)
    : plugin(name_, pb_)
    , switchboard_{pb_->lookup_impl<switchboard>()}
    , rtt_logger_{env_or_empty("ILLIXR_LATENCY_CSV")}
    , start_time_{std::chrono::steady_clock::now()} {
    // read environment variables
    if (switchboard_->get_env_char("ILLIXR_SERVER_IP")) {
        server_ip_ = switchboard_->get_env_char("ILLIXR_SERVER_IP");
        spdlog::get("illixr")->info("[tcp_network_backend] Using server IP {}", server_ip_);
    } else if (switchboard_->get_env_char("ILLIXR_TCP_SERVER_IP")) {
        server_ip_ = switchboard_->get_env_char("ILLIXR_TCP_SERVER_IP");
        spdlog::get("illixr")->info("[tcp_network_backend] Using TCP server IP {}", server_ip_);
    }

    if (switchboard_->get_env_char("ILLIXR_TCP_SERVER_PORT")) {
        server_port_ = std::stoi(switchboard_->get_env_char("ILLIXR_TCP_SERVER_PORT"));
        spdlog::get("illixr")->info("[tcp_network_backend] Using TCP server port {}", server_port_);
    }

    if (switchboard_->get_env_char("ILLIXR_CLIENT_IP")) {
        client_ip_ = switchboard_->get_env_char("ILLIXR_CLIENT_IP");
        spdlog::get("illixr")->info("[tcp_network_backend] Using client IP {}", client_ip_);
    } else if (switchboard_->get_env_char("ILLIXR_TCP_CLIENT_IP")) {
        client_ip_ = switchboard_->get_env_char("ILLIXR_TCP_CLIENT_IP");
        spdlog::get("illixr")->info("[tcp_network_backend] Using TCP client IP {}", client_ip_);
    }

    if (switchboard_->get_env_char("ILLIXR_TCP_CLIENT_PORT")) {
        client_port_ = std::stoi(switchboard_->get_env_char("ILLIXR_TCP_CLIENT_PORT"));
        spdlog::get("illixr")->info("[tcp_network_backend] Using TCP client port {}", client_port_);
    }

    if (switchboard_->get_env_char("ILLIXR_IS_CLIENT")) {
        is_client_ = std::stoi(switchboard_->get_env_char("ILLIXR_IS_CLIENT"));
        spdlog::get("illixr")->info("[tcp_network_backend] Is client {}", is_client_);
    } else {
        is_client_ = 0;
    }

    if (is_client_) {
        client = true;
#ifdef __ANDROID__
        // The Quest may launch before desktop ILLIXR. Keep the app resident and
        // retry until the host starts listening or shutdown is requested.
        while (running_) {
            auto* socket = new network::TCPSocket();
            try {
                socket->socket_set_reuseaddr();
                if (switchboard_->get_env_char("ILLIXR_TCP_CLIENT_IP") &&
                    switchboard_->get_env_char("ILLIXR_TCP_CLIENT_PORT")) {
                    socket->socket_bind(client_ip_, client_port_);
                }

                spdlog::get("illixr")->info("[tcp_network_backend] Connecting to {}:{}", server_ip_, server_port_);
                socket->socket_connect(server_ip_, server_port_);
                socket->enable_no_delay();
                peer_socket_ = socket;
                spdlog::get("illixr")->info("[tcp_network_backend] Connected; TCP_NODELAY={}", socket->is_no_delay());
                break;
            } catch (const std::exception& error) {
                delete socket;
                spdlog::get("illixr")->warn("[tcp_network_backend] Desktop is not ready ({}); retrying", error.what());
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
#else
        io_thread_ = std::thread([this]() {
            start_client();
        });

        // wait till we are connected
        while (!ready_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
#endif
    } else {
        client = false;
#ifdef __ANDROID__
        server_socket_.socket_set_reuseaddr();
        server_socket_.socket_bind(server_ip_, server_port_);
        server_socket_.enable_no_delay();
        server_socket_.socket_listen();

        auto* client_socket = new network::TCPSocket(server_socket_.socket_accept());
        // Set linger-zero on the accepted socket so it sends RST on close
        // rather than entering TIME_WAIT after a force-quit.
        client_socket->socket_set_linger_zero();
        spdlog::get("illixr")->debug("Accepted connection from client: " + client_socket->peer_address());
        peer_socket_ = client_socket;
#else
        io_thread_ = std::thread([this]() {
            start_server();
        });

        while (!ready_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
#endif
    }
}

#ifdef __ANDROID__
void tcp_network_backend::start() {
    plugin::start();
    io_thread_ = std::thread([this]() {
        read_loop(peer_socket_);
    });
}

#else

void tcp_network_backend::start_client() {
    auto* socket = new network::TCPSocket();
    if (switchboard_->get_env_char("ILLIXR_TCP_CLIENT_IP") && switchboard_->get_env_char("ILLIXR_TCP_CLIENT_PORT")) {
        // socket_bind() sets SO_REUSEADDR, SO_REUSEPORT, and SO_LINGER=0 internally.
        socket->socket_bind(client_ip_, client_port_);
    } else {
        socket->socket_set_linger_zero();
    }
    socket->socket_set_reuseaddr();
    socket->enable_no_delay();
    peer_socket_ = socket;

    std::cout << "Connecting to " + server_ip_ + " at port " + std::to_string(server_port_) << std::endl;
    socket->socket_connect(server_ip_, server_port_);
    std::cout << "Connected to server" << std::endl;

    ready_ = true;
    read_loop(socket);
}

void tcp_network_backend::start_server() {
    network::TCPSocket server_socket;
    server_socket.socket_set_reuseaddr();
    server_socket.socket_bind(server_ip_, server_port_);
    server_socket.enable_no_delay();
    server_socket.socket_listen();

    auto* client_socket = new network::TCPSocket(server_socket.socket_accept());
    client_socket->socket_set_linger_zero();
    client_socket->enable_no_delay();
    spdlog::get("illixr")->info("[tcp_network_backend] TCP_NODELAY verified = {}", client_socket->is_no_delay());
    std::cout << "Accepted connection from client: " << client_socket->peer_address() << std::endl;
    peer_socket_ = client_socket;
    ready_       = true;
    read_loop(client_socket);
}
#endif

void tcp_network_backend::read_loop(network::TCPSocket* socket) {
    std::string buffer;
    while (running_) {
        // read from socket
        // packet are in the format
        // total_length:4bytes|topic_name_length:4bytes|topic_name|message
#ifdef __ANDROID__
        std::string packet = socket->read_data(10000);
#else
        std::string packet = socket->read_data();
#endif
        if (packet.empty()) {
            if (running_.exchange(false)) {
                spdlog::get("illixr")->error("[tcp_network_backend] TCP connection closed or read failed; restart the session");
            }
            return;
        }

        buffer += packet;

        // check if we have a complete packet
        while (buffer.size() >= 8) {
            uint32_t total_length;
            uint32_t topic_name_length;
            std::memcpy(&total_length, buffer.data(), sizeof(total_length));
            std::memcpy(&topic_name_length, buffer.data() + 4, sizeof(topic_name_length));
            if (total_length < 8 + network::SEQ_WIRE_SIZE || total_length > MAX_PACKET_BYTES ||
                topic_name_length > total_length - 8 - network::SEQ_WIRE_SIZE) {
                spdlog::get("illixr")->error("[tcp_network_backend] malformed packet header (total_length={}, "
                                             "topic_name_length={}, buffered={} B) -- stream is desynced, "
                                             "closing the read loop",
                                             total_length, topic_name_length, buffer.size());
                running_ = false;
                return;
            }

            if (buffer.size() >= total_length) {
                std::string topic_name(buffer.data() + 8, topic_name_length);

                size_t   seq_offset = 8 + topic_name_length;
                uint64_t seq;
                std::memcpy(&seq, buffer.data() + seq_offset, network::SEQ_WIRE_SIZE);

                size_t            message_offset = seq_offset + network::SEQ_WIRE_SIZE;
                std::vector<char> message(buffer.begin() + message_offset, buffer.begin() + total_length);
                try {
                    topic_receive(topic_name, message, seq);
                } catch (const std::exception& error) {
                    spdlog::get("illixr")->error(
                        "[tcp_network_backend] Failed to deserialize topic={} bytes={}: {}; restart the session", topic_name,
                        message.size(), error.what());
                    running_ = false;
                    return;
                }

                buffer.erase(buffer.begin(), buffer.begin() + total_length);
            } else {
                break;
            }
        }
    }
}

void tcp_network_backend::topic_create(std::string topic_name, network::topic_config& config) {
    networked_topics_.push_back(topic_name);
    networked_topics_configs_[topic_name] = config;
    std::string serialization;
    if (config.serialization_method == network::topic_config::SerializationMethod::BOOST) {
        serialization = "BOOST";
    } else {
        serialization = "PROTOBUF";
    }
    std::string message = "create_topic" + topic_name + delimiter_ + serialization;
    send_to_peer("illixr_control", std::move(message));
}

bool tcp_network_backend::is_topic_networked(std::string topic_name) {
    return std::find(networked_topics_.begin(), networked_topics_.end(), topic_name) != networked_topics_.end();
}

void tcp_network_backend::topic_send(std::string topic_name, std::string&& message) {
    if (!is_topic_networked(topic_name)) {
        std::cout << "Topic not networked" << std::endl;
        return;
    }

    bool     track   = rtt_logger_.enabled();
    auto     sent_at = std::chrono::steady_clock::now();
    size_t   bytes   = message.size();
    uint64_t seq     = send_to_peer(topic_name, std::move(message));
    if (track) {
        pending_sends_.record(seq, network::pending_send_info{sent_at, topic_name, bytes});
    }
}

// Helper function to queue a received message into the corresponding topic
void tcp_network_backend::topic_receive(const std::string& topic_name, std::vector<char>& message, uint64_t seq) {
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

    if (topic_name == "illixr_control") {
        std::string message_str(message.begin(), message.end());
        // check if message starts with "create_topic"
        if (message_str.find("create_topic") == 0) {
            size_t d_pos = message_str.find(delimiter_);
            assert(d_pos != std::string::npos);
            std::string l_topic_name  = message_str.substr(12, d_pos - 12);
            std::string serialization = message_str.substr(d_pos + 1);
            networked_topics_.push_back(l_topic_name);
            network::topic_config config;
            if (serialization == "BOOST") {
                config.serialization_method = network::topic_config::SerializationMethod::BOOST;
            } else {
                config.serialization_method = network::topic_config::SerializationMethod::PROTOBUF;
            }
            networked_topics_configs_[l_topic_name] = config;
            std::cout << "Received create_topic for " << l_topic_name << std::endl;
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
        send_to_peer(network::LATENCY_ACK_TOPIC, std::move(ack_payload));
    }

    switchboard_->get_topic(topic_name).deserialize_and_put(message, networked_topics_configs_[topic_name]);
}

void tcp_network_backend::stop() {
    rtt_logger_.flush();
    std::lock_guard<std::mutex> lock{stop_mutex_};
    // shutdown() wakes read_loop without invalidating the descriptor; deletion
    // is deferred until the owning thread has returned. A read/send failure may
    // already have cleared running_, but its worker still needs to be joined.
    running_.store(false);
    if (peer_socket_ != nullptr) {
        peer_socket_->socket_shutdown();
    }
    if (io_thread_.joinable() && io_thread_.get_id() != std::this_thread::get_id()) {
        io_thread_.join();
    }
    delete peer_socket_;
    peer_socket_ = nullptr;
    plugin::stop();
}

tcp_network_backend::~tcp_network_backend() {
    stop();
}

uint64_t tcp_network_backend::send_to_peer(const std::string& topic_name, std::string&& message) {
    // packet are in the format
    // total_length:4bytes|topic_name_length:4bytes|topic_name|seq:8bytes|message
    uint64_t seq = next_seq_.fetch_add(1);

    uint32_t    total_length = 8 + network::SEQ_WIRE_SIZE + topic_name.size() + message.size();
    std::string packet;
    packet.append(reinterpret_cast<char*>(&total_length), 4);
    uint32_t topic_name_length = topic_name.size();
    packet.append(reinterpret_cast<char*>(&topic_name_length), 4);
    packet.append(topic_name);

    char seq_bytes[network::SEQ_WIRE_SIZE];
    std::memcpy(seq_bytes, &seq, network::SEQ_WIRE_SIZE);
    packet.append(seq_bytes, network::SEQ_WIRE_SIZE);

    packet.append(message.begin(), message.end());
    if (!running_)
        return seq;
    try {
        std::lock_guard<std::mutex> lock{send_mutex_};
        peer_socket_->write_data(packet);
    } catch (const std::exception& error) {
        running_ = false;
        spdlog::get("illixr")->error("[tcp_network_backend] TCP send failed for topic={}: {}; restart the session", topic_name,
                                     error.what());
    }
    return seq;
}

extern "C" MY_EXPORT_API plugin* this_plugin_factory(phonebook* pb) {
    auto* obj = new tcp_network_backend("tcp_network_backend", pb);
    // The runtime owns the plugin returned by this factory. Register a non-owning
    // service alias so the phonebook does not try to delete the same object again.
    pb->register_impl<network::tcp_backend>(
        std::shared_ptr<network::tcp_backend>(static_cast<network::tcp_backend*>(obj), [](network::tcp_backend*) { }));
    return obj;
}
