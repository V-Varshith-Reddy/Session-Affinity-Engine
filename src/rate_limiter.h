/**
 * @file rate_limiter.h
 * @brief Sliding window queue-based rate limiter.
 *
 * For each client (identified by their 5-tuple key), maintains a queue of
 * request timestamps. On each incoming request:
 *   1. Pop expired timestamps from the front (outside the window)
 *   2. Check if queue size >= RATE_LIMIT_MAX_REQUESTS
 *   3. If under limit, push current timestamp and allow; else reject
 *
 * This queue also serves as the request history / audit trail.
 */

#ifndef RATE_LIMITER_H
#define RATE_LIMITER_H

#include <string>
#include <queue>
#include <unordered_map>
#include <chrono>
#include <mutex>
#include <vector>

/**
 * @struct RequestRecord
 * @brief A single request record used for both rate limiting and history tracking.
 */
struct RequestRecord {
    std::chrono::steady_clock::time_point timestamp;
    std::string                           status;    // "ALLOWED" or "REJECTED"
    std::string                           reason;    // Reason if rejected
    std::string                           wall_time; // Human-readable timestamp
};

/**
 * @class RateLimiter
 * @brief Per-client sliding window rate limiter using a queue of timestamps.
 *
 * Design choice: A queue is used instead of a simple counter because:
 *   1. It naturally provides request history for logging/auditing
 *   2. The sliding window approach is more fair than fixed windows
 *   3. Memory is bounded (max RATE_LIMIT_MAX_REQUESTS entries per client)
 */
class RateLimiter {
public:
    /**
     * @brief Constructs a rate limiter.
     * @param window_seconds  Duration of the sliding window.
     * @param max_requests    Maximum requests allowed in the window.
     */
    RateLimiter(int window_seconds, int max_requests);

    /**
     * @brief Checks if a request from the given client is allowed.
     *
     * Evicts expired entries first, then checks the count.
     *
     * @param client_key The 5-tuple key identifying the client.
     * @return true if the request is allowed, false if rate-limited.
     */
    bool allowRequest(const std::string& client_key);

    /**
     * @brief Returns the recent request history for a given client.
     * @param client_key The 5-tuple key.
     * @return Vector of RequestRecord entries.
     */
    std::vector<RequestRecord> getHistory(const std::string& client_key) const;

    /**
     * @brief Returns the current request count in the window for a client.
     */
    int currentCount(const std::string& client_key) const;

    /**
     * @brief Clears all rate limiting state (used in testing or reset).
     */
    void reset();

private:
    int window_seconds_;
    int max_requests_;

    // Per-client queue of request timestamps for the sliding window
    std::unordered_map<std::string, std::deque<RequestRecord>> client_queues_;
    mutable std::mutex mutex_;

    /**
     * @brief Evicts entries older than the sliding window from the front of the queue.
     */
    void evictExpired(std::deque<RequestRecord>& q,
                      std::chrono::steady_clock::time_point now);
};

#endif // RATE_LIMITER_H
