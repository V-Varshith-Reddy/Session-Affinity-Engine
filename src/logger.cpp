/**
 * @file logger.cpp
 * @brief Implementation of the thread-safe Logger singleton.
 *
 * Log format:
 *   Request log: [timestamp] [tuple_key] [action] [status] [details]
 *   System  log: [timestamp] [LEVEL] [message]
 */

#include "logger.h"
#include "../config.h"
#include "common.h"

#include <filesystem>
#include <iostream>

// ─────────────────────────────────────────────────────────────
// Singleton accessor
// ─────────────────────────────────────────────────────────────
Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

// ─────────────────────────────────────────────────────────────
// Initialization
// ─────────────────────────────────────────────────────────────
bool Logger::init(bool enabled) {
    enabled_ = enabled;
    if (!enabled_) return true;  // Nothing to open

    // Ensure the logs directory exists
    try {
        std::filesystem::create_directories(LOG_DIRECTORY);
    } catch (const std::exception& e) {
        std::cerr << "[Logger] ERROR: Could not create log directory: " << e.what() << "\n";
        return false;
    }

    // Open request log file in append mode
    request_file_.open(REQUEST_LOG_FILE, std::ios::app);
    if (!request_file_.is_open()) {
        std::cerr << "[Logger] ERROR: Could not open request log file: "
                  << REQUEST_LOG_FILE << "\n";
        return false;
    }

    // Open system log file in append mode
    system_file_.open(SYSTEM_LOG_FILE, std::ios::app);
    if (!system_file_.is_open()) {
        std::cerr << "[Logger] ERROR: Could not open system log file: "
                  << SYSTEM_LOG_FILE << "\n";
        return false;
    }

    logSystem(LogLevel::INFO, "Logger initialized successfully.");
    return true;
}

// ─────────────────────────────────────────────────────────────
// Shutdown
// ─────────────────────────────────────────────────────────────
void Logger::shutdown() {
    if (enabled_) {
        logSystem(LogLevel::INFO, "Logger shutting down.");
        if (request_file_.is_open()) request_file_.close();
        if (system_file_.is_open())  system_file_.close();
    }
}

Logger::~Logger() {
    shutdown();
}

// ─────────────────────────────────────────────────────────────
// Request logging
// ─────────────────────────────────────────────────────────────
void Logger::logRequest(const std::string& tuple_key,
                        const std::string& action,
                        const std::string& status,
                        const std::string& details) {
    if (!enabled_) return;

    std::lock_guard<std::mutex> lock(request_mutex_);
    request_file_ << "[" << currentTimestamp() << "] "
                  << "[" << tuple_key << "] "
                  << "[" << action << "] "
                  << "[" << status << "] "
                  << details << "\n";
    request_file_.flush();
}

// ─────────────────────────────────────────────────────────────
// System logging
// ─────────────────────────────────────────────────────────────
void Logger::logSystem(LogLevel level, const std::string& message) {
    if (!enabled_) return;

    std::lock_guard<std::mutex> lock(system_mutex_);
    system_file_ << "[" << currentTimestamp() << "] "
                 << "[" << levelToString(level) << "] "
                 << message << "\n";
    system_file_.flush();
}

// ─────────────────────────────────────────────────────────────
// Helper
// ─────────────────────────────────────────────────────────────
std::string Logger::levelToString(LogLevel level) {
    switch (level) {
        case LogLevel::INFO:    return "INFO";
        case LogLevel::WARNING: return "WARNING";
        case LogLevel::ERROR:   return "ERROR";
        case LogLevel::DEBUG:   return "DEBUG";
    }
    return "UNKNOWN";
}
