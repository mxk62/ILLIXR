#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ILLIXR::network {

// Size, in bytes, of the sequence-number field tcp_network_backend and
// biphony_network_backend prepend to every message's wire frame, right
// after the topic name. Always present on the wire (not gated by whether
// latency logging is enabled) so the two ends of a connection can never
// disagree about frame layout. Used only to correlate a round-trip ack back
// to its original send.
inline constexpr size_t SEQ_WIRE_SIZE = 8;

// Reserved topic name used for round-trip acknowledgments: the receiver of a
// real-topic message echoes the message's seq back on this topic so the
// original sender can compute RTT purely from its own clock (no cross-host
// clock-sync assumption).
inline const std::string LATENCY_ACK_TOPIC = "illixr_latency_ack";

// Writes one CSV row per round-trip-acked message. Disabled (every call is a
// no-op) unless constructed with a non-empty path.
//
// Must be flushed explicitly (call flush()) from the owning plugin's stop()
// -- ILLIXR's shutdown path does not reliably run object destructors
// (runtime teardown / dlclose / SIGINT only calling stop()), so relying on
// ~rtt_csv_logger() alone can silently lose buffered rows.
class rtt_csv_logger {
public:
    rtt_csv_logger() = default;

    explicit rtt_csv_logger(const std::string& path) {
        if (path.empty()) {
            return;
        }
        file_.open(path, std::ios::out | std::ios::trunc);
        if (!file_.is_open()) {
            std::cerr << "[rtt_csv_logger] failed to open CSV output at '" << path << "' -- latency logging disabled"
                      << std::endl;
            return;
        }
        file_ << "seq,topic_name,message_bytes,send_time_ms_since_start,rtt_ms\n";
        file_.flush();
        enabled_ = true;
    }

    bool enabled() const {
        return enabled_;
    }

    // send_time_ms_since_start is the sender's send instant, relative to its
    // own process start (steady clock) -- purely for a readable x-axis, no
    // role in the rtt_ms computation itself.
    void log(uint64_t seq, const std::string& topic_name, size_t message_bytes, double send_time_ms_since_start,
             double rtt_ms) {
        if (!enabled_) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        file_ << seq << ',' << topic_name << ',' << message_bytes << ',' << send_time_ms_since_start << ',' << rtt_ms
              << '\n';
    }

    void flush() {
        if (!enabled_) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        file_.flush();
    }

    ~rtt_csv_logger() {
        flush();
    }

private:
    bool          enabled_ = false;
    std::mutex    mutex_;
    std::ofstream file_;
};

// Bookkeeping for one in-flight send awaiting an RTT ack.
struct pending_send_info {
    std::chrono::steady_clock::time_point sent_at;
    std::string                           topic_name;
    size_t                                bytes = 0;
};

// Tracks sends awaiting an RTT ack, keyed by the frame's sequence number.
// Entries that never get acked are pruned so the map can't grow unboundedly
// if acks are dropped.
class pending_send_tracker {
public:
    void record(uint64_t seq, pending_send_info info) {
        std::lock_guard<std::mutex> lock(mutex_);
        prune_locked();
        pending_[seq] = std::move(info);
    }

    // On a match, fills `out` with the original send info and removes the
    // entry. Returns false if seq is unknown (already pruned, or a stray
    // ack).
    bool complete(uint64_t seq, pending_send_info& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto                        it = pending_.find(seq);
        if (it == pending_.end()) {
            return false;
        }
        out = std::move(it->second);
        pending_.erase(it);
        return true;
    }

private:
    static constexpr std::chrono::seconds STALE_AFTER{5};

    void prune_locked() {
        auto now = std::chrono::steady_clock::now();
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (now - it->second.sent_at > STALE_AFTER) {
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::mutex                                      mutex_;
    std::unordered_map<uint64_t, pending_send_info> pending_;
};

} // namespace ILLIXR::network
