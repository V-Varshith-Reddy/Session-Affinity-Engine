/**
 * @file session_manager.cpp
 * @brief Implementation of the SessionManager for sticky session routing.
 *
 * Core logic:
 *   1. On routeRequest: check if session exists → if yes, check expiry → if valid, return same backend (sticky)
 *   2. If session expired or doesn't exist → assign via consistent hash ring
 *   3. If assigned backend is no longer in the ring (server down) → reassign and log failover
 *   4. All path changes are recorded in the session's path_history
 */

#include "session_manager.h"
#include "logger.h"

// ─────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────
SessionManager::SessionManager(ConsistentHashRing& hash_ring, int default_timeout_sec)
    : hash_ring_(hash_ring), default_timeout_sec_(default_timeout_sec) {}

// ─────────────────────────────────────────────────────────────
// Check expiry
// ─────────────────────────────────────────────────────────────
bool SessionManager::isExpired(const SessionEntry& entry) const {
    int timeout = (entry.custom_timeout >= 0) ? entry.custom_timeout : default_timeout_sec_;
    auto elapsed = std::chrono::steady_clock::now() - entry.last_access;
    return elapsed > std::chrono::seconds(timeout);
}

// ─────────────────────────────────────────────────────────────
// Route a request — core sticky session logic
// ─────────────────────────────────────────────────────────────
RequestStatus SessionManager::routeRequest(const FiveTuple& ft, int& assigned_backend) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string key = ft.toKey();
    auto now = std::chrono::steady_clock::now();

    auto it = sessions_.find(key);

    // ─── Case 1: Existing session found ───
    if (it != sessions_.end()) {
        SessionEntry& entry = it->second;

        // Check if session is blocked
        if (entry.blocked) {
            Logger::instance().logRequest(key, "ROUTE", "BLOCKED",
                "Session is blocked by admin policy");
            return RequestStatus::SESSION_BLOCKED;
        }

        // Check if session has expired
        if (isExpired(entry)) {
            Logger::instance().logSystem(LogLevel::INFO,
                "Session expired: " + key + " (was on backend " +
                std::to_string(entry.backend_id) + ")");
            sessions_.erase(it);
            // Fall through to create new session below
        } else {
            // Check if the assigned backend is still alive
            auto active = hash_ring_.getActiveServers();
            bool backend_alive = false;
            for (int id : active) {
                if (id == entry.backend_id) {
                    backend_alive = true;
                    break;
                }
            }

            if (backend_alive) {
                // ─── STICKY: Return same backend ───
                entry.last_access = now;
                assigned_backend = entry.backend_id;

                Logger::instance().logRequest(key, "ROUTE", "SUCCESS",
                    "Sticky session -> backend " + std::to_string(assigned_backend));
                return RequestStatus::SUCCESS;
            } else {
                // ─── FAILOVER: Backend is down, reassign ───
                int old_backend = entry.backend_id;
                int new_backend = hash_ring_.getServer(key);

                if (new_backend < 0) {
                    Logger::instance().logRequest(key, "ROUTE", "FAILED",
                        "No backends available for failover");
                    return RequestStatus::SERVER_DOWN;
                }

                entry.backend_id = new_backend;
                entry.last_access = now;

                PathHistoryEntry hist;
                hist.backend_id = new_backend;
                hist.timestamp  = currentTimestamp();
                hist.reason     = "Failover from backend " + std::to_string(old_backend) +
                                  " (server down)";
                entry.path_history.push_back(hist);

                assigned_backend = new_backend;

                Logger::instance().logRequest(key, "FAILOVER", "REASSIGNED",
                    "Backend " + std::to_string(old_backend) + " -> " +
                    std::to_string(new_backend));

                return RequestStatus::REASSIGNED;
            }
        }
    }

    // ─── Case 2: New session (or expired session was just removed) ───
    int backend = hash_ring_.getServer(key);
    if (backend < 0) {
        Logger::instance().logRequest(key, "ROUTE", "FAILED",
            "No backends available for new session");
        return RequestStatus::SERVER_DOWN;
    }

    SessionEntry new_entry;
    new_entry.tuple          = ft;
    new_entry.backend_id     = backend;
    new_entry.blocked        = false;
    new_entry.custom_timeout = -1;  // Use default
    new_entry.created_at     = now;
    new_entry.last_access    = now;

    PathHistoryEntry hist;
    hist.backend_id = backend;
    hist.timestamp  = currentTimestamp();
    hist.reason     = "Initial assignment";
    new_entry.path_history.push_back(hist);

    sessions_[key] = new_entry;
    assigned_backend = backend;

    Logger::instance().logRequest(key, "NEW_SESSION", "SUCCESS",
        "Assigned to backend " + std::to_string(backend));

    return RequestStatus::SUCCESS;
}

// ─────────────────────────────────────────────────────────────
// Force reassignment
// ─────────────────────────────────────────────────────────────
bool SessionManager::forceReassign(const std::string& tuple_key) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = sessions_.find(tuple_key);
    if (it == sessions_.end()) return false;

    SessionEntry& entry = it->second;
    int old_backend = entry.backend_id;
    int new_backend = hash_ring_.getServer(tuple_key);

    if (new_backend < 0) return false;

    // If consistent hash gives the same server, try to find a different one
    // by appending a salt to the key
    if (new_backend == old_backend) {
        auto servers = hash_ring_.getActiveServers();
        if (servers.size() <= 1) return false;  // Only one server, can't reassign

        // Pick next server in the list
        for (int s : servers) {
            if (s != old_backend) {
                new_backend = s;
                break;
            }
        }
    }

    entry.backend_id = new_backend;
    entry.last_access = std::chrono::steady_clock::now();

    PathHistoryEntry hist;
    hist.backend_id = new_backend;
    hist.timestamp  = currentTimestamp();
    hist.reason     = "Forced reassignment from backend " + std::to_string(old_backend);
    entry.path_history.push_back(hist);

    Logger::instance().logSystem(LogLevel::INFO,
        "Forced reassignment: " + tuple_key + " from backend " +
        std::to_string(old_backend) + " to " + std::to_string(new_backend));

    return true;
}

// ─────────────────────────────────────────────────────────────
// Block / unblock a session
// ─────────────────────────────────────────────────────────────
bool SessionManager::blockSession(const std::string& tuple_key, bool block) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = sessions_.find(tuple_key);
    if (it == sessions_.end()) return false;

    it->second.blocked = block;

    Logger::instance().logSystem(LogLevel::INFO,
        "Session " + tuple_key + (block ? " BLOCKED" : " UNBLOCKED"));

    return true;
}

// ─────────────────────────────────────────────────────────────
// Set custom expiry for a session
// ─────────────────────────────────────────────────────────────
bool SessionManager::setSessionExpiry(const std::string& tuple_key, int timeout_sec) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = sessions_.find(tuple_key);
    if (it == sessions_.end()) return false;

    it->second.custom_timeout = timeout_sec;

    Logger::instance().logSystem(LogLevel::INFO,
        "Session " + tuple_key + " timeout set to " +
        std::to_string(timeout_sec) + "s");

    return true;
}

// ─────────────────────────────────────────────────────────────
// Get path history for a session
// ─────────────────────────────────────────────────────────────
std::vector<PathHistoryEntry> SessionManager::getPathHistory(const std::string& tuple_key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(tuple_key);
    if (it == sessions_.end()) return {};
    return it->second.path_history;
}

// ─────────────────────────────────────────────────────────────
// Count active sessions
// ─────────────────────────────────────────────────────────────
int SessionManager::activeSessionCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int count = 0;
    for (const auto& pair : sessions_) {
        if (!isExpired(pair.second)) ++count;
    }
    return count;
}

// ─────────────────────────────────────────────────────────────
// Get all sessions
// ─────────────────────────────────────────────────────────────
std::vector<std::pair<std::string, int>> SessionManager::getAllSessions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<std::string, int>> result;
    for (const auto& pair : sessions_) {
        if (!isExpired(pair.second)) {
            result.push_back({pair.first, pair.second.backend_id});
        }
    }
    return result;
}

// ─────────────────────────────────────────────────────────────
// Handle a backend server going down — reassign all its sessions
// ─────────────────────────────────────────────────────────────
void SessionManager::handleServerDown(int server_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    int reassigned_count = 0;

    for (auto& pair : sessions_) {
        SessionEntry& entry = pair.second;
        if (entry.backend_id == server_id && !isExpired(entry)) {
            int new_backend = hash_ring_.getServer(pair.first);
            if (new_backend >= 0 && new_backend != server_id) {
                int old_backend = entry.backend_id;
                entry.backend_id = new_backend;
                entry.last_access = std::chrono::steady_clock::now();

                PathHistoryEntry hist;
                hist.backend_id = new_backend;
                hist.timestamp  = currentTimestamp();
                hist.reason     = "Server " + std::to_string(old_backend) +
                                  " went down — redistributed";
                entry.path_history.push_back(hist);

                ++reassigned_count;
            }
        }
    }

    Logger::instance().logSystem(LogLevel::WARNING,
        "Server " + std::to_string(server_id) + " went down. Reassigned " +
        std::to_string(reassigned_count) + " sessions.");
}

// ─────────────────────────────────────────────────────────────
// Cleanup expired sessions
// ─────────────────────────────────────────────────────────────
int SessionManager::cleanupExpired() {
    std::lock_guard<std::mutex> lock(mutex_);

    int removed = 0;
    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        if (isExpired(it->second)) {
            Logger::instance().logSystem(LogLevel::DEBUG,
                "Cleaned up expired session: " + it->first);
            it = sessions_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }

    return removed;
}
