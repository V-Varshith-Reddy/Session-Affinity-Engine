/**
 * @file main.cpp
 * @brief Entry point for the Session Affinity Engine.
 *
 * This is the unified entry point that runs the complete simulation:
 *   1. Initializes all components (backends, hash ring, session manager, LBs)
 *   2. Runs a client simulation that exercises all features
 *   3. Demonstrates sticky sessions, failover, rate limiting, security
 *
 * CLI Arguments:
 *   --logging on|off    Enable/disable file-based logging (default: on)
 *   --mode sim|test     Run simulation or test mode (default: sim)
 *
 * Usage:
 *   ./session_engine --logging on --mode sim
 *   ./session_engine --logging on --mode test
 */

#include "common.h"
#include "logger.h"
#include "consistent_hash.h"
#include "rate_limiter.h"
#include "security.h"
#include "session_manager.h"
#include "request_handler.h"
#include "load_balancer.h"
#include "../config.h"

#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <random>
#include <cassert>

// ─────────────────────────────────────────────────────────────
// CLI argument parsing
// ─────────────────────────────────────────────────────────────

struct CLIArgs {
    bool logging_enabled = true;
    std::string mode     = "sim";  // "sim" or "test"
};

CLIArgs parseArgs(int argc, char* argv[]) {
    CLIArgs args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--logging" && i + 1 < argc) {
            args.logging_enabled = (std::string(argv[++i]) == "on");
        } else if (arg == "--mode" && i + 1 < argc) {
            args.mode = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Session Affinity Engine\n"
                      << "Usage: ./session_engine [OPTIONS]\n"
                      << "  --logging on|off    Enable/disable logging (default: on)\n"
                      << "  --mode sim|test     Run simulation or tests (default: sim)\n"
                      << "  --help              Show this help message\n";
            exit(0);
        }
    }
    return args;
}

// ─────────────────────────────────────────────────────────────
// Utility: print separator
// ─────────────────────────────────────────────────────────────
void printSeparator(const std::string& title) {
    std::cout << "\n" << std::string(70, '=') << "\n";
    std::cout << "  " << title << "\n";
    std::cout << std::string(70, '=') << "\n\n";
}

void printSubSeparator(const std::string& title) {
    std::cout << "\n--- " << title << " ---\n\n";
}

// ─────────────────────────────────────────────────────────────
// Generate sample 5-tuples for simulation
// ─────────────────────────────────────────────────────────────
std::vector<FiveTuple> generateTestTuples(int count) {
    std::vector<FiveTuple> tuples;
    std::mt19937 rng(42);  // Fixed seed for reproducibility

    for (int i = 0; i < count; ++i) {
        FiveTuple ft;
        ft.src_ip   = "192.168.1." + std::to_string((i % 254) + 1);
        ft.src_port = static_cast<uint16_t>(10000 + (rng() % 55000));
        ft.dst_ip   = "10.0.0." + std::to_string((i % 8) + 1);
        ft.dst_port = 80;
        ft.protocol = (i % 3 == 0) ? "UDP" : "TCP";
        tuples.push_back(ft);
    }

    return tuples;
}

// ─────────────────────────────────────────────────────────────
// SIMULATION MODE — exercises all features
// ─────────────────────────────────────────────────────────────
void runSimulation() {
    printSeparator("SESSION AFFINITY ENGINE — SIMULATION");

    std::cout << "Configuration:\n"
              << "  Backend servers:     " << NUM_BACKEND_SERVERS << "\n"
              << "  Session timeout:     " << SESSION_TIMEOUT_SEC << "s\n"
              << "  Rate limit:          " << RATE_LIMIT_MAX_REQUESTS
              << " req/" << RATE_LIMIT_WINDOW_SEC << "s\n"
              << "  Hash ring vnodes:    " << CONSISTENT_HASH_VNODES << "\n"
              << "  LB Primary port:     " << LB_PRIMARY_PORT << "\n"
              << "  LB Standby port:     " << LB_STANDBY_PORT << "\n"
              << "  Heartbeat interval:  " << LB_HEARTBEAT_INTERVAL_MS << "ms\n"
              << "  Failover threshold:  " << LB_FAILOVER_THRESHOLD_MS << "ms\n\n";

    // ── Initialize components ──
    ConsistentHashRing hash_ring(CONSISTENT_HASH_VNODES);
    RateLimiter rate_limiter(RATE_LIMIT_WINDOW_SEC, RATE_LIMIT_MAX_REQUESTS);
    Security security;
    SessionManager session_mgr(hash_ring, SESSION_TIMEOUT_SEC);

    // Create backend servers
    std::vector<std::unique_ptr<RequestHandler>> backends;
    for (int i = 0; i < NUM_BACKEND_SERVERS; ++i) {
        backends.push_back(std::make_unique<RequestHandler>(i));
        hash_ring.addServer(i);
    }

    // Create primary and standby load balancers
    LoadBalancer primary_lb(LBRole::PRIMARY, session_mgr, hash_ring,
                            rate_limiter, security, backends);
    LoadBalancer standby_lb(LBRole::STANDBY, session_mgr, hash_ring,
                            rate_limiter, security, backends);

    // ════════════════════════════════════════════════════════
    // TEST 1: Sticky session routing
    // ════════════════════════════════════════════════════════
    printSubSeparator("TEST 1: Sticky Session Routing");

    auto tuples = generateTestTuples(10);

    std::cout << "Sending 10 unique clients, each with 3 requests...\n\n";
    std::cout << std::left << std::setw(45) << "5-TUPLE"
              << std::setw(12) << "REQ #"
              << "RESPONSE" << "\n";
    std::cout << std::string(90, '-') << "\n";

    for (const auto& ft : tuples) {
        for (int r = 0; r < 3; ++r) {
            std::string response = primary_lb.handleRequest(ft, ft.serialize());
            std::cout << std::left << std::setw(45) << ft.toKey()
                      << std::setw(12) << (r + 1)
                      << response << "\n";
        }
    }

    std::cout << "\nActive sessions: " << session_mgr.activeSessionCount() << "\n";

    // ════════════════════════════════════════════════════════
    // TEST 2: Path history
    // ════════════════════════════════════════════════════════
    printSubSeparator("TEST 2: Path History for First Session");

    auto history = session_mgr.getPathHistory(tuples[0].toKey());
    for (const auto& h : history) {
        std::cout << "  Backend: " << h.backend_id
                  << " | Time: " << h.timestamp
                  << " | Reason: " << h.reason << "\n";
    }

    // ════════════════════════════════════════════════════════
    // TEST 3: Backend server failure & redistribution
    // ════════════════════════════════════════════════════════
    printSubSeparator("TEST 3: Backend Failure & Redistribution");

    // Find which backend server 0's sessions are on
    auto all_sessions = session_mgr.getAllSessions();
    int server_to_kill = 0;
    std::cout << "Taking backend " << server_to_kill << " offline...\n";
    primary_lb.takeBackendOffline(server_to_kill);

    std::cout << "Active backends after failure: " << hash_ring.serverCount() << "\n";
    std::cout << "\nSending requests from same clients (should be redistributed if affected):\n\n";

    std::cout << std::left << std::setw(45) << "5-TUPLE"
              << "RESPONSE" << "\n";
    std::cout << std::string(90, '-') << "\n";

    for (const auto& ft : tuples) {
        std::string response = primary_lb.handleRequest(ft, ft.serialize());
        std::cout << std::left << std::setw(45) << ft.toKey()
                  << response << "\n";
    }

    // Check path history after failover
    std::cout << "\nPath history for first session after failover:\n";
    history = session_mgr.getPathHistory(tuples[0].toKey());
    for (const auto& h : history) {
        std::cout << "  Backend: " << h.backend_id
                  << " | Time: " << h.timestamp
                  << " | Reason: " << h.reason << "\n";
    }

    // Bring backend back online
    std::cout << "\nBringing backend " << server_to_kill << " back online...\n";
    primary_lb.bringBackendOnline(server_to_kill);
    std::cout << "Active backends: " << hash_ring.serverCount() << "\n";

    // ════════════════════════════════════════════════════════
    // TEST 4: Load balancer failover
    // ════════════════════════════════════════════════════════
    printSubSeparator("TEST 4: Load Balancer Failover");

    std::cout << "Primary LB is currently: " << (primary_lb.isActive() ? "ACTIVE" : "PASSIVE") << "\n";
    std::cout << "Standby LB is currently: " << (standby_lb.isActive() ? "ACTIVE" : "PASSIVE") << "\n";

    // Simulate primary going down
    std::cout << "\nSimulating primary LB failure...\n";
    primary_lb.deactivate();

    // Try sending through primary (should fail)
    std::string resp = primary_lb.handleRequest(tuples[0], tuples[0].serialize());
    std::cout << "Request via PRIMARY: " << resp << "\n";

    // Standby detects failure and takes over
    // Simulate heartbeat timeout by using a very old timestamp
    auto old_heartbeat = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    if (standby_lb.shouldTakeover(old_heartbeat)) {
        std::cout << "Standby detected primary timeout — TAKING OVER!\n";
        standby_lb.activate();
    }

    // Send through standby (should succeed)
    resp = standby_lb.handleRequest(tuples[0], tuples[0].serialize());
    std::cout << "Request via STANDBY: " << resp << "\n";

    // Restore primary
    std::cout << "\nRestoring primary LB...\n";
    primary_lb.activate();
    standby_lb.deactivate();
    primary_lb.sendHeartbeat();

    std::cout << "Primary: " << (primary_lb.isActive() ? "ACTIVE" : "PASSIVE") << "\n";
    std::cout << "Standby: " << (standby_lb.isActive() ? "ACTIVE" : "PASSIVE") << "\n";

    // ════════════════════════════════════════════════════════
    // TEST 5: Security — invalid inputs
    // ════════════════════════════════════════════════════════
    printSubSeparator("TEST 5: Security — Malformed Input Rejection");

    // Invalid IP
    FiveTuple bad_ip;
    bad_ip.src_ip = "999.999.999.999";
    bad_ip.src_port = 12345;
    bad_ip.dst_ip = "10.0.0.1";
    bad_ip.dst_port = 80;
    bad_ip.protocol = "TCP";
    resp = primary_lb.handleRequest(bad_ip, bad_ip.serialize());
    std::cout << "Invalid IP:       " << resp << "\n";

    // Invalid port (0)
    FiveTuple bad_port;
    bad_port.src_ip = "192.168.1.1";
    bad_port.src_port = 0;
    bad_port.dst_ip = "10.0.0.1";
    bad_port.dst_port = 80;
    bad_port.protocol = "TCP";
    resp = primary_lb.handleRequest(bad_port, bad_port.serialize());
    std::cout << "Invalid port:     " << resp << "\n";

    // Invalid protocol
    FiveTuple bad_proto;
    bad_proto.src_ip = "192.168.1.1";
    bad_proto.src_port = 12345;
    bad_proto.dst_ip = "10.0.0.1";
    bad_proto.dst_port = 80;
    bad_proto.protocol = "ICMP";
    resp = primary_lb.handleRequest(bad_proto, bad_proto.serialize());
    std::cout << "Invalid protocol: " << resp << "\n";

    // Oversized payload
    std::string huge_payload(MAX_PAYLOAD_SIZE + 100, 'X');
    FiveTuple valid_ft;
    valid_ft.src_ip = "192.168.1.1";
    valid_ft.src_port = 12345;
    valid_ft.dst_ip = "10.0.0.1";
    valid_ft.dst_port = 80;
    valid_ft.protocol = "TCP";
    resp = primary_lb.handleRequest(valid_ft, huge_payload);
    std::cout << "Oversized payload:" << resp << "\n";

    // ════════════════════════════════════════════════════════
    // TEST 6: Rate limiting
    // ════════════════════════════════════════════════════════
    printSubSeparator("TEST 6: Rate Limiting");

    // Create a fresh rate limiter with low limit for testing
    RateLimiter test_rl(60, 5);  // 5 requests per 60 seconds
    SessionManager test_sm(hash_ring, SESSION_TIMEOUT_SEC);
    LoadBalancer test_lb(LBRole::PRIMARY, test_sm, hash_ring,
                         test_rl, security, backends);

    FiveTuple rate_test_ft;
    rate_test_ft.src_ip   = "172.16.0.1";
    rate_test_ft.src_port = 20000;
    rate_test_ft.dst_ip   = "10.0.0.1";
    rate_test_ft.dst_port = 80;
    rate_test_ft.protocol = "TCP";

    std::cout << "Sending 8 rapid requests (limit: 5 per 60s):\n\n";
    for (int i = 1; i <= 8; ++i) {
        resp = test_lb.handleRequest(rate_test_ft, rate_test_ft.serialize());
        std::cout << "  Request " << i << ": " << resp << "\n";
    }

    // ════════════════════════════════════════════════════════
    // TEST 7: Session blocking
    // ════════════════════════════════════════════════════════
    printSubSeparator("TEST 7: Dynamic Session Blocking");

    FiveTuple block_ft;
    block_ft.src_ip   = "10.10.10.1";
    block_ft.src_port = 30000;
    block_ft.dst_ip   = "10.0.0.1";
    block_ft.dst_port = 80;
    block_ft.protocol = "TCP";

    resp = primary_lb.handleRequest(block_ft, block_ft.serialize());
    std::cout << "Before block: " << resp << "\n";

    session_mgr.blockSession(block_ft.toKey(), true);
    resp = primary_lb.handleRequest(block_ft, block_ft.serialize());
    std::cout << "After block:  " << resp << "\n";

    session_mgr.blockSession(block_ft.toKey(), false);
    resp = primary_lb.handleRequest(block_ft, block_ft.serialize());
    std::cout << "After unblock:" << resp << "\n";

    // ════════════════════════════════════════════════════════
    // TEST 8: Force reassignment
    // ════════════════════════════════════════════════════════
    printSubSeparator("TEST 8: Force Reassignment");

    FiveTuple reassign_ft;
    reassign_ft.src_ip   = "10.20.30.1";
    reassign_ft.src_port = 40000;
    reassign_ft.dst_ip   = "10.0.0.1";
    reassign_ft.dst_port = 80;
    reassign_ft.protocol = "TCP";

    resp = primary_lb.handleRequest(reassign_ft, reassign_ft.serialize());
    std::cout << "Initial assignment: " << resp << "\n";

    session_mgr.forceReassign(reassign_ft.toKey());
    resp = primary_lb.handleRequest(reassign_ft, reassign_ft.serialize());
    std::cout << "After reassignment: " << resp << "\n";

    auto reassign_hist = session_mgr.getPathHistory(reassign_ft.toKey());
    std::cout << "\nPath history:\n";
    for (const auto& h : reassign_hist) {
        std::cout << "  Backend: " << h.backend_id
                  << " | Reason: " << h.reason << "\n";
    }

    // ════════════════════════════════════════════════════════
    // TEST 9: Load distribution check
    // ════════════════════════════════════════════════════════
    printSubSeparator("TEST 9: Load Distribution Across Backends");

    auto new_tuples = generateTestTuples(100);
    for (const auto& ft : new_tuples) {
        primary_lb.handleRequest(ft, ft.serialize());
    }

    std::cout << "Distribution of 100 sessions across " << NUM_BACKEND_SERVERS << " backends:\n\n";
    std::cout << std::left << std::setw(15) << "BACKEND"
              << std::setw(15) << "REQUESTS"
              << "SESSIONS" << "\n";
    std::cout << std::string(45, '-') << "\n";

    for (int i = 0; i < NUM_BACKEND_SERVERS; ++i) {
        std::cout << std::left << std::setw(15) << ("Backend " + std::to_string(i))
                  << std::setw(15) << backends[i]->totalRequestsProcessed()
                  << backends[i]->getLocalData().size() << "\n";
    }

    // ════════════════════════════════════════════════════════
    // TEST 10: Serialization / deserialization
    // ════════════════════════════════════════════════════════
    printSubSeparator("TEST 10: 5-Tuple Serialization");

    FiveTuple orig;
    orig.src_ip = "192.168.1.100";
    orig.src_port = 54321;
    orig.dst_ip = "10.0.0.5";
    orig.dst_port = 443;
    orig.protocol = "TCP";

    std::string serialized = orig.serialize();
    std::cout << "Serialized: " << serialized << "\n";

    FiveTuple deserialized;
    bool ok = FiveTuple::deserialize(serialized, deserialized);
    std::cout << "Deserialized OK: " << (ok ? "YES" : "NO") << "\n";
    std::cout << "Match: " << ((orig == deserialized) ? "YES" : "NO") << "\n";

    // ════════════════════════════════════════════════════════
    // Summary
    // ════════════════════════════════════════════════════════
    printSeparator("SIMULATION COMPLETE");

    std::cout << "Total active sessions: " << session_mgr.activeSessionCount() << "\n";
    std::cout << "Active backends:       " << hash_ring.serverCount() << "\n";
    std::cout << "\nCheck logs/ directory for detailed request and system logs.\n";
}

// ─────────────────────────────────────────────────────────────
// Main entry point
// ─────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    CLIArgs args = parseArgs(argc, argv);

    // Initialize logger
    if (!Logger::instance().init(args.logging_enabled)) {
        std::cerr << "FATAL: Failed to initialize logger.\n";
        return 1;
    }

    Logger::instance().logSystem(LogLevel::INFO,
        "Session Affinity Engine starting — mode: " + args.mode +
        ", logging: " + (args.logging_enabled ? "ON" : "OFF"));

    try {
        if (args.mode == "test") {
            std::cout << "Running in TEST mode — see tests/test_all.cpp\n";
            std::cout << "Build and run tests with: make test && ./run_tests\n";
        } else {
            runSimulation();
        }
    } catch (const std::exception& e) {
        std::cerr << "FATAL ERROR: " << e.what() << "\n";
        Logger::instance().logSystem(LogLevel::ERROR,
            "Fatal exception: " + std::string(e.what()));
        Logger::instance().shutdown();
        return 1;
    }

    Logger::instance().shutdown();
    return 0;
}
