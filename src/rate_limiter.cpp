/**
 * @file rate_limiter.cpp
 * @brief Implementation of the sliding window queue-based rate limiter.
 *
 * The sliding window approach works as follows:
 *   - Each client has a deque of RequestRecords
 *   - On each request, we first remove all records whose timestamp
 *     is older than (now - window_seconds)
 *   - Then check if the remaining count exceeds max_requests
 *   - This gives us a true sliding window (not fixed intervals)
 */

#include "rate_limiter.h"
#include "logger.h"
#include "common.h"

// ─────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────
RateLimiter::RateLimiter(int window_seconds, int max_requests)
    : window_seconds_(window_seconds), max_requests_(max_requests) {}

// ─────────────────────────────────────────────────────────────
// Evict expired entries from the front of the deque
// ─────────────────────────────────────────────────────────────
void RateLimiter::evictExpired(std::deque<RequestRecord>& q,
                               std::chrono::steady_clock::time_point now) {
    auto window = std::chrono::seconds(window_seconds_);
    while (!q.empty() && (now - q.front().timestamp) > window) {
        q.pop_front();
    }
}

// ─────────────────────────────────────────────────────────────
// Check if a request is allowed
// ─────────────────────────────────────────────────────────────
bool RateLimiter::allowRequest(const std::string& client_key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();

    auto& q = client_queues_[client_key];

    // Step 1: Evict expired entries outside the window
    evictExpired(q, now);

    // Step 2: Check if adding this request would exceed the limit
    if (static_cast<int>(q.size()) >= max_requests_) {
        // Rate limit exceeded — record the rejection
        RequestRecord record;
        record.timestamp = now;
        record.status    = "REJECTED";
        record.reason    = "Rate limit exceeded (" +
                           std::to_string(max_requests_) + " req/" +
                           std::to_string(window_seconds_) + "s)";
        record.wall_time = currentTimestamp();
        q.push_back(record);

        Logger::instance().logRequest(client_key, "RATE_LIMIT",
            "REJECTED", record.reason);

        return false;
    }

    // Step 3: Allow the request — record it in the queue
    RequestRecord record;
    record.timestamp = now;
    record.status    = "ALLOWED";
    record.reason    = "";
    record.wall_time = currentTimestamp();
    q.push_back(record);

    return true;
}

// ─────────────────────────────────────────────────────────────
// Get request history for a client
// ─────────────────────────────────────────────────────────────
std::vector<RequestRecord> RateLimiter::getHistory(const std::string& client_key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = client_queues_.find(client_key);
    if (it == client_queues_.end()) return {};

    return std::vector<RequestRecord>(it->second.begin(), it->second.end());
}

// ─────────────────────────────────────────────────────────────
// Current count in window
// ─────────────────────────────────────────────────────────────
int RateLimiter::currentCount(const std::string& client_key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = client_queues_.find(client_key);
    if (it == client_queues_.end()) return 0;
    return static_cast<int>(it->second.size());
}

// ─────────────────────────────────────────────────────────────
// Reset all state
// ─────────────────────────────────────────────────────────────
void RateLimiter::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    client_queues_.clear();
}
