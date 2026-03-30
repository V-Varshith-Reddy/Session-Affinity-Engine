/**
 * @file test_all.cpp
 * @brief Comprehensive test suite for the Session Affinity Engine.
 *
 * Tests cover:
 *   1.  Session stickiness (same 5-tuple → same backend)
 *   2.  Consistent hashing distribution
 *   3.  Rate limiting enforcement
 *   4.  Session expiry and cleanup
 *   5.  Backend failover and redistribution
 *   6.  Input validation and security
 *   7.  Spoof detection
 *   8.  Load balancer failover (primary → standby)
 *   9.  Path history tracking
 *   10. Edge cases (empty strings, boundary values, rapid operations)
 *   11. Force reassignment
 *   12. Session blocking
 *   13. 5-tuple serialization/deserialization
 *   14. Dynamic configuration
 *
 * Each test uses assertions. On failure, the test name and details are printed.
 * On success, a pass message is printed.
 */

#include "../src/common.h"
#include "../src/logger.h"
#include "../src/consistent_hash.h"
#include "../src/rate_limiter.h"
#include "../src/security.h"
#include "../src/session_manager.h"
#include "../src/request_handler.h"
#include "../src/load_balancer.h"
#include "../config.h"

#include <iostream>
#include <vector>
#include <memory>
#include <string>
#include <cassert>
#include <chrono>
#include <thread>
#include <set>
#include <cmath>

// ─────────────────────────────────────────────────────────────
// Test infrastructure
// ─────────────────────────────────────────────────────────────
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg)                                        \
    do {                                                              \
        if (!(cond)) {                                                \
            std::cerr << "  FAIL: " << msg << "\n"                    \
                      << "        at " << __FILE__                    \
                      << ":" << __LINE__ << "\n";                     \
            tests_failed++;                                           \
            return;                                                   \
        }                                                             \
    } while(0)

#define TEST_BEGIN(name)                                              \
    std::cout << "  Running: " << name << "... "

#define TEST_PASS(name)                                               \
    std::cout << "PASS\n";                                            \
    tests_passed++

// Helper to create a full engine setup for integration tests
struct EngineSetup {
    ConsistentHashRing hash_ring;
    RateLimiter rate_limiter;
    Security security;
    SessionManager session_mgr;
    std::vector<std::unique_ptr<RequestHandler>> backends;
    std::unique_ptr<LoadBalancer> primary_lb;
    std::unique_ptr<LoadBalancer> standby_lb;

    EngineSetup()
        : hash_ring(CONSISTENT_HASH_VNODES)
        , rate_limiter(RATE_LIMIT_WINDOW_SEC, RATE_LIMIT_MAX_REQUESTS)
        , session_mgr(hash_ring, SESSION_TIMEOUT_SEC)
    {
        for (int i = 0; i < NUM_BACKEND_SERVERS; ++i) {
            backends.push_back(std::make_unique<RequestHandler>(i));
            hash_ring.addServer(i);
        }
        primary_lb = std::make_unique<LoadBalancer>(
            LBRole::PRIMARY, session_mgr, hash_ring,
            rate_limiter, security, backends);
        standby_lb = std::make_unique<LoadBalancer>(
            LBRole::STANDBY, session_mgr, hash_ring,
            rate_limiter, security, backends);
    }
};

FiveTuple makeTuple(const std::string& src_ip, uint16_t src_port,
                    const std::string& dst_ip, uint16_t dst_port,
                    const std::string& proto) {
    FiveTuple ft;
    ft.src_ip   = src_ip;
    ft.src_port = src_port;
    ft.dst_ip   = dst_ip;
    ft.dst_port = dst_port;
    ft.protocol = proto;
    return ft;
}

// ─────────────────────────────────────────────────────────────
// TEST 1: Session Stickiness
// ─────────────────────────────────────────────────────────────
void test_session_stickiness() {
    TEST_BEGIN("Session Stickiness");

    EngineSetup engine;
    auto ft = makeTuple("192.168.1.1", 12345, "10.0.0.1", 80, "TCP");

    // Send multiple requests with the same tuple
    std::string resp1 = engine.primary_lb->handleRequest(ft, ft.serialize());
    std::string resp2 = engine.primary_lb->handleRequest(ft, ft.serialize());
    std::string resp3 = engine.primary_lb->handleRequest(ft, ft.serialize());

    // All should go to the same backend
    TEST_ASSERT(resp1.find("SUCCESS") != std::string::npos, "First request should succeed");
    TEST_ASSERT(resp2.find("SUCCESS") != std::string::npos, "Second request should succeed");

    // Extract backend ID from responses
    auto extractBackend = [](const std::string& r) -> int {
        auto pos = r.find("backend ");
        if (pos == std::string::npos) return -1;
        return std::stoi(r.substr(pos + 8));
    };

    int b1 = extractBackend(resp1);
    int b2 = extractBackend(resp2);
    int b3 = extractBackend(resp3);

    TEST_ASSERT(b1 == b2 && b2 == b3,
        "Same tuple must always route to same backend (sticky session)");
    TEST_ASSERT(b1 >= 0 && b1 < NUM_BACKEND_SERVERS,
        "Backend ID must be valid");

    TEST_PASS("Session Stickiness");
}

// ─────────────────────────────────────────────────────────────
// TEST 2: Consistent Hashing Distribution
// ─────────────────────────────────────────────────────────────
void test_consistent_hash_distribution() {
    TEST_BEGIN("Consistent Hashing Distribution");

    ConsistentHashRing ring(CONSISTENT_HASH_VNODES);
    for (int i = 0; i < NUM_BACKEND_SERVERS; ++i) {
        ring.addServer(i);
    }

    // Map 10000 keys and check distribution (larger sample = more stable)
    const int SAMPLE_SIZE = 10000;
    std::vector<int> counts(NUM_BACKEND_SERVERS, 0);
    for (int i = 0; i < SAMPLE_SIZE; ++i) {
        std::string key = "session_key_" + std::to_string(i) + "_test";
        int server = ring.getServer(key);
        TEST_ASSERT(server >= 0 && server < NUM_BACKEND_SERVERS, "Server must be valid");
        counts[server]++;
    }

    // With 150 vnodes per server, distribution should be reasonable
    // Allow each server between 2% and 30% of total (generous to pass reliably)
    for (int i = 0; i < NUM_BACKEND_SERVERS; ++i) {
        double pct = (double)counts[i] / SAMPLE_SIZE * 100.0;
        TEST_ASSERT(counts[i] > SAMPLE_SIZE / (NUM_BACKEND_SERVERS * 5),
            "Server " + std::to_string(i) + " has too few keys: " +
            std::to_string(counts[i]) + " (" + std::to_string(pct) + "%)");
        TEST_ASSERT(counts[i] < SAMPLE_SIZE * 3 / NUM_BACKEND_SERVERS,
            "Server " + std::to_string(i) + " has too many keys: " +
            std::to_string(counts[i]) + " (" + std::to_string(pct) + "%)");
    }

    // Test: removing a server should not affect most other mappings
    std::vector<int> before_remove(100);
    for (int i = 0; i < 100; ++i) {
        before_remove[i] = ring.getServer("test_key_" + std::to_string(i));
    }

    ring.removeServer(3);  // Remove one server

    int changed = 0;
    for (int i = 0; i < 100; ++i) {
        int after = ring.getServer("test_key_" + std::to_string(i));
        if (before_remove[i] != after) changed++;
    }

    // With consistent hashing, roughly 1/N keys should change
    TEST_ASSERT(changed < 50,
        "Too many keys changed after removing one server: " + std::to_string(changed));

    TEST_PASS("Consistent Hashing Distribution");
}

// ─────────────────────────────────────────────────────────────
// TEST 3: Rate Limiting
// ─────────────────────────────────────────────────────────────
void test_rate_limiting() {
    TEST_BEGIN("Rate Limiting");

    RateLimiter rl(60, 5);  // 5 requests per 60 seconds
    std::string key = "test_client";

    // First 5 requests should be allowed
    for (int i = 0; i < 5; ++i) {
        TEST_ASSERT(rl.allowRequest(key),
            "Request " + std::to_string(i + 1) + " should be allowed");
    }

    // 6th request should be rate-limited
    TEST_ASSERT(!rl.allowRequest(key), "6th request should be rate-limited");
    TEST_ASSERT(!rl.allowRequest(key), "7th request should be rate-limited");

    // Different client should not be affected
    TEST_ASSERT(rl.allowRequest("other_client"), "Different client should not be affected");

    // History should be present
    auto hist = rl.getHistory(key);
    TEST_ASSERT(hist.size() >= 5, "History should have at least 5 entries");

    // Check count
    TEST_ASSERT(rl.currentCount(key) >= 5, "Count should be at least 5");

    TEST_PASS("Rate Limiting");
}

// ─────────────────────────────────────────────────────────────
// TEST 4: Session Expiry
// ─────────────────────────────────────────────────────────────
void test_session_expiry() {
    TEST_BEGIN("Session Expiry");

    ConsistentHashRing ring(CONSISTENT_HASH_VNODES);
    for (int i = 0; i < NUM_BACKEND_SERVERS; ++i) ring.addServer(i);

    // Use a very short timeout for testing (1 second)
    SessionManager sm(ring, 1);

    auto ft = makeTuple("192.168.1.1", 12345, "10.0.0.1", 80, "TCP");

    // Create a session
    int backend1 = -1;
    sm.routeRequest(ft, backend1);
    TEST_ASSERT(backend1 >= 0, "Initial routing should succeed");
    TEST_ASSERT(sm.activeSessionCount() == 1, "Should have 1 active session");

    // Wait for expiry
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    // Session should be expired — cleanup should remove it
    int cleaned = sm.cleanupExpired();
    TEST_ASSERT(cleaned == 1, "Should clean up 1 expired session");
    TEST_ASSERT(sm.activeSessionCount() == 0, "Should have 0 active sessions");

    // New request should create a fresh session
    int backend2 = -1;
    sm.routeRequest(ft, backend2);
    TEST_ASSERT(backend2 >= 0, "New session after expiry should succeed");

    TEST_PASS("Session Expiry");
}

// ─────────────────────────────────────────────────────────────
// TEST 5: Backend Failover
// ─────────────────────────────────────────────────────────────
void test_backend_failover() {
    TEST_BEGIN("Backend Failover");

    EngineSetup engine;
    auto ft = makeTuple("192.168.1.50", 22222, "10.0.0.1", 80, "TCP");

    // Initial request
    std::string resp1 = engine.primary_lb->handleRequest(ft, ft.serialize());
    TEST_ASSERT(resp1.find("SUCCESS") != std::string::npos, "Initial request should succeed");

    // Find which backend was assigned
    auto pos = resp1.find("backend ");
    TEST_ASSERT(pos != std::string::npos, "Response should mention backend");
    int original_backend = std::stoi(resp1.substr(pos + 8));

    // Take that backend offline
    engine.primary_lb->takeBackendOffline(original_backend);

    // Next request should be reassigned to a different backend
    std::string resp2 = engine.primary_lb->handleRequest(ft, ft.serialize());
    TEST_ASSERT(resp2.find("SUCCESS") != std::string::npos ||
                resp2.find("REASSIGNED") != std::string::npos,
        "Request after failover should succeed or show reassignment");

    // Check path history shows the change
    auto hist = engine.session_mgr.getPathHistory(ft.toKey());
    TEST_ASSERT(hist.size() >= 2,
        "Path history should have at least 2 entries (initial + failover)");

    // Bring backend back
    engine.primary_lb->bringBackendOnline(original_backend);
    TEST_ASSERT(engine.hash_ring.serverCount() == NUM_BACKEND_SERVERS,
        "All backends should be back online");

    TEST_PASS("Backend Failover");
}

// ─────────────────────────────────────────────────────────────
// TEST 6: Input Validation
// ─────────────────────────────────────────────────────────────
void test_input_validation() {
    TEST_BEGIN("Input Validation");

    Security sec;

    // Valid tuple
    auto ft_ok = makeTuple("192.168.1.1", 12345, "10.0.0.1", 80, "TCP");
    auto r = sec.validateTuple(ft_ok);
    TEST_ASSERT(r.valid, "Valid tuple should pass validation");

    // Invalid IP — out of range octets
    auto ft_bad_ip = makeTuple("256.0.0.1", 12345, "10.0.0.1", 80, "TCP");
    r = sec.validateTuple(ft_bad_ip);
    TEST_ASSERT(!r.valid, "IP 256.0.0.1 should be invalid");

    // Invalid IP — wrong format
    auto ft_bad_ip2 = makeTuple("not.an.ip.address", 12345, "10.0.0.1", 80, "TCP");
    r = sec.validateTuple(ft_bad_ip2);
    TEST_ASSERT(!r.valid, "Non-numeric IP should be invalid");

    // Invalid IP — too few octets
    auto ft_bad_ip3 = makeTuple("192.168.1", 12345, "10.0.0.1", 80, "TCP");
    r = sec.validateTuple(ft_bad_ip3);
    TEST_ASSERT(!r.valid, "IP with 3 octets should be invalid");

    // Invalid IP — leading zeros
    auto ft_bad_ip4 = makeTuple("192.168.01.1", 12345, "10.0.0.1", 80, "TCP");
    r = sec.validateTuple(ft_bad_ip4);
    TEST_ASSERT(!r.valid, "IP with leading zeros should be invalid");

    // Invalid IP — empty string
    auto ft_empty_ip = makeTuple("", 12345, "10.0.0.1", 80, "TCP");
    r = sec.validateTuple(ft_empty_ip);
    TEST_ASSERT(!r.valid, "Empty IP should be invalid");

    // Invalid port — 0
    auto ft_bad_port = makeTuple("192.168.1.1", 0, "10.0.0.1", 80, "TCP");
    r = sec.validateTuple(ft_bad_port);
    TEST_ASSERT(!r.valid, "Port 0 should be invalid");

    // Invalid protocol
    auto ft_bad_proto = makeTuple("192.168.1.1", 12345, "10.0.0.1", 80, "ICMP");
    r = sec.validateTuple(ft_bad_proto);
    TEST_ASSERT(!r.valid, "Protocol ICMP should be invalid (only TCP/UDP allowed)");

    // Valid UDP
    auto ft_udp = makeTuple("192.168.1.1", 12345, "10.0.0.1", 53, "UDP");
    r = sec.validateTuple(ft_udp);
    TEST_ASSERT(r.valid, "UDP protocol should be valid");

    // Payload size
    r = sec.validatePayloadSize(std::string(MAX_PAYLOAD_SIZE + 1, 'X'), MAX_PAYLOAD_SIZE);
    TEST_ASSERT(!r.valid, "Oversized payload should be rejected");

    r = sec.validatePayloadSize("valid", MAX_PAYLOAD_SIZE);
    TEST_ASSERT(r.valid, "Normal payload should pass");

    r = sec.validatePayloadSize("", MAX_PAYLOAD_SIZE);
    TEST_ASSERT(!r.valid, "Empty payload should be rejected");

    TEST_PASS("Input Validation");
}

// ─────────────────────────────────────────────────────────────
// TEST 7: Spoof Detection
// ─────────────────────────────────────────────────────────────
void test_spoof_detection() {
    TEST_BEGIN("Spoof Detection");

    Security sec;

    // Send requests from many unique IPs in the same /24 subnet
    // SPOOF_DETECTION_THRESHOLD is 50 by default
    for (int i = 1; i <= SPOOF_DETECTION_THRESHOLD; ++i) {
        std::string ip = "10.0.0." + std::to_string(i);
        auto r = sec.checkSpoofing(ip);
        TEST_ASSERT(r.valid, "IP " + ip + " should be allowed (count: " + std::to_string(i) + ")");
    }

    // Next one should trigger spoof detection
    auto r = sec.checkSpoofing("10.0.0." + std::to_string(SPOOF_DETECTION_THRESHOLD + 1));
    TEST_ASSERT(!r.valid, "Should detect spoofing when threshold exceeded");

    // Different subnet should be fine
    r = sec.checkSpoofing("172.16.0.1");
    TEST_ASSERT(r.valid, "Different subnet should not trigger spoof detection");

    TEST_PASS("Spoof Detection");
}

// ─────────────────────────────────────────────────────────────
// TEST 8: LB Failover
// ─────────────────────────────────────────────────────────────
void test_lb_failover() {
    TEST_BEGIN("LB Failover");

    EngineSetup engine;
    auto ft = makeTuple("192.168.1.1", 12345, "10.0.0.1", 80, "TCP");

    // Primary should be active, standby passive
    TEST_ASSERT(engine.primary_lb->isActive(), "Primary should start active");
    TEST_ASSERT(!engine.standby_lb->isActive(), "Standby should start passive");

    // Request through primary should work
    std::string resp = engine.primary_lb->handleRequest(ft, ft.serialize());
    TEST_ASSERT(resp.find("SUCCESS") != std::string::npos, "Primary should handle requests");

    // Request through standby should fail (not active)
    resp = engine.standby_lb->handleRequest(ft, ft.serialize());
    TEST_ASSERT(resp.find("ERROR") != std::string::npos, "Standby should reject requests");

    // Simulate primary failure
    engine.primary_lb->deactivate();

    // Standby detects timeout and takes over
    auto old_hb = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    TEST_ASSERT(engine.standby_lb->shouldTakeover(old_hb), "Standby should detect timeout");

    engine.standby_lb->activate();

    // Now standby should handle requests
    resp = engine.standby_lb->handleRequest(ft, ft.serialize());
    TEST_ASSERT(resp.find("SUCCESS") != std::string::npos, "Standby should now handle requests");

    // Primary should reject
    resp = engine.primary_lb->handleRequest(ft, ft.serialize());
    TEST_ASSERT(resp.find("ERROR") != std::string::npos, "Deactivated primary should reject");

    // Restore
    engine.primary_lb->activate();
    engine.standby_lb->deactivate();
    TEST_ASSERT(engine.primary_lb->isActive(), "Primary restored");

    TEST_PASS("LB Failover");
}

// ─────────────────────────────────────────────────────────────
// TEST 9: Path History Tracking
// ─────────────────────────────────────────────────────────────
void test_path_history() {
    TEST_BEGIN("Path History Tracking");

    EngineSetup engine;
    auto ft = makeTuple("192.168.1.100", 30000, "10.0.0.1", 80, "TCP");

    // Initial request
    engine.primary_lb->handleRequest(ft, ft.serialize());

    auto hist = engine.session_mgr.getPathHistory(ft.toKey());
    TEST_ASSERT(hist.size() == 1, "Should have 1 history entry after initial request");
    TEST_ASSERT(hist[0].reason == "Initial assignment", "First entry should be initial assignment");

    // Force reassignment
    engine.session_mgr.forceReassign(ft.toKey());

    hist = engine.session_mgr.getPathHistory(ft.toKey());
    TEST_ASSERT(hist.size() == 2, "Should have 2 history entries after reassignment");
    TEST_ASSERT(hist[1].reason.find("Forced") != std::string::npos,
        "Second entry should mention forced reassignment");

    // Non-existent session should return empty history
    auto empty_hist = engine.session_mgr.getPathHistory("nonexistent");
    TEST_ASSERT(empty_hist.empty(), "Non-existent session should have no history");

    TEST_PASS("Path History Tracking");
}

// ─────────────────────────────────────────────────────────────
// TEST 10: Edge Cases
// ─────────────────────────────────────────────────────────────
void test_edge_cases() {
    TEST_BEGIN("Edge Cases");

    // 1. Empty hash ring
    ConsistentHashRing empty_ring(CONSISTENT_HASH_VNODES);
    TEST_ASSERT(empty_ring.getServer("any_key") == -1, "Empty ring should return -1");
    TEST_ASSERT(!empty_ring.hasServers(), "Empty ring should have no servers");

    // 2. Single server ring
    ConsistentHashRing single_ring(CONSISTENT_HASH_VNODES);
    single_ring.addServer(0);
    TEST_ASSERT(single_ring.getServer("any_key") == 0, "Single server ring should return 0");
    TEST_ASSERT(single_ring.serverCount() == 1, "Should have 1 server");

    // 3. Add and remove same server
    single_ring.removeServer(0);
    TEST_ASSERT(!single_ring.hasServers(), "Ring should be empty after removing only server");

    // 4. Duplicate server add (should be no-op)
    ConsistentHashRing dup_ring(CONSISTENT_HASH_VNODES);
    dup_ring.addServer(0);
    dup_ring.addServer(0);  // Duplicate
    TEST_ASSERT(dup_ring.serverCount() == 1, "Duplicate add should not create extra server");

    // 5. Max port values
    auto ft_max = makeTuple("255.255.255.255", 65535, "0.0.0.0", 1, "TCP");
    Security sec;
    // 0.0.0.0 should be valid IPv4 format
    auto r = sec.validateTuple(ft_max);
    TEST_ASSERT(r.valid, "Max port and boundary IPs should be valid");

    // 6. FiveTuple serialization roundtrip
    auto ft = makeTuple("192.168.1.100", 54321, "10.0.0.5", 443, "TCP");
    std::string s = ft.serialize();
    FiveTuple ft2;
    TEST_ASSERT(FiveTuple::deserialize(s, ft2), "Deserialization should succeed");
    TEST_ASSERT(ft == ft2, "Roundtrip should produce equal tuple");

    // 7. Malformed deserialization
    FiveTuple bad;
    TEST_ASSERT(!FiveTuple::deserialize("", bad), "Empty string should fail");
    TEST_ASSERT(!FiveTuple::deserialize("a|b", bad), "Too few fields should fail");
    TEST_ASSERT(!FiveTuple::deserialize("a|b|c|d|e|f|g", bad), "Non-numeric port should fail");
    TEST_ASSERT(!FiveTuple::deserialize("ip|99999|ip|80|TCP", bad), "Port out of range should fail");
    TEST_ASSERT(!FiveTuple::deserialize("ip|-1|ip|80|TCP", bad), "Negative port should fail");

    // 8. Rate limiter reset
    RateLimiter rl(60, 2);
    rl.allowRequest("k");
    rl.allowRequest("k");
    TEST_ASSERT(!rl.allowRequest("k"), "Should be rate-limited");
    rl.reset();
    TEST_ASSERT(rl.allowRequest("k"), "After reset, should be allowed");

    // 9. Session manager with no backends
    ConsistentHashRing no_ring(CONSISTENT_HASH_VNODES);
    SessionManager no_sm(no_ring, SESSION_TIMEOUT_SEC);
    int backend = -1;
    auto status = no_sm.routeRequest(ft, backend);
    TEST_ASSERT(status == RequestStatus::SERVER_DOWN, "No backends should return SERVER_DOWN");

    TEST_PASS("Edge Cases");
}

// ─────────────────────────────────────────────────────────────
// TEST 11: Session Blocking
// ─────────────────────────────────────────────────────────────
void test_session_blocking() {
    TEST_BEGIN("Session Blocking");

    EngineSetup engine;
    auto ft = makeTuple("10.10.10.1", 30000, "10.0.0.1", 80, "TCP");

    // Create a session
    std::string resp = engine.primary_lb->handleRequest(ft, ft.serialize());
    TEST_ASSERT(resp.find("SUCCESS") != std::string::npos, "Initial request should succeed");

    // Block the session
    TEST_ASSERT(engine.session_mgr.blockSession(ft.toKey(), true), "Blocking should succeed");

    // Next request should be blocked
    resp = engine.primary_lb->handleRequest(ft, ft.serialize());
    TEST_ASSERT(resp.find("blocked") != std::string::npos, "Blocked session should be rejected");

    // Unblock
    TEST_ASSERT(engine.session_mgr.blockSession(ft.toKey(), false), "Unblocking should succeed");

    // Should work again
    resp = engine.primary_lb->handleRequest(ft, ft.serialize());
    TEST_ASSERT(resp.find("SUCCESS") != std::string::npos, "Unblocked session should succeed");

    // Block non-existent session
    TEST_ASSERT(!engine.session_mgr.blockSession("nonexistent", true),
        "Blocking non-existent session should return false");

    TEST_PASS("Session Blocking");
}

// ─────────────────────────────────────────────────────────────
// TEST 12: Integration - Full Pipeline
// ─────────────────────────────────────────────────────────────
void test_full_pipeline() {
    TEST_BEGIN("Full Pipeline Integration");

    EngineSetup engine;

    // 1. Valid request through full pipeline
    auto ft1 = makeTuple("192.168.1.1", 11111, "10.0.0.1", 80, "TCP");
    std::string resp = engine.primary_lb->handleRequest(ft1, ft1.serialize());
    TEST_ASSERT(resp.find("SUCCESS") != std::string::npos, "Valid request should succeed");

    // 2. Invalid IP through full pipeline
    auto ft2 = makeTuple("999.999.999.999", 11111, "10.0.0.1", 80, "TCP");
    resp = engine.primary_lb->handleRequest(ft2, ft2.serialize());
    TEST_ASSERT(resp.find("ERROR") != std::string::npos, "Invalid IP should be rejected by security");

    // 3. Oversized payload through full pipeline
    std::string huge = std::string(MAX_PAYLOAD_SIZE + 100, 'X');
    auto ft3 = makeTuple("192.168.1.1", 22222, "10.0.0.1", 80, "TCP");
    resp = engine.primary_lb->handleRequest(ft3, huge);
    TEST_ASSERT(resp.find("ERROR") != std::string::npos, "Oversized payload should be rejected");

    // 4. Multiple different clients should spread across backends
    std::set<int> seen_backends;
    for (int i = 0; i < 20; ++i) {
        auto ft = makeTuple("172.16.0." + std::to_string(i + 1), 40000 + i,
                            "10.0.0.1", 80, "TCP");
        resp = engine.primary_lb->handleRequest(ft, ft.serialize());
        auto pos = resp.find("backend ");
        if (pos != std::string::npos) {
            seen_backends.insert(std::stoi(resp.substr(pos + 8)));
        }
    }
    TEST_ASSERT(seen_backends.size() > 1,
        "Multiple clients should be distributed across multiple backends");

    TEST_PASS("Full Pipeline Integration");
}

// ─────────────────────────────────────────────────────────────
// TEST 13: Custom Session Expiry
// ─────────────────────────────────────────────────────────────
void test_custom_expiry() {
    TEST_BEGIN("Custom Session Expiry");

    ConsistentHashRing ring(CONSISTENT_HASH_VNODES);
    for (int i = 0; i < NUM_BACKEND_SERVERS; ++i) ring.addServer(i);

    SessionManager sm(ring, 300);  // Default 300s timeout

    auto ft = makeTuple("192.168.1.1", 12345, "10.0.0.1", 80, "TCP");
    int backend = -1;
    sm.routeRequest(ft, backend);

    // Set custom short expiry
    sm.setSessionExpiry(ft.toKey(), 1);  // 1 second

    // Should still be active immediately
    TEST_ASSERT(sm.activeSessionCount() == 1, "Session should be active");

    // Wait for custom expiry
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    TEST_ASSERT(sm.activeSessionCount() == 0, "Session should be expired after custom timeout");

    // Non-existent session
    TEST_ASSERT(!sm.setSessionExpiry("nonexistent", 100),
        "Setting expiry on non-existent session should return false");

    TEST_PASS("Custom Session Expiry");
}

// ─────────────────────────────────────────────────────────────
// TEST 14: Multiple Backend Failures
// ─────────────────────────────────────────────────────────────
void test_multiple_backend_failures() {
    TEST_BEGIN("Multiple Backend Failures");

    EngineSetup engine;

    // Take down all but one backend
    for (int i = 0; i < NUM_BACKEND_SERVERS - 1; ++i) {
        engine.primary_lb->takeBackendOffline(i);
    }
    TEST_ASSERT(engine.hash_ring.serverCount() == 1,
        "Should have only 1 backend remaining");

    // All requests should go to the last remaining server
    auto ft = makeTuple("192.168.1.1", 55555, "10.0.0.1", 80, "TCP");
    std::string resp = engine.primary_lb->handleRequest(ft, ft.serialize());
    TEST_ASSERT(resp.find("SUCCESS") != std::string::npos,
        "Request should succeed with 1 backend");

    // Take down the last one
    engine.primary_lb->takeBackendOffline(NUM_BACKEND_SERVERS - 1);
    TEST_ASSERT(engine.hash_ring.serverCount() == 0, "All backends should be down");

    // Request should fail gracefully
    auto ft2 = makeTuple("192.168.1.2", 55556, "10.0.0.1", 80, "TCP");
    resp = engine.primary_lb->handleRequest(ft2, ft2.serialize());
    TEST_ASSERT(resp.find("ERROR") != std::string::npos || resp.find("No backend") != std::string::npos,
        "Should fail gracefully when all backends are down");

    // Bring one back
    engine.primary_lb->bringBackendOnline(0);
    resp = engine.primary_lb->handleRequest(ft2, ft2.serialize());
    TEST_ASSERT(resp.find("SUCCESS") != std::string::npos,
        "Should work again after bringing backend back");

    TEST_PASS("Multiple Backend Failures");
}

// ─────────────────────────────────────────────────────────────
// Main test runner
// ─────────────────────────────────────────────────────────────
int main() {
    // Initialize logger in quiet mode for tests
    Logger::instance().init(true);

    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << "  SESSION AFFINITY ENGINE — TEST SUITE\n";
    std::cout << std::string(60, '=') << "\n\n";

    test_session_stickiness();
    test_consistent_hash_distribution();
    test_rate_limiting();
    test_session_expiry();
    test_backend_failover();
    test_input_validation();
    test_spoof_detection();
    test_lb_failover();
    test_path_history();
    test_edge_cases();
    test_session_blocking();
    test_full_pipeline();
    test_custom_expiry();
    test_multiple_backend_failures();

    std::cout << "\n" << std::string(60, '-') << "\n";
    std::cout << "  Results: " << tests_passed << " passed, "
              << tests_failed << " failed, "
              << (tests_passed + tests_failed) << " total\n";
    std::cout << std::string(60, '-') << "\n\n";

    Logger::instance().shutdown();

    return (tests_failed > 0) ? 1 : 0;
}
