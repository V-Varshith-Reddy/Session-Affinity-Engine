/**
 * @file session_manager.h
 * @brief Session table management with sticky sessions, expiry, and path history.
 *
 * Manages the mapping of 5-tuples to backend servers. Provides:
 *   - Sticky session lookup (same tuple → same backend)
 *   - Session expiry (idle timeout)
 *   - Path history tracking (audit trail of all assignments)
 *   - Dynamic configuration (force reassign, block session, adjust expiry)
 *   - Integration with ConsistentHashRing for backend selection
 */

#ifndef SESSION_MANAGER_H
#define SESSION_MANAGER_H

#include "common.h"
#include "consistent_hash.h"
#include <unordered_map>
#include <string>
#include <vector>
#include <mutex>
#include <chrono>

/**
 * @struct SessionEntry
 * @brief Represents a single session in the session table.
 */
struct SessionEntry {
    FiveTuple   tuple;          // The 5-tuple that identifies this session
    int         backend_id;     // Currently assigned backend server ID
    bool        blocked;        // If true, all requests for this session are rejected
    int         custom_timeout; // Per-session timeout override (-1 = use global default)

    std::chrono::steady_clock::time_point created_at;     // When the session was created
    std::chrono::steady_clock::time_point last_access;    // Last request time (for expiry)

    std::vector<PathHistoryEntry> path_history;  // Full audit trail of path assignments
};

/**
 * @class SessionManager
 * @brief Central session table with sticky routing, expiry, and dynamic config.
 *
 * Design decisions:
 *   - Uses unordered_map for O(1) average lookup by 5-tuple key
 *   - No explicit locking for data within a session (per problem spec:
 *     a tuple is only handled by one server at a time)
 *   - Mutex protects the session table structure itself
 *   - Lazy expiry: sessions are checked/cleaned on access, not via a background thread
 *     (simpler, avoids thread complexity for this simulation)
 */
class SessionManager {
public:
    /**
     * @brief Constructs the session manager.
     * @param hash_ring  Reference to the consistent hash ring for backend selection.
     * @param default_timeout_sec  Default session timeout in seconds.
     */
    SessionManager(ConsistentHashRing& hash_ring, int default_timeout_sec);

    /**
     * @brief Looks up or creates a session for the given 5-tuple.
     *
     * If the session exists and is not expired/blocked:
     *   - Returns the existing backend assignment (sticky)
     *   - Updates last_access time
     * If the session doesn't exist or is expired:
     *   - Creates a new session with backend from consistent hash ring
     * If the assigned backend is down (not in ring):
     *   - Reassigns to a new backend and logs the path change
     *
     * @param ft The 5-tuple identifying the session.
     * @param assigned_backend [out] The backend server ID for this session.
     * @return RequestStatus indicating the result.
     */
    RequestStatus routeRequest(const FiveTuple& ft, int& assigned_backend);

    /**
     * @brief Forces reassignment of a session to a new backend.
     * @param tuple_key The 5-tuple key string.
     * @return true if the session was found and reassigned.
     */
    bool forceReassign(const std::string& tuple_key);

    /**
     * @brief Blocks a session — all subsequent requests will be rejected.
     * @param tuple_key The session key.
     * @param block true to block, false to unblock.
     * @return true if the session was found.
     */
    bool blockSession(const std::string& tuple_key, bool block);

    /**
     * @brief Sets a custom expiry timeout for a specific session.
     * @param tuple_key The session key.
     * @param timeout_sec The new timeout in seconds (-1 to reset to default).
     * @return true if the session was found.
     */
    bool setSessionExpiry(const std::string& tuple_key, int timeout_sec);

    /**
     * @brief Retrieves the path history for a given session.
     * @param tuple_key The session key.
     * @return Vector of PathHistoryEntry (empty if session not found).
     */
    std::vector<PathHistoryEntry> getPathHistory(const std::string& tuple_key) const;

    /**
     * @brief Returns the total number of active (non-expired) sessions.
     */
    int activeSessionCount() const;

    /**
     * @brief Returns a snapshot of all session keys and their assigned backends.
     */
    std::vector<std::pair<std::string, int>> getAllSessions() const;

    /**
     * @brief Called when a backend server goes down.
     *
     * Iterates all sessions assigned to the downed server and reassigns
     * them to new backends via the consistent hash ring.
     *
     * @param server_id The backend server that went down.
     */
    void handleServerDown(int server_id);

    /**
     * @brief Runs a lazy cleanup pass, removing expired sessions.
     * @return Number of sessions cleaned up.
     */
    int cleanupExpired();

private:
    ConsistentHashRing& hash_ring_;
    int                 default_timeout_sec_;

    // The session table: 5-tuple key string → SessionEntry
    std::unordered_map<std::string, SessionEntry> sessions_;
    mutable std::mutex mutex_;

    /**
     * @brief Checks if a session has expired.
     */
    bool isExpired(const SessionEntry& entry) const;
};

#endif // SESSION_MANAGER_H
