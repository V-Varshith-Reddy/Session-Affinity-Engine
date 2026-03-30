/**
 * @file load_balancer.h
 * @brief Primary and standby load balancer with heartbeat-based failover.
 *
 * The load balancer is the central routing component:
 *   1. Receives client requests (5-tuples)
 *   2. Validates via Security module
 *   3. Checks rate limits via RateLimiter
 *   4. Routes via SessionManager (which uses ConsistentHashRing)
 *   5. Forwards to the appropriate backend RequestHandler
 *   6. Returns the response to the client
 *
 * Failover:
 *   - Primary LB sends heartbeats to standby at LB_HEARTBEAT_INTERVAL_MS
 *   - If standby doesn't see heartbeat for LB_FAILOVER_THRESHOLD_MS, it takes over
 *   - Standby shares the same SessionManager and ConsistentHashRing
 */

#ifndef LOAD_BALANCER_H
#define LOAD_BALANCER_H

#include "common.h"
#include "session_manager.h"
#include "consistent_hash.h"
#include "rate_limiter.h"
#include "security.h"
#include "request_handler.h"

#include <vector>
#include <memory>
#include <atomic>
#include <chrono>
#include <string>
#include <mutex>

/**
 * @enum LBRole
 * @brief Whether this load balancer instance is primary or standby.
 */
enum class LBRole {
    PRIMARY,
    STANDBY
};

/**
 * @class LoadBalancer
 * @brief Routes requests to backends with session affinity, rate limiting, and security.
 *
 * Design: Both primary and standby LBs share references to the same
 * SessionManager, ConsistentHashRing, backend servers, RateLimiter, and
 * Security module. This simulates a shared-state failover (in production,
 * this would be via a shared database or state sync protocol).
 */
class LoadBalancer {
public:
    /**
     * @brief Constructs a load balancer.
     * @param role             PRIMARY or STANDBY.
     * @param session_mgr      Shared session manager.
     * @param hash_ring        Shared consistent hash ring.
     * @param rate_limiter     Shared rate limiter.
     * @param security         Shared security module.
     * @param backends         Vector of backend server instances.
     */
    LoadBalancer(LBRole role,
                 SessionManager& session_mgr,
                 ConsistentHashRing& hash_ring,
                 RateLimiter& rate_limiter,
                 Security& security,
                 std::vector<std::unique_ptr<RequestHandler>>& backends);

    /**
     * @brief Processes a client request through the full pipeline.
     *
     * Pipeline:
     *   1. Check if this LB is active
     *   2. Validate the request (security)
     *   3. Check rate limit
     *   4. Route via session manager (sticky session)
     *   5. Forward to backend and get response
     *   6. Return response to client
     *
     * @param ft      The 5-tuple from the client.
     * @param payload The raw payload string (for security validation).
     * @return Response string to send back to the client.
     */
    std::string handleRequest(const FiveTuple& ft, const std::string& payload);

    /**
     * @brief Sends a heartbeat (called periodically by primary).
     * Updates the last_heartbeat timestamp visible to the standby.
     */
    void sendHeartbeat();

    /**
     * @brief Checks if the primary is still alive (called by standby).
     * @param primary_last_heartbeat The timestamp of the last heartbeat from primary.
     * @return true if the primary has timed out and failover should occur.
     */
    bool shouldTakeover(std::chrono::steady_clock::time_point primary_last_heartbeat) const;

    /**
     * @brief Activates this LB (transitions to active routing state).
     */
    void activate();

    /**
     * @brief Deactivates this LB (stops routing, becomes passive).
     */
    void deactivate();

    /**
     * @brief Returns whether this LB is currently active.
     */
    bool isActive() const { return active_.load(); }

    /**
     * @brief Returns the role of this LB (PRIMARY or STANDBY).
     */
    LBRole getRole() const { return role_; }

    /**
     * @brief Returns the port this LB listens on.
     */
    int getPort() const;

    /**
     * @brief Simulates taking a backend server offline and redistributing.
     * @param server_id The backend server to take down.
     */
    void takeBackendOffline(int server_id);

    /**
     * @brief Simulates bringing a backend server back online.
     * @param server_id The backend server to bring up.
     */
    void bringBackendOnline(int server_id);

    /**
     * @brief Returns the last heartbeat timestamp.
     */
    std::chrono::steady_clock::time_point getLastHeartbeat() const;

private:
    LBRole                                          role_;
    std::atomic<bool>                               active_;
    SessionManager&                                 session_mgr_;
    ConsistentHashRing&                             hash_ring_;
    RateLimiter&                                    rate_limiter_;
    Security&                                       security_;
    std::vector<std::unique_ptr<RequestHandler>>&   backends_;

    std::chrono::steady_clock::time_point           last_heartbeat_;
    mutable std::mutex                              hb_mutex_;
};

#endif // LOAD_BALANCER_H
