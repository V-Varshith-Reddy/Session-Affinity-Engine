/**
 * @file request_handler.cpp
 * @brief Implementation of the backend server (request handler).
 *
 * Each handler maintains its own local hash map of per-tuple session data.
 * Since sticky sessions ensure a tuple is only served by one handler at a time,
 * no locking is needed for the local_data_ map.
 */

#include "request_handler.h"
#include "logger.h"
#include "../config.h"

// ─────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────
RequestHandler::RequestHandler(int id)
    : id_(id), online_(true), total_requests_(0) {

    Logger::instance().logSystem(LogLevel::INFO,
        "Backend server " + std::to_string(id_) + " created on port " +
        std::to_string(getPort()));
}

// ─────────────────────────────────────────────────────────────
// Process a request
// ─────────────────────────────────────────────────────────────
std::string RequestHandler::processRequest(const FiveTuple& ft) {
    if (!online_.load()) {
        return "ERROR: Server " + std::to_string(id_) + " is offline";
    }

    std::string key = ft.toKey();
    ++total_requests_;

    // Update or create local session data for this tuple
    auto& data = local_data_[key];
    data.tuple_key         = key;
    data.request_count    += 1;
    data.last_request_time = currentTimestamp();
    data.metadata          = "Processed by backend " + std::to_string(id_);

    Logger::instance().logRequest(key, "PROCESS", "SUCCESS",
        "Backend " + std::to_string(id_) + " | request #" +
        std::to_string(data.request_count) + " for this tuple");

    return "SUCCESS: Processed by backend " + std::to_string(id_) +
           " (request #" + std::to_string(data.request_count) + " for this session)";
}

// ─────────────────────────────────────────────────────────────
// Accessors
// ─────────────────────────────────────────────────────────────
const std::unordered_map<std::string, SessionData>& RequestHandler::getLocalData() const {
    return local_data_;
}

void RequestHandler::start() {
    online_.store(true);
    Logger::instance().logSystem(LogLevel::INFO,
        "Backend server " + std::to_string(id_) + " is now ONLINE");
}

void RequestHandler::stop() {
    online_.store(false);
    Logger::instance().logSystem(LogLevel::WARNING,
        "Backend server " + std::to_string(id_) + " is now OFFLINE");
}

int RequestHandler::getPort() const {
    return BACKEND_BASE_PORT + id_;
}

int RequestHandler::totalRequestsProcessed() const {
    return total_requests_;
}
