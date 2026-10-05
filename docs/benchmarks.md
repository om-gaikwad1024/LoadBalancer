# Benchmarks

Real measurements only (plan X); no estimates. Each figure comes from a run of the scripts
in `tools/scripts/`, whose raw output (k6 summaries, per-request CSV, proxy metrics dumps,
event logs, samples) is kept under `build\gate\<name>-<timestamp>\`. Medians are over 3 runs
after a warm-up, unless a row says otherwise.

## Environment
| Item | Value |
|---|---|
| CPU | Intel Core i5-1135G7 @ 2.40 GHz, 4 cores / 8 threads |
| RAM | 7.7 GB |
| OS | Windows 11 Home Single Language 10.0.26200 |
| Build | `release` preset (MSVC 14.51, RelWithDebInfo), proxy run headless as `lb_console.exe` |
| Proxy config | `config/bench.json`: 8 IOCP workers (`auto`), 3 backends, round robin, 30 s metrics window |
| Load generator | k6 v2.2.0, **constant-arrival-rate** executor only (open model, no coordinated omission) |
| Topology | k6, the proxy and 3 mock backends **all on the same laptop**: they compete for the same 8 logical CPUs |
| Date | 2026-10-04 |

**Measurement limits on this machine**
- **k6 can't resolve sub-millisecond latency here.** For the zero-delay mock on loopback, k6 reports most requests as 0 ms (p50 = 0). Its latencies are only used where the backend adds a fixed 5 ms delay. The proxy's own histograms (microsecond resolution, ≤1.6% bucket error) give the sub-millisecond numbers.
- **The load generator, not the proxy, sets the throughput ceiling.** k6 drops iterations at 4,000–6,000 req/s even when it talks to a single mock directly with no proxy involved, and where that happens varies from run to run (see the mock baseline).
- **Mock delays need a 1 ms timer.** The mock requests 1 ms resolution (`timeBeginPeriod(1)`), so a 5 ms delay measures about 5.3 ms.

## Phase 1 gate (step 1.11)

### Mock backend baseline (measured before use as the direct-to-backend reference)
Pass rule: no failed requests, ≤0.1% dropped iterations, achieved rate ≥98% of target, p99 ≤ 10 ms. One 15 s run per rate, one mock instance, no proxy.

| Search run | Highest passing rate | First failing rate (reason) |
|---|---|---|
| bench-20261004-210717 (the run reported below) | **3,500 req/s** | 4,000 req/s (81 dropped iterations; p99 0.52 ms, 0 errors) |
| earlier run, same day | 6,000 req/s | not reached |

The mock never failed a request and its p99 stayed under 1 ms. Each failure was k6 dropping iterations, so the measured mock "limit" is really the load generator's limit on this machine. The overhead tests below run at 1,000 req/s, well under every measured limit.

### Throughput
Highest constant-arrival rate with p99 ≤ **10 ms**, no failed requests, and ≤0.1% dropped iterations. A coarse search (15 s per rate: 1,000 → 6,000) found 5,000 req/s passing and 6,000 req/s failing (5,014 dropped iterations, 0 errors). 5,000 req/s was then run 3 × 30 s:

| Run | Achieved | k6 p99 | k6 max | Dropped | Proxy total p50 / p95 / p99 / max | Proxy backend-only p50 / p99 |
|---|---|---|---|---|---|---|
| 1 | 4,934.9/s | 3.01 ms | 430.3 ms | 1,952 | 0.133 / 0.199 / 0.395 / 18.2 ms | 0.113 / 0.335 ms |
| 2 | 5,000.0/s | 0.64 ms | 22.6 ms | 0 | 0.114 / 0.165 / 0.245 / 8.2 ms | 0.098 / 0.197 ms |
| 3 | 4,999.8/s | 0.40 ms | 17.3 ms | 0 | 0.110 / 0.141 / 0.169 / 14.8 ms | 0.095 / 0.141 ms |
| **Median** | **4,999.8/s** | **0.64 ms** | **22.6 ms** | | **0.114 / 0.165 / 0.245 / 14.8 ms** | **0.098 / 0.197 ms** |

0 failed requests in every run. In run 1, k6 saw a 430 ms stall and dropped 1,952 iterations, while the proxy's own max over the same window was 18 ms. The stall happened outside the proxy (in the load generator or OS scheduling on the shared CPUs). **The proxy sustains at least 5,000 req/s at sub-millisecond p99; this setup can't drive it harder.**

### Proxy overhead and tail latency (backends add a fixed 5 ms)
Same rate, 1,000 req/s, 3 × 30 s each: directly to one mock, then through the proxy to 3 mocks.

| | p50 | p95 | p99 | max |
|---|---|---|---|---|
| k6 → mock directly (median) | 5.359 ms | 6.294 ms | 6.571 ms | 9.44 ms |
| k6 → proxy → mock (median) | 5.325 ms | 6.310 ms | 6.488 ms | 10.25 ms |
| **Overhead seen by k6** | **−0.034 ms** | +0.016 ms | **−0.083 ms** | |
| Proxy's own measurement of the same runs | 5.375 ms | 6.335 ms | 6.527 ms | 9.74 ms |

- **Proxy overhead is below what k6 can resolve at this latency:** the medians differ by less than run-to-run noise. The proxy's own histograms isolate it: at 5,000 req/s, total p50 0.114 ms against backend-only 0.098 ms, so about **16 µs at p50** and about **48 µs at p99** (0.245 against 0.197 ms). Percentiles of two series don't subtract exactly; this is an indication, not an exact figure.
- **The proxy's percentiles agree with k6's:** p50 +0.9%, p95 +0.4%, p99 +0.6%, max −5%. That's within the histogram's ≤1.6% bucket error (it reports bucket upper bounds) plus the client-side time k6 adds.

### Connection reuse
Over the whole throughput run: 872,603 requests through **167 new backend connections**, i.e. **5,225 requests per backend connection**.

### Failover: backend kill under load
`kill_test.ps1`: 500 req/s for 40 s through `config/killtest.json` (probes every 500 ms, timeout 250 ms, N = 3, M = 2, so the detection window is 1,750 ms). Backend `web-2` is a real process, hard-killed (TerminateProcess) at +10 s and restarted at +25 s. Kill and restart times come from the script, exclusion and re-inclusion times from the event log, and failures from k6's per-request CSV, all on one clock.

| Run | Failover (kill → excluded) | Failed before kill | Failed kill → exclusion | **Failed after exclusion** | Re-included after restart |
|---|---|---|---|---|---|
| kill-20261004-211751 | 1,529 ms | 0 | 253 | **0** | 824 ms |
| kill-20261004-211844 | 1,446 ms | 0 | 239 | **0** | 627 ms |
| kill-20261004-211931 | 1,476 ms | 0 | 244 | **0** | 699 ms |
| **Median** | **1,476 ms** | 0 | **244** | **0** | **699 ms** |

- **Errors during failover are without retries.** Phase 1 has no retries, so the requests that round robin sends to the dead backend before exclusion fail with 502: about a third of 500 req/s over about 1.5 s. Each of them is also in the proxy's event log as `backend_error`, and the count matches k6's exactly (253 vs 253 in the first run). Retries come in phase 3.
- **Exclusion reason:** "connect: 10061 (actively refused)". Refusals are detected immediately because of `pool.fail_fast_connect` and fail-fast probes.

### Stability: soak
`soak.ps1`: **30 minutes** through `config/soak.json`. Load was 1,000 req/s keep-alive plus 50 req/s with `Connection: close` (socket churn), both constant-arrival-rate. Every 2 minutes one backend's health endpoint failed for 3 s, marking it down and up (14 times). Pooled connections had a 2 s idle timeout. The proxy process was sampled every 10 s. Baseline = median of samples in minutes 2–3; end = median of the last 3 samples.

| | Baseline | End | Change | Trend after warm-up |
|---|---|---|---|---|
| Handle count | 223 | 223 | **0** | −0.2 / hour |
| Private bytes | 8.52 MB | 8.65 MB | **+0.13 MB** | +0.16 MB / hour |
| Threads | 15 | 15 | **0** | |

1,890,001 requests, **0 failed, 0 dropped iterations**, k6 p99 0.59 ms; 90,050 client connections and 771 backend connections opened; 14 marked down and 14 marked up; 0 event-log entries dropped. Over the whole run, handles stayed between 222 and 226 after warm-up, and private bytes between 8.63 and 8.66 MB.

## Phase 2: load-balancing strategies (step 2.2, plan IV.7)
Measured by the integration tests in `tests/integration/proxy_balancing_test.cpp`, release build,
3 consecutive runs (`--gtest_repeat=3`). In-process proxy (4 workers) and mock backends, with 8
blocking keep-alive test clients sending back to back for 1.5 s (1 s for weighted round robin).
These are closed-loop clients, so request counts depend on latency; the shares are what matter.

**Slow backend (20 ms of added latency) next to a fast one (no added latency):** share of
requests sent to the slow backend.

| Strategy | Run 1 | Run 2 | Run 3 | Requests per run |
|---|---|---|---|---|
| `round_robin` | 50.06% | 50.06% | 50.06% | 825–889 (the slow backend paces every client) |
| `least_connections` | 1.24% | 1.35% | 1.35% | 15,440–18,058 |
| `least_response_time` | **0.02%** (4) | **0.02%** (4) | **0.02%** (4) | 18,400–18,446 |

Under least response time, the slow backend got only its 4 cold-start requests (before its first
sample came back). The averages the engine reported at the end were 22.5–31.1 ms for the slow
backend and 0.61–0.76 ms for the fast one. A backend that resets every connection got 2 requests
in each run, against 4,318–4,363 good responses from the other: a failure counts as the full
`backend_response_ms`.

**Weighted round robin, weights 3:1:** the weight-3 backend got **75.00%** of 12,443–12,550
requests in each run.

## Phase 2 gate: reloads and drains under load (step 2.8)
Plan I gate and X "Drain and reload safety": *reload and drain tests pass under load with zero
dropped requests.* `tools\scripts\live_ops_test.ps1` runs real processes: `lb_console` (release),
four mock backends (each adding 5 ms), and k6 at a constant arrival rate for 60 s. Every 6 s it
changes the running proxy the way an operator would, and waits for each change to take effect:

1. add `web-4` (config file edit, picked up by the file watcher)
2. `web-1` weight 3 and strategy `least_connections` (file edit)
3. drain `web-2` (operator command, as the dashboard's Drain button does), until it is drained
4. remove `web-2` from the config (file edit)
5. drain `web-3` from the config (`"drain": "start"`), until it is drained
6. save a half-written config (must be rejected and change nothing)
7. `web-3` back in service (`"drain": "cancel"`)
8. back to round robin, `"keep"`

A run passes only if all of these hold:
- k6 saw zero failed requests and zero dropped iterations, and the proxy sent zero error responses.
- Exactly 6 reloads were accepted and 1 rejected.
- Both drains completed, with no timeout and no aborted request.
- Each drained backend received no request after it was drained, counted by the mock itself.

**2,000 req/s, 3 runs:**

| Run | Requests | Failed | Dropped | k6 p50 / p99 / max | Drains: duration (in flight at start) |
|---|---|---|---|---|---|
| 1 | 119,999 | **0** | **0** | 5.56 / 6.97 / 14.1 ms | 30 ms (3), 46 ms (4) |
| 2 | 120,001 | **0** | **0** | 5.57 / 6.99 / 13.4 ms | 61 ms (3), 107 ms (4) |
| 3 | 120,001 | **0** | **0** | 5.55 / 6.97 / 11.2 ms | 45 ms (2), 76 ms (4) |

**4,000 req/s, 3 runs:**

| Run | Requests | Failed | Dropped | k6 p50 / p99 / max | Drains: duration (in flight at start) |
|---|---|---|---|---|---|
| 1 | 239,999 | **0** | **0** | 5.61 / 6.84 / 15.6 ms | 29 ms (5), 106 ms (7) |
| 2 | 240,000 | **0** | **0** | 5.62 / 6.81 / 23.3 ms | 14 ms (5), 76 ms (7) |
| 3 | 240,000 | **0** | **0** | 5.62 / 6.84 / 16.8 ms | 45 ms (5), 91 ms (7) |
| **Median** | **240,000** | **0** | **0** | **5.62 / 6.84 / 16.8 ms** | |

Every check passed in all six runs. Each drain started with requests in flight (2–7) and completed in
14–107 ms. Completion is checked every `maintenance.interval_ms`, 100 ms in `config/liveops.json`.
The backend latency is the mocks' added 5 ms; p99 stays under 7 ms through every reload and drain.

**5,000 req/s (6 runs):** 0 failed requests in every run. Three runs had no dropped iterations;
the other three had 22, 34 and 63. All checks other than the drop count passed in every run. In
the run with 55 drops (taken with `-Timeline` to locate them), they all fell in one burst at
16.95 s. That was 4.9 s after the nearest change, a reload that had already taken effect, and 1 s
before the next one (the drain). In that second k6 started 4,947 requests instead of 5,000. This is
the same load-generator limit the phase-1 throughput runs hit at 5,000 req/s with no changes at all
(1,952 dropped in one run). **Gate result: passed at 4,000 req/s, zero failed and zero dropped
requests; at 5,000 req/s, zero failed requests.**

## How to reproduce
Build first (`tools\build.cmd release all`), then from the repo root:
```
powershell -ExecutionPolicy Bypass -File tools\scripts\bench.ps1
powershell -ExecutionPolicy Bypass -File tools\scripts\kill_test.ps1
powershell -ExecutionPolicy Bypass -File tools\scripts\soak.ps1 -Minutes 30
```
Each script prints its results and writes `results.json` to its run directory. `kill_test.ps1` and `soak.ps1` exit with a non-zero code when the gate criterion fails.

Live operations gate (step 2.8), 3 runs each:
```
powershell -ExecutionPolicy Bypass -File tools\scripts\live_ops_test.ps1 -Rate 2000
powershell -ExecutionPolicy Bypass -File tools\scripts\live_ops_test.ps1 -Rate 4000
```
Add `-Timeline` to also record k6's per-request CSV and report when any dropped or failed iteration happened relative to the steps.

Strategy shares (step 2.2): `build\release\tests\lb_integration_tests.exe --gtest_filter=ProxyTest.*Weighted*:ProxyTest.*IpHash*:ProxyTest.*LeastResponse* --gtest_repeat=3`; each test prints a `[ lb ]` line with its counts.
