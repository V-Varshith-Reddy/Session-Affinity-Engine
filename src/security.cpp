/**
 * @file security.cpp
 * @brief Implementation of security validation and spoof detection.
 *
 * IPv4 validation uses manual octet parsing (no regex dependency).
 * Spoof detection uses a sliding window of unique IPs per /24 subnet.
 */

#include "security.h"
#include "logger.h"
#include "../config.h"

#include <sstream>
#include <algorithm>
#include <set>

// ─────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────
Security::Security() {}

// ─────────────────────────────────────────────────────────────
// IPv4 validation — checks four octets, each 0-255
// ─────────────────────────────────────────────────────────────
bool Security::isValidIPv4(const std::string& ip) {
    if (ip.empty()) return false;

    std::istringstream ss(ip);
    std::string octet;
    int count = 0;

    while (std::getline(ss, octet, '.')) {
        if (octet.empty() || octet.size() > 3) return false;

        // Check all characters are digits
        for (char c : octet) {
            if (!std::isdigit(c)) return false;
        }

        // Check leading zeros (except "0" itself)
        if (octet.size() > 1 && octet[0] == '0') return false;

        int val;
        try {
            val = std::stoi(octet);
        } catch (...) {
            return false;
        }

        if (val < 0 || val > 255) return false;
        ++count;
    }

    return count == 4;
}

// ─────────────────────────────────────────────────────────────
// Extract /24 subnet
// ─────────────────────────────────────────────────────────────
std::string Security::getSubnet(const std::string& ip) {
    auto last_dot = ip.rfind('.');
    if (last_dot == std::string::npos) return ip;
    return ip.substr(0, last_dot);
}

// ─────────────────────────────────────────────────────────────
// Validate a FiveTuple
// ─────────────────────────────────────────────────────────────
ValidationResult Security::validateTuple(const FiveTuple& ft) {
    // Validate source IP
    if (!isValidIPv4(ft.src_ip)) {
        return {false, "Invalid source IP: " + ft.src_ip};
    }

    // Validate destination IP
    if (!isValidIPv4(ft.dst_ip)) {
        return {false, "Invalid destination IP: " + ft.dst_ip};
    }

    // Validate source port (1-65535)
    if (ft.src_port == 0) {
        return {false, "Invalid source port: 0 (must be 1-65535)"};
    }

    // Validate destination port (1-65535)
    if (ft.dst_port == 0) {
        return {false, "Invalid destination port: 0 (must be 1-65535)"};
    }

    // Validate protocol (whitelist: TCP, UDP only)
    std::string proto = ft.protocol;
    std::transform(proto.begin(), proto.end(), proto.begin(), ::toupper);
    if (proto != "TCP" && proto != "UDP") {
        return {false, "Invalid protocol: " + ft.protocol + " (must be TCP or UDP)"};
    }

    return {true, ""};
}

// ─────────────────────────────────────────────────────────────
// Validate payload size
// ─────────────────────────────────────────────────────────────
ValidationResult Security::validatePayloadSize(const std::string& payload, int max_size) {
    if (static_cast<int>(payload.size()) > max_size) {
        return {false, "Payload too large: " + std::to_string(payload.size()) +
                       " bytes (max " + std::to_string(max_size) + ")"};
    }
    if (payload.empty()) {
        return {false, "Empty payload"};
    }
    return {true, ""};
}

// ─────────────────────────────────────────────────────────────
// Spoof detection — track unique IPs per /24 subnet
// ─────────────────────────────────────────────────────────────
ValidationResult Security::checkSpoofing(const std::string& src_ip) {
    std::lock_guard<std::mutex> lock(spoof_mutex_);

    std::string subnet = getSubnet(src_ip);
    auto now = std::chrono::steady_clock::now();
    auto window = std::chrono::seconds(SPOOF_DETECTION_WINDOW_SEC);

    auto& tracker = subnet_trackers_[subnet];

    // Evict expired entries
    while (!tracker.entries.empty() &&
           (now - tracker.entries.front().first) > window) {
        tracker.entries.pop_front();
    }

    // Add current IP
    tracker.entries.push_back({now, src_ip});

    // Count unique IPs in the window
    std::set<std::string> unique_ips;
    for (const auto& entry : tracker.entries) {
        unique_ips.insert(entry.second);
    }

    if (static_cast<int>(unique_ips.size()) > SPOOF_DETECTION_THRESHOLD) {
        Logger::instance().logSystem(LogLevel::WARNING,
            "SPOOF DETECTED: " + std::to_string(unique_ips.size()) +
            " unique IPs from subnet " + subnet + ".0/24 in " +
            std::to_string(SPOOF_DETECTION_WINDOW_SEC) + "s window");

        return {false, "Potential IP spoofing detected from subnet " +
                       subnet + ".0/24 (" + std::to_string(unique_ips.size()) +
                       " unique source IPs)"};
    }

    return {true, ""};
}

// ─────────────────────────────────────────────────────────────
// Full validation pipeline — fail on first error
// ─────────────────────────────────────────────────────────────
ValidationResult Security::fullValidation(const FiveTuple& ft, const std::string& payload) {
    // 1. Payload size check
    auto r1 = validatePayloadSize(payload, MAX_PAYLOAD_SIZE);
    if (!r1.valid) {
        Logger::instance().logRequest(ft.toKey(), "SECURITY_CHECK", "REJECTED", r1.error_message);
        return r1;
    }

    // 2. Tuple validation
    auto r2 = validateTuple(ft);
    if (!r2.valid) {
        Logger::instance().logRequest(ft.toKey(), "SECURITY_CHECK", "REJECTED", r2.error_message);
        return r2;
    }

    // 3. Spoof detection
    auto r3 = checkSpoofing(ft.src_ip);
    if (!r3.valid) {
        Logger::instance().logRequest(ft.toKey(), "SECURITY_CHECK", "REJECTED", r3.error_message);
        return r3;
    }

    return {true, ""};
}
