/**
 * @file request_handler.h
 * @brief Backend server (request handler) that processes routed requests.
 *
 * Each backend server instance:
 *   - Has a unique ID and listens on BACKEND_BASE_PORT + id
 *   - Stores per-tuple session data in a local hash map
 *   - Sends heartbeat signals to the load balancer
 *   - Can be started/stopped to simulate failures
 */

#ifndef REQUEST_HANDLER_H
#define REQUEST_HANDLER_H

#include "common.h"
#include <string>
#include <unordered_map>
#include <atomic>

/**
 * @struct SessionData
 * @brief Per-tuple data stored locally on a backend server.
 *
 * Since sticky sessions guarantee that a tuple is only handled by one
 * server at a time, no locking is needed for this data.
 */
struct SessionData {
    std::string tuple_key;
    int         request_count;
    std::string last_request_time;
    std::string metadata;  // Simulated application data
};

/**
 * @class RequestHandler
 * @brief Simulates a backend server that processes requests from the load balancer.
 *
 * In the simulation, the RequestHandler doesn't actually listen on a UDP socket.
 * Instead, it provides a processRequest() method called by the LoadBalancer.
 * This keeps the simulation in-process while maintaining the correct abstractions.
 */
class RequestHandler {
public:
    /**
     * @brief Constructs a backend server with the given ID.
     * @param id Unique server identifier (0 through NUM_BACKEND_SERVERS-1).
     */
    explicit RequestHandler(int id);

    /**
     * @brief Processes an incoming request for the given 5-tuple.
     *
     * Updates the local session data for this tuple (increment count,
     * update timestamp, etc.).
     *
     * @param ft The 5-tuple identifying the session.
     * @return A response string indicating success.
     */
    std::string processRequest(const FiveTuple& ft);

    /**
     * @brief Returns the per-tuple session data stored on this server.
     */
    const std::unordered_map<std::string, SessionData>& getLocalData() const;

    /**
     * @brief Checks if this server is currently online.
     */
    bool isOnline() const { return online_.load(); }

    /**
     * @brief Brings the server online.
     */
    void start();

    /**
     * @brief Takes the server offline (simulates failure).
     */
    void stop();

    /**
     * @brief Returns the server's unique ID.
     */
    int getId() const { return id_; }

    /**
     * @brief Returns the port this server would listen on.
     */
    int getPort() const;

    /**
     * @brief Returns the total number of requests processed by this server.
     */
    int totalRequestsProcessed() const;

private:
    int                                              id_;
    std::atomic<bool>                                online_;
    std::unordered_map<std::string, SessionData>     local_data_;
    int                                              total_requests_;
};

#endif // REQUEST_HANDLER_H
