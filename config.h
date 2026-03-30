/**
 * @file config.h
 * @brief Centralized configuration for the Session Affinity Engine.
 *
 * All tunable parameters are defined here as preprocessor macros so they
 * can be changed in one place without modifying any other source file.
 * To reconfigure the system, simply edit the values below and recompile.
 */

#ifndef CONFIG_H
#define CONFIG_H

// ─────────────────────────────────────────────────────────────
// Backend Server Configuration
// ─────────────────────────────────────────────────────────────
/** Number of backend server instances available for routing */
#define NUM_BACKEND_SERVERS         8

/** Base UDP port for backend servers. Server i listens on BASE + i */
#define BACKEND_BASE_PORT           8001

// ─────────────────────────────────────────────────────────────
// Load Balancer Configuration
// ─────────────────────────────────────────────────────────────
/** UDP port for the primary load balancer */
#define LB_PRIMARY_PORT             9000

/** UDP port for the standby (failover) load balancer */
#define LB_STANDBY_PORT             9001

/** Heartbeat interval (ms) — primary LB sends heartbeat to standby */
#define LB_HEARTBEAT_INTERVAL_MS    1000

/** Failover threshold (ms) — standby takes over if no heartbeat in this window */
#define LB_FAILOVER_THRESHOLD_MS    3000

// ─────────────────────────────────────────────────────────────
// Session Configuration
// ─────────────────────────────────────────────────────────────
/** Session timeout in seconds — idle sessions expire after this duration */
#define SESSION_TIMEOUT_SEC         300

// ─────────────────────────────────────────────────────────────
// Rate Limiting (Sliding Window Queue)
// ─────────────────────────────────────────────────────────────
/** Window size in seconds for the sliding window rate limiter */
#define RATE_LIMIT_WINDOW_SEC       60

/** Maximum number of requests allowed per client within the window */
#define RATE_LIMIT_MAX_REQUESTS     100

// ─────────────────────────────────────────────────────────────
// Security
// ─────────────────────────────────────────────────────────────
/** Maximum allowed UDP payload size in bytes */
#define MAX_PAYLOAD_SIZE            1024

/** Maximum unique source IPs per /24 subnet in a time window (spoof detection) */
#define SPOOF_DETECTION_THRESHOLD   50

/** Spoof detection window in seconds */
#define SPOOF_DETECTION_WINDOW_SEC  60

// ─────────────────────────────────────────────────────────────
// Consistent Hashing
// ─────────────────────────────────────────────────────────────
/** Number of virtual nodes per physical backend on the hash ring */
#define CONSISTENT_HASH_VNODES      150

// ─────────────────────────────────────────────────────────────
// Logging
// ─────────────────────────────────────────────────────────────
/** Directory for log files (relative to working directory) */
#define LOG_DIRECTORY               "logs"

/** Request history log file */
#define REQUEST_LOG_FILE            "logs/requests.log"

/** System events log file */
#define SYSTEM_LOG_FILE             "logs/system.log"

#endif // CONFIG_H
