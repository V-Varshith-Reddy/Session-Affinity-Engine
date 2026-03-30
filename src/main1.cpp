/**
 * @file main1.cpp
 * @brief Docker-ready entry point for the Session Affinity Engine.
 *
 * This version runs an actual infinite loop acting as a UDP server.
 * It listens on port 9000, receives incoming UDP packets, builds a
 * 5-tuple from the packet headers, and routes it through the LB.
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
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>

// Run an infinite loop UDP server
void runUDPServer() {
    std::cout << "Initializing Session Affinity Engine (Docker Mode)...\n";

    // Initialize core components exactly as in main.cpp
    ConsistentHashRing hash_ring(CONSISTENT_HASH_VNODES);
    RateLimiter rate_limiter(RATE_LIMIT_WINDOW_SEC, RATE_LIMIT_MAX_REQUESTS);
    Security security;
    SessionManager session_mgr(hash_ring, SESSION_TIMEOUT_SEC);

    // Create backend servers (simulated in-memory to avoid needing 8 other docker containers)
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

    auto last_primary_heartbeat = std::chrono::steady_clock::now();

    // ─────────────────────────────────────────────────────────
    // Real UDP Network Setup
    // ─────────────────────────────────────────────────────────
    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    char buffer[4096];

    // Create UDP socket
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        std::cerr << "FATAL: Socket creation failed.\n";
        exit(EXIT_FAILURE);
    }

    // Fill server info
    memset(&server_addr, 0, sizeof(server_addr));
    memset(&client_addr, 0, sizeof(client_addr));
    
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons(LB_PRIMARY_PORT);

    // Bind socket
    if (bind(sockfd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "FATAL: Bind failed on port " << LB_PRIMARY_PORT << ".\n";
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    std::cout << "✅ Server listening on UDP port " << LB_PRIMARY_PORT << "...\n";
    Logger::instance().logSystem(LogLevel::INFO, "Listening on UDP port " + std::to_string(LB_PRIMARY_PORT));

    socklen_t len = sizeof(client_addr);

    // Infinite loop processing live requests
    while (true) {
        // Receive packet from literal network
        int n = recvfrom(sockfd, (char *)buffer, sizeof(buffer) - 1, 
                         MSG_WAITALL, (struct sockaddr *)&client_addr, &len);
        
        if (n < 0) continue;
        buffer[n] = '\0';
        std::string payload(buffer);

        // Map the real UDP network headers into our 5-Tuple representation
        FiveTuple ft;
        ft.src_ip   = inet_ntoa(client_addr.sin_addr);
        ft.src_port = ntohs(client_addr.sin_port);
        ft.dst_ip   = "0.0.0.0"; // The bind address
        ft.dst_port = LB_PRIMARY_PORT;
        ft.protocol = "UDP";

        // ─────────────────────────────────────────────────────────
        // Failover Check & Routing Logic
        // ─────────────────────────────────────────────────────────
        std::string response;

        // Hidden testing hooks to toggle LB states via UDP packet
        if (payload == "CMD:KILL_PRIMARY") {
            primary_lb.deactivate();
            response = "CMD_ACK: Primary LB Killed.";
        } else if (payload == "CMD:REVIVE_PRIMARY") {
            primary_lb.activate();
            response = "CMD_ACK: Primary LB Revived.";
        } else {
            // Normal traffic routing logic
            if (primary_lb.isActive()) {
                last_primary_heartbeat = std::chrono::steady_clock::now();
                
                // Failback: If standby was active, the primary just recovered. Yield control.
                if (standby_lb.isActive()) {
                    standby_lb.deactivate();
                    Logger::instance().logSystem(LogLevel::INFO, "Primary LB recovered. Standby yielding control.");
                    std::cout << "\n✅ PRIMARY LB RECOVERED & RESUMED TRAFFIC. Standby yielded.\n";
                }
                
                response = primary_lb.handleRequest(ft, payload);
            } else {
                // Primary is INACTIVE. Standby checks if it should take over.
                if (!standby_lb.isActive() && standby_lb.shouldTakeover(last_primary_heartbeat)) {
                    standby_lb.activate();
                    Logger::instance().logSystem(LogLevel::WARNING, "Standby LB taking over traffic routing!");
                    std::cout << "\n⚠️ STANDBY LB HAS TAKEN OVER TRAFFIC ⚠️\n";
                }

                if (standby_lb.isActive()) {
                    response = standby_lb.handleRequest(ft, payload);
                } else {
                    response = "ERROR: No active Load Balancer available.";
                }
            }
        }

        // Send the backend's response back to the literal network client
        sendto(sockfd, response.c_str(), response.length(), 
               0, (const struct sockaddr *)&client_addr, len);
               
        // Also print to console so 'docker logs' shows live traffic
        std::cout << "[TRAFFIC] " << ft.toKey() << " -> " << response << "\n";
    }

    close(sockfd);
}

int main() {
    // Enable system and request logging by default in container
    if (!Logger::instance().init(true)) {
        std::cerr << "Logging init failed.\n";
        return 1;
    }

    try {
        runUDPServer();
    } catch (const std::exception& e) {
        std::cerr << "Server crashed: " << e.what() << "\n";
        Logger::instance().logSystem(LogLevel::ERROR, "Crash: " + std::string(e.what()));
    }
    
    Logger::instance().shutdown();
    return 0;
}
