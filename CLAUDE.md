# LoadBalancer — working rules

HTTP/1.1 reverse proxy / load balancer, C++20 + MFC, Winsock IOCP data plane. Reference: `docs/plan.md` (section numbers below point into it).

**Current step:** 1.4 done — next: 1.5 (waiting for review/commit)

## Rules
- Read only the `docs/plan.md` sections listed for the current step. Never read the whole plan again, and never re-read the PDF.
- Don't open `src/app/` unless the task is UI/dashboard. Don't open OpenSSL/TLS code unless the task is TLS.
- Follow plan VIII (shortcuts ruled out) and V (threading, data ownership): no thread per connection, no blocking calls on IOCP workers, no MFC sockets, worker threads never touch UI objects, `std::chrono::steady_clock` (`lb::Clock`) for all timing, no one global lock.
- `src/engine/` never includes MFC/ATL headers and uses only standard containers (II.6). Enforced by `cmake/EngineNoMfc.cmake` and the force-included `core/no_mfc_guard.h`. MFC types (CString, CArray…) only in `src/app/`.
- Every timeout, limit, threshold and size lives in config, never hard-coded in the engine. **All config fields are required**; validation rejects missing ones (`worker_threads` may be `"auto"`).
- A step adding a config field documents it in `docs/config-reference.md` in the same step.
- The mock backend keeps its **own** small HTTP parser (never links the engine parser), thread per connection, blocking Winsock.
- k6 scripts use the constant-arrival-rate executor only.
- `docs/benchmarks.md` holds real measurements only, never estimates. Measure the mock backend's own limit before using it as a baseline.
- A step is done only when its plan "Done means" is covered by a passing test. End each step with exact verify-it-yourself commands and a one-line commit message, then stop for review/commit. Phases 3–4 only when the user says so.
- Build and test **only** via `tools\build.cmd [debug|release] [configure|build|test|all] [ctest args]` (sets vcpkg env + vcvars64, Ninja presets). If a tool can't be found, show the exact command and output instead of guessing.

## Folder map
| Path | What |
|---|---|
| `src/engine/` | `lb_engine` static lib: core, config, http, net, proxy, backend, balance, health, metrics, log |
| `src/app/` | `LoadBalancer.exe` MFC dialog app (shell, dashboard, admin) |
| `tests/unit/`, `tests/integration/` | `lb_unit_tests`, `lb_integration_tests` (GoogleTest, labels `unit`/`integration`) |
| `tests/corpus/`, `tests/fuzz/` | escaped reject corpus `<status>_<name>.txt`; parser fuzz target (`tools\build.cmd fuzz all [seconds]`) |
| `tools/lb_console/` | `lb_console.exe --config <file>`: headless engine host for load/soak runs |
| `tools/mock_backend/` | `mock_backend_lib` + `mock_backend.exe` (plan IX); switches and `/__mock/*` control in its README |
| `tests/integration/test_*.h` | test HTTP client (own reader) and child-process helper for kill tests |
| `tools/k6/`, `tools/scripts/` | load scripts; soak/kill/drain scripts (from 1.11) |
| `config/` | complete example configs; `lb.example.json` must stay valid (unit test loads it) |
| `docs/` | `plan.md`, original PDF, `benchmarks.md`, `config-reference.md` |
| `cmake/`, `CMakePresets.json`, `vcpkg.json` | build setup (presets `debug`, `release`, `asan` = all tests under ASan, `fuzz`; triplet x64-windows) |

## Checklist
### Phase 1 — Core proxy
- [x] 1.0 Scaffold: CMake + vcpkg, engine lib, empty MFC dialog, tests, mock skeleton, plan → `docs/plan.md`
- [x] 1.1 Config load + validation → immutable snapshot in `std::atomic<std::shared_ptr>` (II.4, II.7, IV.14 load only)
- [x] 1.2 HTTP/1.1 parser: limits, chunked, strict framing / smuggling defense (IV.3, VII)
- [x] 1.3 Mock backend with all fault switches (IX)
- [x] 1.4 IOCP listener, worker pool, per-request state machine → single backend (II.2, IV.1, IV.18)
- [ ] 1.5 Backend registry + connection pool (IV.4, IV.5)
- [ ] 1.6 Forwarding headers, X-Request-Id, all timeouts (IV.6, VI)
- [ ] 1.7 Round robin + least connections (IV.7)
- [ ] 1.8 Active health checks with hysteresis (IV.10)
- [ ] 1.9 Per-thread latency histograms + JSON-lines event log writer (IV.15, IV.16, V)
- [ ] 1.10 MFC dashboard, phase 1 (IV.17)
- [ ] 1.11 Gate: k6 benchmark, backend-kill test, soak with no handle/memory growth (I, IX, X)
### Phase 2 — Smart routing and live operations
- [ ] 2.1 Hot reload: watcher + debounce, validate-then-swap, content hash (IV.14)
- [ ] 2.2 Weighted RR, least response time (EWMA), IP hash (IV.7)
- [ ] 2.3 Content routing: path, header, cookie (IV.8)
- [ ] 2.4 Sticky sessions per group, after routing (IV.9, III)
- [ ] 2.5 Passive health checks (IV.10)
- [ ] 2.6 Graceful drain (IV.12)
- [ ] 2.7 Dashboard phase 2: GDI graphs, log viewer, admin via validation path (IV.17)
- [ ] 2.8 Gate: reload + drain under load, zero dropped requests
### Phase 3 — Extension (only on request)
- [ ] 3.1 Body buffering + retries with budget (IV.11)
- [ ] 3.2 Circuit breaker (IV.11)
- [ ] 3.3 Rate limiter + per-IP connection caps (IV.13)
- [ ] 3.4 Gate: zero requests to a backend while its circuit is open
### Phase 4 — Extension (only on request)
- [ ] 4.1 OpenSSL over IOCP via memory BIOs (II.3, IV.2)
- [ ] 4.2 Cert validation, reload rejection, hot rotation, X-Forwarded-Proto (IV.2, VII)
- [ ] 4.3 Gate: HTTPS via curl/browser, plain backend hop, s_client TLS 1.2/1.3
### Final
- [ ] README + docs from XI
