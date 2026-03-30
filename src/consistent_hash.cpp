/**
 * @file consistent_hash.cpp
 * @brief Implementation of the ConsistentHashRing.
 *
 * The ring uses std::map ordered by hash values. Virtual node keys are
 * generated as "server_<id>_vnode_<i>" and hashed to produce positions
 * on the ring. Lookups use lower_bound for O(log N) efficiency.
 */

#include "consistent_hash.h"
#include "logger.h"

#include <algorithm>
#include <sstream>

// ─────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────
ConsistentHashRing::ConsistentHashRing(int vnodes_per_server)
    : vnodes_per_server_(vnodes_per_server) {}

// ─────────────────────────────────────────────────────────────
// Hash function — uses std::hash with salt mixing for better distribution
// ─────────────────────────────────────────────────────────────
std::size_t ConsistentHashRing::hashKey(const std::string& key) const {
    // Use a combination of hash and FNV-1a-style mixing for good distribution
    std::size_t hash = 14695981039346656037ULL;  // FNV offset basis
    for (char c : key) {
        hash ^= static_cast<std::size_t>(c);
        hash *= 1099511628211ULL;  // FNV prime
    }
    return hash;
}

// ─────────────────────────────────────────────────────────────
// Add a server with its virtual nodes
// ─────────────────────────────────────────────────────────────
void ConsistentHashRing::addServer(int server_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if already active
    if (std::find(active_servers_.begin(), active_servers_.end(), server_id)
        != active_servers_.end()) {
        return;  // Already on the ring
    }

    // Create virtual nodes
    for (int i = 0; i < vnodes_per_server_; ++i) {
        std::string vnode_key = "server_" + std::to_string(server_id)
                              + "_vnode_" + std::to_string(i);
        std::size_t hash = hashKey(vnode_key);
        ring_[hash] = server_id;
    }

    active_servers_.push_back(server_id);

    Logger::instance().logSystem(LogLevel::INFO,
        "ConsistentHash: Added server " + std::to_string(server_id) +
        " with " + std::to_string(vnodes_per_server_) + " vnodes. " +
        "Ring size: " + std::to_string(ring_.size()));
}

// ─────────────────────────────────────────────────────────────
// Remove a server and all its virtual nodes
// ─────────────────────────────────────────────────────────────
void ConsistentHashRing::removeServer(int server_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Remove virtual nodes from ring
    for (int i = 0; i < vnodes_per_server_; ++i) {
        std::string vnode_key = "server_" + std::to_string(server_id)
                              + "_vnode_" + std::to_string(i);
        std::size_t hash = hashKey(vnode_key);
        ring_.erase(hash);
    }

    // Remove from active list
    active_servers_.erase(
        std::remove(active_servers_.begin(), active_servers_.end(), server_id),
        active_servers_.end());

    Logger::instance().logSystem(LogLevel::WARNING,
        "ConsistentHash: Removed server " + std::to_string(server_id) +
        ". Ring size: " + std::to_string(ring_.size()) +
        ". Active servers: " + std::to_string(active_servers_.size()));
}

// ─────────────────────────────────────────────────────────────
// Get the server for a given key — O(log N) lookup
// ─────────────────────────────────────────────────────────────
int ConsistentHashRing::getServer(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (ring_.empty()) return -1;

    std::size_t hash = hashKey(key);

    // Find the first node at or after this hash position (clockwise walk)
    auto it = ring_.lower_bound(hash);

    // Wrap around to the beginning if we've gone past the last node
    if (it == ring_.end()) {
        it = ring_.begin();
    }

    return it->second;
}

// ─────────────────────────────────────────────────────────────
// Utility accessors
// ─────────────────────────────────────────────────────────────
std::vector<int> ConsistentHashRing::getActiveServers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_servers_;
}

bool ConsistentHashRing::hasServers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !active_servers_.empty();
}

int ConsistentHashRing::serverCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(active_servers_.size());
}
