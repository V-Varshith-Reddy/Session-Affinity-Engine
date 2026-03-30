# Session Affinity Engine — Presentation (5 min)

---

## Slide 1: Problem & Goal

**Building**: A Layer-4 load balancer simulation in C++17

**Core requirements**:
- Route requests based on 5-tuple `(src_ip, src_port, dst_ip, dst_port, protocol)`
- Maintain **sticky sessions** — same tuple → same backend, always
- Handle **backend failures** and **LB failures** transparently
- Enforce **rate limiting** and **security** checks
- Full **audit logging** for every request

**Tech**: C++17, STL only, zero external dependencies

---

## Slide 2: System Architecture

![Session Affinity Engine — Architecture Block Diagram](architecture_block_diagram.png)

**Key components**:
- **Primary + Standby LB** with heartbeat-based failover (3s timeout)
- **Security Module** → **Rate Limiter** → **Session Manager** → **Hash Ring** pipeline
- **8 configurable backend servers**, each with lock-free local storage
- Dual log channels: `requests.log` + `system.log`

---

## Slide 3: Request Flow

![Request Flow — Full Pipeline Sequence](request_flow_diagram.png)

**6-stage fail-fast pipeline**: LB Active Check → Security Validation → Rate Limiting → Session Routing → Backend Forwarding → Response

---

## Slide 4: Failover — Backend & Load Balancer

![Failover Diagrams](failover_diagram.png)

**Backend failure**: Remove 150 vnodes from hash ring → ~12.5% keys redistribute → sessions reassigned automatically

**LB failure**: Standby detects 3s heartbeat timeout → `shouldTakeover() = TRUE` → becomes ACTIVE

---

## Slide 5: Security Pipeline

![Security Validation Pipeline](security_flowchart.png)

6 layers of defense: payload size → IP validation → port range → protocol whitelist → spoof detection → rate limiting

---

## Slide 6: Key Design Decisions

| Decision | Why | Tradeoff |
|----------|-----|----------|
| `unordered_map` for sessions | O(1) lookup | Unordered; O(N) worst-case |
| Consistent hashing (150 vnodes) | Only ~12.5% keys move on failure | Imperfect distribution |
| Sliding window rate limiter | Exact fairness + built-in history | More memory than counter |
| Lazy session expiry | No background threads | Dead sessions linger |
| Active-passive LB | No split-brain risk | Standby wastes resources |

---

## Slide 7: Test Results & Demo

**14/14 tests pass** ✅

| Test Category | Status |
|---------------|--------|
| Session Stickiness | ✅ |
| Consistent Hash Distribution | ✅ |
| Rate Limiting | ✅ |
| Session Expiry | ✅ |
| Backend Failover | ✅ |
| Input Validation | ✅ |
| Spoof Detection | ✅ |
| LB Failover | ✅ |
| Path History | ✅ |
| Edge Cases | ✅ |
| Session Blocking | ✅ |
| Full Pipeline Integration | ✅ |
| Custom Expiry | ✅ |
| Multiple Backend Failures | ✅ |

```bash
make all    # Build (< 5s)
make test   # Run all 14 tests
make run    # Run 10-scenario simulation
```

---

*Presentation duration: ~5 minutes*
