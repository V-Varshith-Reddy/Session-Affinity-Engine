# Session Affinity Engine

A high-performance C++ Layer-4 load balancer simulation implementing sticky sessions based on the 5-tuple `(src_ip, src_port, dst_ip, dst_port, protocol)`. Features consistent hashing, dual-LB failover, sliding window rate limiting, comprehensive security checks, and full request logging.

---

## Directory Structure

```
Check_Point/
├── config.h                    # All configurable parameters (#define macros)
├── Makefile                    # Build system
├── README.md                   # This file
├── Session Affinity Engine.md  # Architecture & design documentation
├── problem_statement.txt       # Original problem statement
├── src/
│   ├── common.h                # Shared types: FiveTuple, hashing, serialization
│   ├── consistent_hash.h/cpp   # Consistent hashing ring with virtual nodes
│   ├── rate_limiter.h/cpp      # Sliding window queue-based rate limiter
│   ├── security.h/cpp          # Input validation, spoof detection
│   ├── logger.h/cpp            # Thread-safe, file-based logging system
│   ├── session_manager.h/cpp   # Session table: sticky routing, expiry, history
│   ├── request_handler.h/cpp   # Backend server instances
│   ├── load_balancer.h/cpp     # Primary + standby LB with failover
│   └── main.cpp                # Entry point, simulation runner, CLI parsing
├── tests/
│   └── test_all.cpp            # 14 comprehensive test categories
├── build/                      # Compiled object files (generated)
└── logs/                       # Runtime logs (generated)
    ├── requests.log            # Per-request audit trail
    └── system.log              # System events (failover, server up/down)
```

---

## Prerequisites

- **C++17 compatible compiler** (g++ or clang++)
- **macOS / Linux** (POSIX-compatible)
- No external dependencies — uses only the C++ Standard Library

---

## Build & Run Commands

### Build the Main Binary
```bash
make all
```

### Run the Simulation (exercises all features)
```bash
./session_engine --logging on --mode sim
```

### Build and Run Tests (14 test categories)
```bash
make test
```

### Build with Debug Symbols
```bash
make debug
```

### Run Simulation Directly
```bash
make run
```

### Clean Build Artifacts
```bash
make clean
```

### CLI Arguments
| Argument         | Values       | Default | Description                        |
|------------------|--------------|---------|------------------------------------|
| `--logging`      | `on` / `off` | `on`    | Enable/disable file-based logging  |
| `--mode`         | `sim` / `test` | `sim` | Run simulation or test mode        |
| `--help`         | —            | —       | Show help message                  |

---

## Configuration

All parameters are in **`config.h`** as `#define` macros — change and recompile:

| Parameter                    | Default | Description                              |
|------------------------------|---------|------------------------------------------|
| `NUM_BACKEND_SERVERS`        | 8       | Number of backend server instances       |
| `SESSION_TIMEOUT_SEC`        | 300     | Session idle timeout (seconds)           |
| `RATE_LIMIT_WINDOW_SEC`      | 60      | Sliding window duration (seconds)        |
| `RATE_LIMIT_MAX_REQUESTS`    | 100     | Max requests per client per window       |
| `LB_PRIMARY_PORT`            | 9000    | Primary load balancer port               |
| `LB_STANDBY_PORT`            | 9001    | Standby load balancer port               |
| `BACKEND_BASE_PORT`          | 8001    | Backend servers listen on BASE + id      |
| `LB_HEARTBEAT_INTERVAL_MS`   | 1000    | Primary→standby heartbeat interval (ms)  |
| `LB_FAILOVER_THRESHOLD_MS`   | 3000    | Standby takeover if no heartbeat (ms)    |
| `CONSISTENT_HASH_VNODES`     | 150     | Virtual nodes per server on hash ring    |
| `MAX_PAYLOAD_SIZE`           | 1024    | Maximum request payload (bytes)          |
| `SPOOF_DETECTION_THRESHOLD`  | 50      | Max unique IPs per /24 subnet per window |

---

## Test Output

```
============================================================
  SESSION AFFINITY ENGINE — TEST SUITE
============================================================

  Running: Session Stickiness... PASS
  Running: Consistent Hashing Distribution... PASS
  Running: Rate Limiting... PASS
  Running: Session Expiry... PASS
  Running: Backend Failover... PASS
  Running: Input Validation... PASS
  Running: Spoof Detection... PASS
  Running: LB Failover... PASS
  Running: Path History Tracking... PASS
  Running: Edge Cases... PASS
  Running: Session Blocking... PASS
  Running: Full Pipeline Integration... PASS
  Running: Custom Session Expiry... PASS
  Running: Multiple Backend Failures... PASS

------------------------------------------------------------
  Results: 14 passed, 0 failed, 14 total
------------------------------------------------------------
```

---

## Sample Simulation Output

```
======================================================================
  SESSION AFFINITY ENGINE — SIMULATION
======================================================================

Configuration:
  Backend servers:     8
  Session timeout:     300s
  Rate limit:          100 req/60s
  Hash ring vnodes:    150
  LB Primary port:     9000
  LB Standby port:     9001
  Heartbeat interval:  1000ms
  Failover threshold:  3000ms

--- TEST 1: Sticky Session Routing ---

5-TUPLE                                      REQ #       RESPONSE
------------------------------------------------------------------------------------------
192.168.1.1:62542->10.0.0.1:80/UDP           1           SUCCESS: Processed by backend 7
192.168.1.1:62542->10.0.0.1:80/UDP           2           SUCCESS: Processed by backend 7
192.168.1.1:62542->10.0.0.1:80/UDP           3           SUCCESS: Processed by backend 7

--- TEST 3: Backend Failure & Redistribution ---

Taking backend 0 offline...
Active backends after failure: 7
192.168.1.10:22113->10.0.0.2:80/UDP          SUCCESS: Processed by backend 2
  (previously on backend 0 — automatically redistributed)

--- TEST 4: Load Balancer Failover ---

Simulating primary LB failure...
Request via PRIMARY: ERROR: Load balancer [PRIMARY] is not active
Standby detected primary timeout — TAKING OVER!
Request via STANDBY: SUCCESS: Processed by backend 7

--- TEST 5: Security — Malformed Input Rejection ---

Invalid IP:       ERROR: Security validation failed — Invalid source IP: 999.999.999.999
Invalid port:     ERROR: Security validation failed — Invalid source port: 0
Invalid protocol: ERROR: Security validation failed — Invalid protocol: ICMP
Oversized payload:ERROR: Security validation failed — Payload too large: 1124 bytes

--- TEST 6: Rate Limiting ---

Sending 8 rapid requests (limit: 5 per 60s):
  Request 1-5: SUCCESS
  Request 6-8: ERROR: Rate limit exceeded

--- TEST 7: Dynamic Session Blocking ---

Before block: SUCCESS: Processed by backend 4
After block:  ERROR: Session blocked by administrator policy
After unblock:SUCCESS: Processed by backend 4

--- TEST 8: Force Reassignment ---

Initial assignment: SUCCESS: backend 0
After reassignment: SUCCESS: backend 1
Path history:
  Backend: 0 | Reason: Initial assignment
  Backend: 1 | Reason: Forced reassignment from backend 0

--- TEST 9: Load Distribution Across Backends ---

BACKEND        REQUESTS       SESSIONS
---------------------------------------------
Backend 0      8              6
Backend 1      7              7
Backend 2      22             13
Backend 3      9              5
Backend 4      13             4
Backend 5      11             7
Backend 6      5              1
Backend 7      25             12
```

---

## Request Log Format (`logs/requests.log`)

```
[2026-03-30 10:43:31.322] [192.168.1.1:62542->10.0.0.1:80/UDP] [NEW_SESSION] [SUCCESS] Assigned to backend 7
[2026-03-30 10:43:31.322] [192.168.1.1:62542->10.0.0.1:80/UDP] [ROUTE] [SUCCESS] Sticky session -> backend 7
[2026-03-30 10:43:31.323] [999.999.999.999:12345->10.0.0.1:80/TCP] [SECURITY_CHECK] [REJECTED] Invalid source IP
[2026-03-30 10:43:31.323] [172.16.0.1:20000->10.0.0.1:80/TCP] [RATE_LIMIT] [REJECTED] Rate limit exceeded
```

---

## System Log Format (`logs/system.log`)

```
[2026-03-30 10:43:31.320] [INFO] Logger initialized successfully.
[2026-03-30 10:43:31.320] [INFO] ConsistentHash: Added server 0 with 150 vnodes. Ring size: 150
[2026-03-30 10:43:31.322] [WARNING] Backend server 0 is now OFFLINE
[2026-03-30 10:43:31.322] [WARNING] Server 0 went down. Reassigned 1 sessions.
[2026-03-30 10:43:31.322] [INFO] LoadBalancer [STANDBY] is now ACTIVE
```

---

## How to Test Edge Cases

| Scenario | How to Test |
|----------|-------------|
| **Sticky sessions** | Send same 5-tuple 3x — verify same backend each time |
| **Backend failure** | Kill a backend mid-traffic → sessions automatically redistributed |
| **LB failover** | Deactivate primary → standby takes over via heartbeat timeout |
| **Rate limiting** | Send > `RATE_LIMIT_MAX_REQUESTS` from same client in window |
| **Invalid IP** | Send "999.999.999.999" — rejected with error message |
| **Port 0** | Send port 0 — rejected (valid range: 1-65535) |
| **Invalid protocol** | Send "ICMP" — only TCP/UDP accepted |
| **Oversized payload** | Send > `MAX_PAYLOAD_SIZE` bytes — rejected |
| **Spoof detection** | Send from > 50 unique IPs in same /24 subnet |
| **Session blocking** | Block a session via `blockSession()` API |
| **Force reassignment** | Call `forceReassign()` — session moves to different backend |
| **Session expiry** | Wait > `SESSION_TIMEOUT_SEC` — next request creates new session |
| **All backends down** | Take all servers offline — error returned gracefully |
| **Empty hash ring** | No servers added — returns -1 (no crash) |
