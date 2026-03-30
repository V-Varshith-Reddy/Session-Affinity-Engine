/**
 * @file load_balancer.cpp
 * @brief Implementation of the LoadBalancer with request pipeline and failover.
 *
 * Request processing pipeline:
 *   1. Active check — only active LB processes requests
 *   2. Security validation — malformed input rejection
 *   3. Rate limiting — sliding window queue check
 *   4. Session routing — sticky session lookup via SessionManager
 *   5. Backend forwarding — forward to assigned RequestHandler
 *   6. Response relay — return result to caller
 */

#include "load_balancer.h"
#include "logger.h"
#include "../config.h"

// ─────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────
LoadBalancer::LoadBalancer(LBRole role,
                           SessionManager& session_mgr,
                           ConsistentHashRing& hash_ring,
                           RateLimiter& rate_limiter,
                           Security& security,
                           std::vector<std::unique_ptr<RequestHandler>>& backends)
    : role_(role)
    , active_(role == LBRole::PRIMARY)  // Primary starts active, standby starts passive
    , session_mgr_(session_mgr)
    , hash_ring_(hash_ring)
    , rate_limiter_(rate_limiter)
    , security_(security)
    , backends_(backends)
    , last_heartbeat_(std::chrono::steady_clock::now())
{
    std::string role_str = (role == LBRole::PRIMARY) ? "PRIMARY" : "STANDBY";
    Logger::instance().logSystem(LogLevel::INFO,
        "LoadBalancer [" + role_str + "] initialized on port " +
        std::to_string(getPort()) + " — " +
        (active_.load() ? "ACTIVE" : "PASSIVE"));
}

// ─────────────────────────────────────────────────────────────
// Handle a client request — full pipeline
// ─────────────────────────────────────────────────────────────
std::string LoadBalancer::handleRequest(const FiveTuple& ft, const std::string& payload) {
    std::string role_str = (role_ == LBRole::PRIMARY) ? "PRIMARY" : "STANDBY";
    std::string key = ft.toKey();

    // ─── Step 1: Check if this LB is active ───
    if (!active_.load()) {
        Logger::instance().logRequest(key, "LB_CHECK", "REJECTED",
            role_str + " LB is currently PASSIVE (not routing)");
        return "ERROR: Load balancer [" + role_str + "] is not active";
    }

    // ─── Step 2: Security validation ───
    auto validation = security_.fullValidation(ft, payload);
    if (!validation.valid) {
        Logger::instance().logRequest(key, "SECURITY", "REJECTED", validation.error_message);
        return "ERROR: Security validation failed — " + validation.error_message;
    }

    // ─── Step 3: Rate limiting ───
    if (!rate_limiter_.allowRequest(key)) {
        return "ERROR: Rate limit exceeded for session " + key +
               ". Max " + std::to_string(RATE_LIMIT_MAX_REQUESTS) +
               " requests per " + std::to_string(RATE_LIMIT_WINDOW_SEC) + "s window.";
    }

    // ─── Step 4: Session routing (sticky session lookup) ───
    int assigned_backend = -1;
    RequestStatus status = session_mgr_.routeRequest(ft, assigned_backend);

    if (status == RequestStatus::SESSION_BLOCKED) {
        return "ERROR: Session " + key + " is blocked by administrator policy";
    }

    if (status == RequestStatus::SERVER_DOWN) {
        return "ERROR: No backend servers available to handle request";
    }

    // ─── Step 5: Forward to the assigned backend ───
    if (assigned_backend < 0 || assigned_backend >= static_cast<int>(backends_.size())) {
        Logger::instance().logRequest(key, "ROUTE", "ERROR",
            "Invalid backend ID: " + std::to_string(assigned_backend));
        return "ERROR: Internal routing error — invalid backend ID";
    }

    RequestHandler* backend_ptr = backends_[assigned_backend].get();

    // Check if backend is actually online (it might have gone down between routing and forwarding)
    if (!backend_ptr->isOnline()) {
        // Backend went down — trigger failover
        Logger::instance().logSystem(LogLevel::WARNING,
            "Backend " + std::to_string(assigned_backend) +
            " is offline during forwarding. Triggering failover...");

        // Remove from hash ring and reassign
        hash_ring_.removeServer(assigned_backend);
        session_mgr_.handleServerDown(assigned_backend);

        // Try routing again
        status = session_mgr_.routeRequest(ft, assigned_backend);
        if (status == RequestStatus::SERVER_DOWN || assigned_backend < 0) {
            return "ERROR: No backend servers available after failover";
        }

        backend_ptr = backends_[assigned_backend].get();
        if (!backend_ptr || !backend_ptr->isOnline()) {
            return "ERROR: Failover target also unavailable";
        }
    }

    // Process the request on the backend
    std::string response = backend_ptr->processRequest(ft);

    // Note if the session was reassigned (failover happened)
    if (status == RequestStatus::REASSIGNED) {
        response += " [REASSIGNED — previous backend was down]";
    }

    Logger::instance().logRequest(key, "COMPLETE", statusToString(status),
        "Routed to backend " + std::to_string(assigned_backend) +
        " via " + role_str + " LB");

    return response;
}

// ─────────────────────────────────────────────────────────────
// Heartbeat
// ─────────────────────────────────────────────────────────────
void LoadBalancer::sendHeartbeat() {
    std::lock_guard<std::mutex> lock(hb_mutex_);
    last_heartbeat_ = std::chrono::steady_clock::now();
}

std::chrono::steady_clock::time_point LoadBalancer::getLastHeartbeat() const {
    std::lock_guard<std::mutex> lock(hb_mutex_);
    return last_heartbeat_;
}

bool LoadBalancer::shouldTakeover(std::chrono::steady_clock::time_point primary_last_hb) const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - primary_last_hb);
    return elapsed.count() > LB_FAILOVER_THRESHOLD_MS;
}

// ─────────────────────────────────────────────────────────────
// Activation / Deactivation
// ─────────────────────────────────────────────────────────────
void LoadBalancer::activate() {
    active_.store(true);
    std::string role_str = (role_ == LBRole::PRIMARY) ? "PRIMARY" : "STANDBY";
    Logger::instance().logSystem(LogLevel::INFO,
        "LoadBalancer [" + role_str + "] is now ACTIVE");
}

void LoadBalancer::deactivate() {
    active_.store(false);
    std::string role_str = (role_ == LBRole::PRIMARY) ? "PRIMARY" : "STANDBY";
    Logger::instance().logSystem(LogLevel::WARNING,
        "LoadBalancer [" + role_str + "] is now PASSIVE");
}

int LoadBalancer::getPort() const {
    return (role_ == LBRole::PRIMARY) ? LB_PRIMARY_PORT : LB_STANDBY_PORT;
}

// ─────────────────────────────────────────────────────────────
// Backend management
// ─────────────────────────────────────────────────────────────
void LoadBalancer::takeBackendOffline(int server_id) {
    if (server_id < 0 || server_id >= static_cast<int>(backends_.size())) return;

    backends_[server_id]->stop();
    hash_ring_.removeServer(server_id);
    session_mgr_.handleServerDown(server_id);

    Logger::instance().logSystem(LogLevel::WARNING,
        "Backend " + std::to_string(server_id) +
        " taken offline. Sessions redistributed. Active backends: " +
        std::to_string(hash_ring_.serverCount()));
}

void LoadBalancer::bringBackendOnline(int server_id) {
    if (server_id < 0 || server_id >= static_cast<int>(backends_.size())) return;

    backends_[server_id]->start();
    hash_ring_.addServer(server_id);

    Logger::instance().logSystem(LogLevel::INFO,
        "Backend " + std::to_string(server_id) +
        " brought back online. Active backends: " +
        std::to_string(hash_ring_.serverCount()));
}
