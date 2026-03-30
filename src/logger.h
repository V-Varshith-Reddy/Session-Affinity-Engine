/**
 * @file logger.h
 * @brief Thread-safe, file-based logging system with runtime toggle.
 *
 * Provides two log channels:
 *   - REQUEST log: records every client request with tuple, status, reason
 *   - SYSTEM  log: records internal events (failover, server up/down, etc.)
 *
 * Logging can be enabled/disabled at startup via CLI args and is controlled
 * globally through this singleton-style module.
 */

#ifndef LOGGER_H
#define LOGGER_H

#include <string>
#include <fstream>
#include <mutex>
#include <iostream>

/**
 * @enum LogLevel
 * @brief Severity levels for log entries.
 */
enum class LogLevel {
    INFO,
    WARNING,
    ERROR,
    DEBUG
};

/**
 * @class Logger
 * @brief Singleton logger that writes to separate request and system log files.
 *
 * Thread-safety is ensured via a mutex on each write operation.
 * When logging is disabled, all log calls are no-ops.
 */
class Logger {
public:
    /**
     * @brief Returns the singleton Logger instance.
     */
    static Logger& instance();

    /**
     * @brief Initializes the logger — opens log files and sets enabled state.
     * @param enabled Whether logging is active.
     * @return true on success, false if files could not be opened.
     */
    bool init(bool enabled);

    /**
     * @brief Shuts down the logger and flushes/closes all files.
     */
    void shutdown();

    /**
     * @brief Logs a request event (goes to requests.log).
     * @param tuple_key  The 5-tuple key string identifying the session.
     * @param action     What happened (e.g., "ROUTE", "RATE_LIMIT", "REJECT").
     * @param status     Whether the request was valid/invalid.
     * @param details    Additional context (backend id, reason, etc.).
     */
    void logRequest(const std::string& tuple_key,
                    const std::string& action,
                    const std::string& status,
                    const std::string& details);

    /**
     * @brief Logs a system event (goes to system.log).
     * @param level    Severity level.
     * @param message  Descriptive message.
     */
    void logSystem(LogLevel level, const std::string& message);

    /** @brief Returns whether logging is currently enabled. */
    bool isEnabled() const { return enabled_; }

private:
    Logger() = default;
    ~Logger();

    // Non-copyable
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    bool            enabled_ = false;
    std::ofstream   request_file_;
    std::ofstream   system_file_;
    std::mutex      request_mutex_;
    std::mutex      system_mutex_;

    /** @brief Converts LogLevel enum to string. */
    static std::string levelToString(LogLevel level);
};

#endif // LOGGER_H
