/**
 * @file common.h
 * @brief Shared data structures and utility functions used across all modules.
 *
 * Defines the FiveTuple struct (the session key), serialization helpers,
 * request/response structures, and the hash function for session lookups.
 */

#ifndef COMMON_H
#define COMMON_H

#include <string>
#include <sstream>
#include <functional>
#include <chrono>
#include <cstdint>

/**
 * @struct FiveTuple
 * @brief Represents a network 5-tuple that uniquely identifies a session.
 *
 * The 5-tuple consists of source IP, source port, destination IP,
 * destination port, and protocol. This is the session key used for
 * sticky session routing in the load balancer.
 */
struct FiveTuple {
    std::string src_ip;
    uint16_t    src_port;
    std::string dst_ip;
    uint16_t    dst_port;
    std::string protocol;   // "TCP" or "UDP"

    /**
     * @brief Generates a canonical string key for hashing/lookup.
     * Format: "src_ip:src_port->dst_ip:dst_port/protocol"
     */
    std::string toKey() const {
        return src_ip + ":" + std::to_string(src_port) + "->" +
               dst_ip + ":" + std::to_string(dst_port) + "/" + protocol;
    }

    /**
     * @brief Serializes the 5-tuple to a pipe-delimited string for network transmission.
     * Format: "src_ip|src_port|dst_ip|dst_port|protocol"
     */
    std::string serialize() const {
        return src_ip + "|" + std::to_string(src_port) + "|" +
               dst_ip + "|" + std::to_string(dst_port) + "|" + protocol;
    }

    /**
     * @brief Deserializes a pipe-delimited string back into a FiveTuple.
     * @param data The serialized string.
     * @return true if parsing succeeded, false on malformed input.
     */
    static bool deserialize(const std::string& data, FiveTuple& out) {
        std::istringstream ss(data);
        std::string token;
        std::vector<std::string> tokens;

        while (std::getline(ss, token, '|')) {
            tokens.push_back(token);
        }

        if (tokens.size() < 5) return false;

        out.src_ip   = tokens[0];
        out.dst_ip   = tokens[2];
        out.protocol = tokens[4];

        try {
            int sp = std::stoi(tokens[1]);
            int dp = std::stoi(tokens[3]);
            if (sp < 0 || sp > 65535 || dp < 0 || dp > 65535) return false;
            out.src_port = static_cast<uint16_t>(sp);
            out.dst_port = static_cast<uint16_t>(dp);
        } catch (...) {
            return false;
        }

        return true;
    }

    bool operator==(const FiveTuple& o) const {
        return src_ip == o.src_ip && src_port == o.src_port &&
               dst_ip == o.dst_ip && dst_port == o.dst_port &&
               protocol == o.protocol;
    }
};

/**
 * @brief Hash function for FiveTuple, enabling use in std::unordered_map.
 *
 * Combines hashes of all five fields using XOR and bit-shifting to produce
 * a well-distributed hash value.
 */
struct FiveTupleHash {
    std::size_t operator()(const FiveTuple& ft) const {
        std::size_t h = std::hash<std::string>{}(ft.src_ip);
        h ^= std::hash<uint16_t>{}(ft.src_port) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<std::string>{}(ft.dst_ip) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint16_t>{}(ft.dst_port) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<std::string>{}(ft.protocol) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

// ─────────────────────────────────────────────────────────────
// Timestamp helper
// ─────────────────────────────────────────────────────────────
using SteadyClock  = std::chrono::steady_clock;
using TimePoint    = std::chrono::steady_clock::time_point;
using SystemClock  = std::chrono::system_clock;

/**
 * @brief Returns the current wall-clock time as a human-readable string.
 */
inline std::string currentTimestamp() {
    auto now    = SystemClock::now();
    auto time_t = SystemClock::to_time_t(now);
    auto ms     = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()) % 1000;
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&time_t));
    std::string result(buf);
    result += "." + std::to_string(ms.count());
    return result;
}

// ─────────────────────────────────────────────────────────────
// Request / Response types
// ─────────────────────────────────────────────────────────────

/** Status codes for request processing outcomes */
enum class RequestStatus {
    SUCCESS,
    RATE_LIMITED,
    INVALID_INPUT,
    SERVER_DOWN,
    SESSION_BLOCKED,
    REASSIGNED,
    LB_FAILOVER,
    UNKNOWN_ERROR
};

/**
 * @brief Converts a RequestStatus to a human-readable string.
 */
inline std::string statusToString(RequestStatus s) {
    switch (s) {
        case RequestStatus::SUCCESS:        return "SUCCESS";
        case RequestStatus::RATE_LIMITED:    return "RATE_LIMITED";
        case RequestStatus::INVALID_INPUT:  return "INVALID_INPUT";
        case RequestStatus::SERVER_DOWN:    return "SERVER_DOWN";
        case RequestStatus::SESSION_BLOCKED:return "SESSION_BLOCKED";
        case RequestStatus::REASSIGNED:     return "REASSIGNED";
        case RequestStatus::LB_FAILOVER:    return "LB_FAILOVER";
        case RequestStatus::UNKNOWN_ERROR:  return "UNKNOWN_ERROR";
    }
    return "UNKNOWN";
}

/**
 * @struct PathHistoryEntry
 * @brief Records a single path assignment change for audit trail.
 */
struct PathHistoryEntry {
    int         backend_id;     // Assigned backend server ID
    std::string timestamp;      // When this assignment happened
    std::string reason;         // Why the path changed (initial, failover, rebalance, etc.)
};

#endif // COMMON_H
