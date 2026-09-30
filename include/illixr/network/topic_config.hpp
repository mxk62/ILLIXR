#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>

namespace ILLIXR::network {

struct topic_config {
    enum priority_type { LOWEST, LOW, MEDIUM, HIGH, HIGHEST };

    enum packetization_type { IMMEDIATE, DEFAULT, SUGGEST_LATENCY };

    priority_type                                         priority           = MEDIUM;
    bool                                                  retransmit         = false;
    bool                                                  allow_out_of_order = false;
    packetization_type                                    packetization      = DEFAULT;
    std::optional<std::chrono::duration<long, std::nano>> latency;

    enum SerializationMethod { BOOST, PROTOBUF } serialization_method;

    enum TransportMethod { TCP, UDP, BIPHONY } transport_method;

    topic_config()
        : serialization_method{BOOST}
        , transport_method{TCP} { }

    explicit topic_config(SerializationMethod method, TransportMethod transport = TCP,
                          std::chrono::duration<long, std::nano> late = std::chrono::duration<long, std::nano>{})
        : latency{late}
        , serialization_method{method}
        , transport_method{transport} { }
};

/**
 * @brief Parse a transport name ("tcp", "udp" or "biphony", case-insensitive) into a TransportMethod.
 *
 * An empty string yields TCP, so an unset environment variable keeps the default transport.
 *
 * @throws std::invalid_argument if the name is not recognized.
 */
inline topic_config::TransportMethod parse_transport_method(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (name.empty() || name == "tcp")
        return topic_config::TCP;
    if (name == "udp")
        return topic_config::UDP;
    if (name == "biphony")
        return topic_config::BIPHONY;
    throw std::invalid_argument("Unknown network transport '" + name + "' (expected tcp, udp or biphony)");
}

} // namespace ILLIXR::network
