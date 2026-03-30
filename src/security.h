/**
 * @file security.h
 * @brief Input validation and security checks for the Session Affinity Engine.
 *
 * Provides defense-in-depth against:
 *   1. Malformed 5-tuple data (invalid IPs, ports, protocols)
 *   2. Oversized payloads (buffer overflow prevention)
 *   3. IP spoofing detection (too many unique IPs from same subnet)
 *   4. Abusive traffic patterns (handled via rate limiter integration)
 */

#ifndef SECURITY_H
#define SECURITY_H

#include "common.h"
#include <string>
#include <unordered_map>
#include <deque>
#include <chrono>
#include <mutex>

/**
 * @struct ValidationResult
 * @brief Result of a security validation check.
 */
struct ValidationResult {
    bool        valid;
    std::string error_message;
};

/**
 * @class Security
 * @brief Validates incoming requests and detects potential attacks.
 *
 * Security measures implemented:
 *   - IPv4 format validation (four octets, each 0-255)
 *   - Port range validation (1-65535)
 *   - Protocol whitelist (TCP, UDP only)
 *   - Payload size enforcement
 *   - Spoof detection via per-subnet unique IP tracking
 */
class Security {
public:
    Security();

    /**
     * @brief Validates a FiveTuple for correctness.
     * @param ft The 5-tuple to validate.
     * @return ValidationResult with valid flag and error message if invalid.
     */
    ValidationResult validateTuple(const FiveTuple& ft);

    /**
     * @brief Validates raw payload size.
     * @param payload The raw request payload string.
     * @param max_size Maximum allowed size in bytes.
     * @return ValidationResult.
     */
    ValidationResult validatePayloadSize(const std::string& payload, int max_size);

    /**
     * @brief Checks for potential IP spoofing from a given source IP.
     *
     * Tracks unique IPs per /24 subnet within a time window. If too many
     * unique IPs appear from the same subnet, it flags potential spoofing.
     *
     * @param src_ip The source IP to check.
     * @return ValidationResult — invalid if spoof detected.
     */
    ValidationResult checkSpoofing(const std::string& src_ip);

    /**
     * @brief Runs all validation checks on a tuple and payload.
     * @param ft      The 5-tuple.
     * @param payload The raw payload string.
     * @return ValidationResult — first failure stops the chain.
     */
    ValidationResult fullValidation(const FiveTuple& ft, const std::string& payload);

private:
    /**
     * @brief Validates an IPv4 address string.
     * @param ip The IP address to validate.
     * @return true if valid IPv4 format.
     */
    bool isValidIPv4(const std::string& ip);

    /**
     * @brief Extracts the /24 subnet prefix from an IPv4 address.
     * @param ip The full IP address.
     * @return The first three octets (e.g., "192.168.1").
     */
    std::string getSubnet(const std::string& ip);

    // Spoof detection: tracks unique IPs per subnet in a time window
    struct SubnetTracker {
        std::deque<std::pair<std::chrono::steady_clock::time_point, std::string>> entries;
    };

    std::unordered_map<std::string, SubnetTracker> subnet_trackers_;
    std::mutex spoof_mutex_;
};

#endif // SECURITY_H
