/**
 * @file consistent_hash.h
 * @brief Consistent hashing ring for stable backend server assignment.
 *
 * Uses virtual nodes (vnodes) to achieve even distribution of sessions
 * across backend servers. When a server is removed, only its portion
 * of the key space is redistributed — minimizing session disruption.
 *
 * Complexity:
 *   - Lookup:  O(log N) where N = total virtual nodes
 *   - Add:     O(V log N) where V = vnodes per server
 *   - Remove:  O(V log N)
 */

#ifndef CONSISTENT_HASH_H
#define CONSISTENT_HASH_H

#include <map>
#include <string>
#include <vector>
#include <cstddef>
#include <functional>
#include <mutex>

/**
 * @class ConsistentHashRing
 * @brief Maps session keys to backend server IDs using a consistent hash ring.
 *
 * Each physical server is represented by multiple virtual nodes on the ring
 * (controlled by CONSISTENT_HASH_VNODES). This ensures even load distribution
 * and minimal disruption when servers are added/removed.
 */
class ConsistentHashRing {
public:
    /**
     * @brief Constructs the hash ring with the specified number of vnodes per server.
     * @param vnodes_per_server Number of virtual nodes per physical server.
     */
    explicit ConsistentHashRing(int vnodes_per_server);

    /**
     * @brief Adds a server to the ring, creating its virtual nodes.
     * @param server_id Unique identifier for the backend server.
     */
    void addServer(int server_id);

    /**
     * @brief Removes a server and all its virtual nodes from the ring.
     * @param server_id The server to remove.
     */
    void removeServer(int server_id);

    /**
     * @brief Finds the backend server responsible for the given key.
     * @param key The session key (typically the hashed 5-tuple string).
     * @return The server ID, or -1 if the ring is empty.
     */
    int getServer(const std::string& key) const;

    /**
     * @brief Returns the list of all currently active server IDs on the ring.
     */
    std::vector<int> getActiveServers() const;

    /**
     * @brief Returns true if the ring has at least one server.
     */
    bool hasServers() const;

    /**
     * @brief Returns the number of active physical servers.
     */
    int serverCount() const;

private:
    int                         vnodes_per_server_;
    std::map<std::size_t, int>  ring_;             // hash_value -> server_id
    std::vector<int>            active_servers_;    // List of active server IDs
    mutable std::mutex          mutex_;

    /**
     * @brief Computes a hash for the given string key.
     */
    std::size_t hashKey(const std::string& key) const;
};

#endif // CONSISTENT_HASH_H
