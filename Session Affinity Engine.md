# Session Affinity Engine — Architecture & Design Deep Dive

## 1. Executive Summary

The Session Affinity Engine is a C++17 simulation of a Layer-4 load balancer that maintains **sticky sessions** based on the network 5-tuple `(src_ip, src_port, dst_ip, dst_port, protocol)`. It routes client requests to backend server instances using **consistent hashing**, supports **dual load balancer failover**, enforces **sliding window rate limiting**, and includes **defense-in-depth security** measures.

This document provides a comprehensive explanation of the architecture, design choices, tradeoffs, bottlenecks, scalability considerations, and scope for improvement.

---

## 2. System Architecture

### 2.1 Block Diagram

![Session Affinity Engine — Architecture Block Diagram](architecture_block_diagram.png)

### 2.2 Request Flow — Sequence Diagram

![Request Flow — Full Pipeline Sequence](request_flow_diagram.png)

### 2.3 Failover Scenarios — Backend & Load Balancer

![Failover Diagrams — Backend Server & Load Balancer](failover_diagram.png)

### 2.4 Security Validation Flowchart

![Security Validation Pipeline](security_flowchart.png)

---

## 3. Request Processing Pipeline

Every request goes through a 6-stage pipeline:

| Stage | Module | Action | Failure Mode |
|-------|--------|--------|--------------|
| 1 | **LB Active Check** | Verify this LB is active | `ERROR: LB not active` |
| 2 | **Security Validation** | Validate IP, port, protocol, payload size | `ERROR: Security validation failed` |
| 3 | **Rate Limiting** | Check sliding window count | `ERROR: Rate limit exceeded` |
| 4 | **Session Routing** | Lookup/create sticky session | `ERROR: No backends available` |
| 5 | **Backend Forwarding** | Forward to assigned RequestHandler | Triggers failover if backend offline |
| 6 | **Response Relay** | Return response to client | — |

**Fail-fast design**: Each stage short-circuits on failure, avoiding unnecessary processing.

---

## 4. Component Deep Dive

### 4.1 Five-Tuple Session Key (`common.h`)

**Design Choice**: The 5-tuple is hashed into a single string key `"src_ip:src_port->dst_ip:dst_port/protocol"` for use as `unordered_map` keys.

**Hash Function**: Uses XOR + bit-shift combining (Boost-style) with `std::hash` components:
```
h ^= hash(field) + 0x9e3779b9 + (h << 6) + (h >> 2)
```

**Tradeoff**:
- ✅ Simple, fast, and produces good distribution
- ❌ Not cryptographic — susceptible to hash collision attacks in adversarial settings
- **Mitigation**: Rate limiting and spoof detection add defense-in-depth

### 4.2 Consistent Hashing Ring (`consistent_hash.h/cpp`)

**How it works**:
1. Each physical server gets `CONSISTENT_HASH_VNODES` (150) virtual nodes
2. Virtual node key = `"server_<id>_vnode_<i>"` → hashed to ring position
3. Ring stored as `std::map<size_t, int>` (ordered by hash value)
4. Lookup: hash the session key → `lower_bound()` → walk clockwise → return server

**Hash Function**: FNV-1a for better distribution than `std::hash`:
```
hash = 14695981039346656037 (FNV offset)
for each char: hash ^= char; hash *= 1099511628211 (FNV prime)
```

**Complexity**:
| Operation | Time | Space |
|-----------|------|-------|
| Lookup | O(log N) | O(V × S) |
| Add server | O(V × log N) | O(V) |
| Remove server | O(V × log N) | — |

Where N = total virtual nodes, V = vnodes per server, S = number of servers.

**Tradeoff**:
- ✅ Adding/removing a server only affects ~1/N of sessions (minimal disruption)
- ✅ Virtual nodes ensure load distribution even with few physical servers
- ❌ More vnodes = more memory + slower add/remove (but faster is not needed often)
- ❌ Distribution is approximate, not perfectly uniform (some servers get up to 2x expected)

**Why 150 virtual nodes?**
- Industry standard range is 100-200; 150 gives good distribution without excessive memory
- With 8 servers: 1200 ring entries = ~19KB — negligible

### 4.3 Session Manager (`session_manager.h/cpp`)

**Data Structure**: `std::unordered_map<string, SessionEntry>`

**Why unordered_map?**:
- O(1) average lookup, insert, delete — critical for high-throughput routing
- The 5-tuple key string is hashed by the standard hash for bucket placement
- No need for ordered iteration over sessions

**Sticky Session Logic**:
```
routeRequest(tuple):
  key = tuple.toKey()
  if sessions[key] exists AND not expired AND not blocked:
    if assigned backend is alive:
      return same backend (STICKY)
    else:
      reassign via hash ring → log path change → return new backend
  else:
    assign via hash ring → create new session → return backend
```

**Session Expiry**: Lazy cleanup — sessions are checked on access rather than via a background timer.
- ✅ Simpler, no background threads
- ❌ Dead sessions accumulate until someone queries them or `cleanupExpired()` is called
- **Mitigation**: `cleanupExpired()` can be called periodically by the caller

**No Explicit Locking for Session Data**: As specified in the problem, a tuple is only handled by one server at a time, so the backend's local data doesn't need locking. The session table structure itself is protected by a mutex for concurrent access.

### 4.4 Rate Limiter (`rate_limiter.h/cpp`)

**Algorithm**: Sliding Window Queue

```
allowRequest(client_key):
  q = client_queues[client_key]
  now = current_time

  // Evict expired entries
  while q.front().timestamp < (now - window):
    q.pop_front()

  // Check limit
  if q.size() >= max_requests:
    q.push({now, "REJECTED"})  // Still record for history
    return false

  q.push({now, "ALLOWED"})
  return true
```

**Why sliding window (not fixed window)?**:
| Approach | Pros | Cons |
|----------|------|------|
| Fixed window | Simple counter | Burst at window boundary (2x allowed in 1-second span) |
| **Sliding window** | Fair, no edge bursts | Requires storing timestamps |
| Token bucket | Smooth rate | More complex state management |

**Dual Purpose**: The queue serves both as a rate limiter AND as a request history / audit trail for logging — eliminating the need for a separate history data structure.

**Tradeoff**:
- ✅ Exact sliding window — no burst-at-boundary problem
- ✅ Built-in history tracking
- ❌ Memory: stores one timestamp per request per client in the window
- ❌ With 100 req/60s limit and 10K clients: ~1M entries max = ~16MB — acceptable

### 4.5 Security Module (`security.h/cpp`)

**Defense Layers**:

| Layer | What it Catches | How |
|-------|----------------|-----|
| **Payload size** | Buffer overflow, oversized packets | Check `payload.size() > MAX_PAYLOAD_SIZE` |
| **IP validation** | Malformed IPs, non-IPs | Manual octet parsing (0-255, no leading zeros) |
| **Port validation** | Invalid ports | Range check 1-65535 |
| **Protocol whitelist** | Unknown protocols | Only TCP/UDP accepted |
| **Spoof detection** | IP spoofing from same subnet | Track unique IPs per /24 in time window |
| **Rate limiting** | DDoS, brute force | Sliding window per-client limit |

**Spoof Detection Algorithm**:
```
checkSpoofing(src_ip):
  subnet = extract /24 prefix (first 3 octets)
  tracker = subnet_trackers[subnet]
  evict entries older than SPOOF_DETECTION_WINDOW_SEC
  add current IP to tracker
  count unique IPs in tracker
  if count > SPOOF_DETECTION_THRESHOLD:
    flag as spoof
```

**Tradeoff**:
- ✅ Catches randomized source IPs from same subnet (common in spoofing)
- ❌ False positives in large NAT environments where many real clients share a /24
- **Tuning**: Adjust `SPOOF_DETECTION_THRESHOLD` based on expected legitimate traffic

### 4.6 Load Balancer (`load_balancer.h/cpp`)

**Dual LB Architecture**:
- **Primary**: Active from start, handles all requests, sends heartbeats
- **Standby**: Passive, monitors primary heartbeat, takes over on timeout

**Failover Detection**:
```
shouldTakeover(primary_last_heartbeat):
  elapsed = now - primary_last_heartbeat
  return elapsed > LB_FAILOVER_THRESHOLD_MS
```

**Shared State**: Both LBs hold references to the same `SessionManager`, `ConsistentHashRing`, `RateLimiter`, and `Security` instances. In production, this would be achieved via shared database or state synchronization protocol.

**Backend Health Management**:
- `takeBackendOffline(id)`: stops server → removes from hash ring → redistributes sessions
- `bringBackendOnline(id)`: starts server → adds to hash ring → new requests may route to it

### 4.7 Backend Server / Request Handler (`request_handler.h/cpp`)

**Local Data Store**: Each backend maintains `unordered_map<string, SessionData>` — per-tuple data stored locally. Since sticky sessions guarantee a tuple goes to one server at a time, **no locking is needed**.

**SessionData fields**: tuple key, request count, last request time, metadata.

### 4.8 Logger (`logger.h/cpp`)

**Design**: Singleton with dual output channels:
- `requests.log`: Every request with tuple, action, status, details
- `system.log`: Internal events (failover, server up/down, config changes)

**Thread Safety**: Each channel has its own mutex, so request logging doesn't block system logging.

**Toggle**: Enabled/disabled via `--logging on|off` CLI argument. When disabled, all log calls are no-ops (zero overhead).

---

## 5. Bottlenecks & Limitations

### 5.1 Current Bottlenecks

| Bottleneck | Impact | Where |
|------------|--------|-------|
| **Session table mutex** | Single lock on entire session map | `SessionManager::routeRequest()` |
| **Hash ring mutex** | Lock contention on lookup | `ConsistentHashRing::getServer()` |
| **Lazy expiry** | Dead sessions accumulate | Session table grows until cleanup |
| **Synchronous pipeline** | Each request blocks until complete | `LoadBalancer::handleRequest()` |
| **Logger file I/O** | Disk flush on every log entry | `Logger::logRequest()` |
| **Spoof tracker memory** | Grows with unique subnets seen | `Security::checkSpoofing()` |

### 5.2 Quantitative Limits

| Metric | Current Limit | Reason |
|--------|---------------|--------|
| Concurrent sessions | ~100K-1M | `unordered_map` memory (each entry ~200 bytes) |
| Throughput (single-threaded) | ~100K-500K req/s | Depends on session table hit rate |
| Backend servers | Configured via `NUM_BACKEND_SERVERS` | Memory for hash ring (150 vnodes × N servers) |
| Rate limiter memory | O(R × C) | R = requests in window, C = unique clients |

---

## 6. Scalability Analysis

### 6.1 What Scales Well

1. **Consistent hashing**: Adding/removing servers only redistributes ~1/N sessions
2. **Hash-based session lookup**: O(1) regardless of session count
3. **Per-server local storage**: No central database contention
4. **Configurable parameters**: All limits adjustable via `config.h`

### 6.2 What Doesn't Scale (Yet)

1. **Single-threaded processing**: All requests go through one `handleRequest()` call chain
2. **In-memory session table**: Limited by process memory; no persistence
3. **No horizontal LB scaling**: Only primary + standby (no LB cluster)
4. **Shared state between LBs**: Currently in-process references (not distributed)

### 6.3 How to Scale Further

| Approach | What it Solves | Complexity |
|----------|---------------|------------|
| **Thread pool for request handling** | Multi-core utilization | Medium — need lock-free session table or sharding |
| **Sharded session table** | Reduce mutex contention | Medium — partition by hash prefix |
| **Write-ahead logging** | Persist sessions across crashes | Medium — add WAL before session mutation |
| **Distributed state store (Redis)** | Share sessions across LB instances | High — network overhead for each lookup |
| **Time-wheel for expiry** | Efficient bulk expiry without scanning | Medium — maintain timer buckets |
| **Lock-free hash map** | Eliminate mutex on hot path | High — CAS-based concurrent map |
| **Connection pooling** | Reuse backend connections | Low — but not needed in simulation |

---

## 7. Tradeoffs & Justifications

### 7.1 Design Decision Matrix

| Decision | Alternative | Why we chose this | Tradeoff |
|----------|-------------|-------------------|----------|
| **`unordered_map` for sessions** | `std::map` (ordered) | O(1) vs O(log N) lookup | Unordered iteration; worse worst-case |
| **`std::map` for hash ring** | Array + binary search | `lower_bound()` gives natural ring semantics | Slightly more memory overhead |
| **FNV-1a hash** | MurmurHash, xxHash | Simple, no dependencies, good distribution | Not the fastest for huge datasets |
| **Lazy expiry** | Background timer thread | Simpler, no threads needed | Dead sessions linger |
| **Sliding window queue** | Token bucket, fixed window | Exact fairness, built-in history | More memory per client |
| **In-process simulation** | Real UDP sockets | Deterministic testing, no network issues | Not a real network test |
| **Single LB mutex** | Per-bucket locking | Simpler, correct | Lower throughput under contention |
| **Macro-based config** | Runtime config file parsing | Compile-time constants (faster), zero overhead | Requires recompilation to change |
| **Dual LB (active-passive)** | Active-active cluster | Simple HA, no split-brain risk | Standby wastes resources |
| **No external dependencies** | Boost, Cap'n Proto, etc. | Zero setup, maximal portability | Re-implement some utilities |

### 7.2 Why UDP (Protocol Analysis)

The problem asks about protocol choice. Analysis:

| Protocol | Latency | Reliability | Connection Overhead | Use Case |
|----------|---------|-------------|-------------------|----------|
| **TCP** | Higher (3-way handshake) | Guaranteed delivery | 1 connection per client | When delivery matters |
| **UDP** | Lowest | Best-effort | None | L4 routing, low-latency |
| **WebSocket** | TCP + upgrade overhead | Reliable, full-duplex | 1 persistent connection | Interactive, bidirectional |

**Verdict**: For a Layer-4 load balancer simulation:
- **UDP is ideal** — matches real L4 LB behavior (e.g., IPVS, F5 BIG-IP)
- Real production LBs operate at L4 with raw packets, not application-layer protocols
- However, the simulation uses in-process calls for determinism (no real sockets needed)

---

## 8. Security Architecture

### 8.1 Threat Model

| Threat | Attack Vector | Defense |
|--------|--------------|---------|
| **DDoS flooding** | Massive request volume | Rate limiter (sliding window per client) |
| **IP spoofing** | Random source IPs | Per-subnet unique IP tracking |
| **Malformed input** | Invalid IPs, ports, protocols | Input validation pipeline |
| **Buffer overflow** | Oversized payloads | Payload size enforcement |
| **Session hijacking** | Guess another client's tuple | 5-tuple has 2^80+ key space (infeasible to guess) |
| **State exhaustion** | Create many sessions | Session timeout + lazy cleanup |
| **Backend probing** | Discover backend topology | All routing internal to LB (backends not exposed) |

### 8.2 Security Ideas Beyond Current Implementation

1. **Request signing**: HMAC on 5-tuple + timestamp to prevent replay attacks
2. **TLS/DTLS**: Encrypt client–LB communication (for confidentiality)
3. **Geo-IP filtering**: Block traffic from known-bad regions
4. **Anomaly detection**: ML-based traffic pattern analysis (e.g., sudden 10x spike from one IP)
5. **Session token rotation**: Periodically reassign session tokens to limit window of exposure
6. **Backend health secrets**: Require shared secret for heartbeat validation
7. **Audit log integrity**: Write-once append-only logs with checksums

---

## 9. Bonus Features Implemented

| Bonus | Status | Implementation |
|-------|--------|----------------|
| ✅ Consistent hashing | Done | FNV-1a + virtual nodes + `std::map` ring |
| ✅ Session timeout | Done | Lazy expiry with configurable per-session override |
| ✅ Multi-path failover | Done | Backend failure → hash ring removal → session redistribution |
| ✅ Weighted path selection | Partial | Vnodes provide implicit weighting (more vnodes = more traffic) |
| ✅ Evasion-resistant session ID | Done | Spoof detection via /24 subnet tracking |
| ❌ Containerization | Not done | Could easily add Dockerfile (single binary, no deps) |

---

## 10. Scope of Improvement

### 10.1 Short-Term Improvements

1. **Real UDP sockets**: Replace in-process calls with actual `sendto()`/`recvfrom()` for real network testing
2. **Configuration file**: Replace `#define` macros with runtime YAML/JSON config parsing
3. **Prometheus metrics**: Export request counts, latency histograms, backend health status
4. **Time-wheel expiry**: O(1) per-tick expiry instead of O(N) lazy scan
5. **Weighted consistent hashing**: Allow different backend capacities

### 10.2 Medium-Term Improvements

1. **Thread pool**: Process requests in parallel with sharded session table
2. **Persistent sessions**: Save session table to disk on shutdown, reload on start
3. **Dynamic backend discovery**: Health check endpoint to auto-discover backends
4. **gRPC API**: For programmatic session management (block, reassign, query)
5. **A/B testing support**: Route percentage of traffic to canary backends

### 10.3 Long-Term Vision

1. **Distributed session store**: Redis/etcd-backed session table for horizontal LB scaling
2. **eBPF integration**: XDP-based packet processing for kernel-level routing (millions of pps)
3. **Service mesh integration**: Sidecar proxy mode compatible with Istio/Envoy
4. **Machine learning**: Predictive load balancing based on traffic pattern history
5. **DPDK integration**: Kernel-bypass for <1μs routing latency

---

## 11. Testing Strategy

### 11.1 Test Categories (14 total)

| # | Test | What it Validates |
|---|------|-------------------|
| 1 | Session Stickiness | Same 5-tuple → same backend, every time |
| 2 | Consistent Hash Distribution | All servers get between 2-30% of traffic |
| 3 | Rate Limiting | First N allowed, N+1 rejected; different clients independent |
| 4 | Session Expiry | Sessions disappear after timeout; cleanup works |
| 5 | Backend Failover | Backend dies → sessions reassigned → path history updated |
| 6 | Input Validation | Invalid IPs, ports, protocols all rejected |
| 7 | Spoof Detection | >50 unique IPs from same /24 → flagged |
| 8 | LB Failover | Primary deactivated → standby takes over via heartbeat |
| 9 | Path History | Initial assignment → force reassignment → 2 entries |
| 10 | Edge Cases | Empty ring, single server, duplicate add, max ports, serialization |
| 11 | Session Blocking | Block → reject; unblock → resume; non-existent → false |
| 12 | Full Pipeline | Valid, invalid, oversized all flow through complete pipeline |
| 13 | Custom Expiry | Per-session timeout override respected |
| 14 | Multiple Failures | All-but-one backend down → still works; all down → graceful error |

### 11.2 Test Design Philosophy

- **Assertions over manual inspection**: Every test verifies its own outcome
- **Independent tests**: Each test creates its own engine instance (no shared state)
- **Edge case focus**: Boundary values, empty inputs, negative scenarios
- **Fast execution**: All tests complete in ~2 seconds (including 2 sleep-based expiry tests)

---

## 12. Data Flow Summary

```
Client Request
     │
     ▼
┌─ LB Active? ─── NO ──→ ERROR: LB not active
│    YES
│    ▼
├─ Valid IP/Port/Proto? ─── NO ──→ ERROR: Invalid input
│    YES
│    ▼
├─ Payload size OK? ─── NO ──→ ERROR: Payload too large
│    YES
│    ▼
├─ Spoof check OK? ─── NO ──→ ERROR: Spoofing detected
│    YES
│    ▼
├─ Rate limit OK? ─── NO ──→ ERROR: Rate limit exceeded
│    YES
│    ▼
├─ Session exists? ─── YES ──→ Expired? ─── YES ──→ Create new session
│    │                              │                          │
│    NO                          NOT EXPIRED                   │
│    │                              │                          │
│    ▼                              ▼                          │
│  Assign via                 Backend alive?                   │
│  hash ring ◄──────────── NO ──┤                              │
│    │                          │                              │
│    │                         YES                             │
│    │                          │                              │
│    ▼                          ▼                              │
│  Create session         Return SAME backend ◄────────────────┘
│    │                     (STICKY)
│    ▼
│  Forward to backend
│    │
│    ▼
│  Backend online? ─── NO ──→ Failover: reassign + retry
│    YES
│    ▼
│  Process request
│    │
│    ▼
│  Return SUCCESS + response
└─────────────────────────────
```

---

## 13. Performance Characteristics

| Operation | Average Case | Worst Case | Notes |
|-----------|-------------|------------|-------|
| Session lookup | O(1) | O(N) hash collision | `unordered_map` amortized O(1) |
| Hash ring lookup | O(log V×S) | O(log V×S) | `std::map::lower_bound` |
| Rate limit check | O(W) | O(W) | W = expired entries to pop |
| Security validation | O(1) | O(1) | Fixed-length IP parsing |
| Spoof detection | O(K) | O(K) | K = entries in current window |
| Session cleanup | O(N) | O(N) | Full scan of session table |
| Backend failover | O(M) | O(M) | M = sessions on failed server |

---

*Last updated: 2026-03-30*
*Author: Session Affinity Engine Team*
