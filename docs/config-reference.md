# Configuration reference

Every field is **required**. The loader rejects a config that has any missing, unknown, mistyped
or out-of-range field, or any duplicate JSON key, and reports **all** errors with their JSON-pointer
path (for example `/groups/0/backends/1/port: must be between 1 and 65535`). Nothing is applied
unless the whole document is valid (plan IV.14). Every timeout, limit, threshold and size lives
here; none are hard-coded in the engine.

Format rules:
- Standard JSON (RFC 8259): no comments and no trailing commas. A UTF-8 byte-order mark is accepted.
- Integers must be JSON integers: `8080` is valid, while `8080.0`, `"8080"` and `true` are rejected.
- Durations are integers in milliseconds, and sizes are integers in bytes, unless a field says otherwise.
- Addresses are IPv4 literals such as `127.0.0.1`. Hostnames and IPv6 are not supported yet.
- Names (group names and backend ids) are 1–64 characters from `[A-Za-z0-9._-]`.

Example: [`config/lb.example.json`](../config/lb.example.json).

## Hot reload (plan IV.14)
A running proxy reloads its config file when the file changes (see [`config_reload`](#config_reload-hot-reload-plan-iv14)),
or on request (`reload` on `lb_console`'s stdin). The new document is parsed and fully validated
first; if anything is wrong, the reload is rejected as a whole, logged as `config_reload_rejected`
with every error, and the proxy keeps running on the previous config. A valid one is swapped in
atomically: requests already in flight finish on the config and backends they started with, and
every new request sees the complete new config. Nothing is ever half-applied.

**When a change takes effect:**

| Applies | Fields |
|---|---|
| Next request | `groups` (backends added or removed, weights, strategy, `host_header`, `sticky`, `passive_health`), `routing`, `trusted_proxies`, `balancing`, `timeouts` (per request; `client_header_ms` for a connection's first request counts from the accept), `pool.wait_timeout_ms`, `pool.fail_fast_connect`, `limits.max_client_connections` |
| At once, on each pool | `pool.max_connections_per_backend`, `pool.max_idle_per_backend`, `pool.max_waiters_per_backend`, `pool.idle_timeout_ms`. Idle connections over a lower `max_idle` are closed; a lower cap is reached as busy connections are released (they are never cut) |
| At once | `groups[].health`: health checks restart with the new settings (only when a backend or a health setting changed). A backend's current health state is kept |
| New client connections | `limits` (parser limits) and `buffers` |
| **Restart only** | `listen.*`, `workers.threads`, `maintenance.interval_ms`, `metrics.*`, `event_log.*`, `dashboard.*`, `config_reload.*`, `sticky_table.*`. A reload that changes any of these is **rejected**, naming each field |

Backends are matched by `id`. A backend whose id, address, port and group are unchanged keeps all of
its live state: health, draining, counters, pooled connections and latency history. A reload
never un-drains a backend (plan IV.12). A backend whose address, port or group changed counts as
removed and added; it starts fresh but stays draining if it was. A removed backend gets no new
requests; requests already using it finish, and its connections close as they are released.

## `listen`: client-facing listener (plan IV.1)
| Field | Type | Valid range | Meaning |
|---|---|---|---|
| `listen.address` | string | IPv4 literal; `0.0.0.0` = all interfaces | Address the HTTP listener binds to |
| `listen.port` | integer | 0–65535 | TCP port. `0` lets the OS pick a free port (used by tests) |
| `listen.backlog` | integer | 1–65535 | `listen()` backlog: connections the OS queues before the proxy accepts them |
| `listen.pending_accepts` | integer | 1–1024 | `AcceptEx` calls kept outstanding on the IOCP. Higher values absorb connection bursts |

## `workers`: IOCP worker pool (plan II.2, V)
| Field | Type | Valid range | Meaning |
|---|---|---|---|
| `workers.threads` | integer or `"auto"` | 1–256, or exactly `"auto"` | Fixed number of IOCP worker threads created at startup. `"auto"` = one per logical processor |

## `limits`: HTTP parser input limits (plan IV.3, VII)
Requests over a limit are rejected before any routing. A backend response over its limits is
treated as a bad response (502) and that backend connection is never reused.

| Field | Type | Valid range | Meaning |
|---|---|---|---|
| `limits.max_request_line_bytes` | integer | 64–1048576 | Longest request line (method, target, version), excluding CRLF. Over it: **400** |
| `limits.max_request_header_bytes` | integer | 256–1048576 | Total size of request header field lines including their CRLFs, excluding the request line. Over it: **431**. Also caps the chunked-body trailer section |
| `limits.max_request_header_count` | integer | 1–10000 | Most header fields in one request. Over it: **431**. Also caps trailer fields |
| `limits.max_response_header_bytes` | integer | 256–1048576 | Largest whole response head (status line, header fields, final empty line). Over it: **502** |
| `limits.max_response_header_count` | integer | 1–10000 | Most header fields in one response. Over it: **502** |
| `limits.max_chunk_line_bytes` | integer | 16–65536 | Longest chunk-size line (hex size plus chunk extensions), excluding CRLF, in either direction. Over it: 400 (request) or 502 (response) |
| `limits.max_client_connections` | integer | 1–1000000 | Global cap on open client connections (plan IV.1). A connection over the cap is accepted, answered with **503**, closed and counted as rejected. Never silently dropped |

## `buffers`: per-connection receive buffers
Each connection reads at most this many bytes per receive. A slow peer causes back-pressure,
because the proxy never reads further ahead than one buffer per direction.

| Field | Type | Valid range | Meaning |
|---|---|---|---|
| `buffers.client_read_bytes` | integer | 1024–1048576 | Receive size on the client connection |
| `buffers.backend_read_bytes` | integer | 1024–1048576 | Receive size on the backend connection |

## `pool`: backend connection pool (plan IV.5)
Each backend has its own pool and its own lock. A connection goes back to the pool only after its
response was read completely with clean framing, the backend allowed keep-alive, and the backend
is still healthy and not draining. A pooled connection the backend has closed is detected when
it's taken and discarded. If a reused connection still fails before any response byte arrives, a
bodiless GET/HEAD/OPTIONS request is sent again once on a fresh connection; any other request
gets 502 (plan VI). When a backend becomes unhealthy or starts draining, its idle connections
are closed at once.

| Field | Type | Valid range | Meaning |
|---|---|---|---|
| `pool.max_connections_per_backend` | integer | 1–65535 | Cap on connections to one backend, counting connecting, in use and idle |
| `pool.max_idle_per_backend` | integer | 0–65535, ≤ `max_connections_per_backend` | Idle keep-alive connections kept for reuse. `0` disables reuse (one connection per request) |
| `pool.idle_timeout_ms` | integer | 1–3600000 | An idle pooled connection older than this is closed by the maintenance thread. Keep it below the backends' own keep-alive timeout |
| `pool.max_waiters_per_backend` | integer | 0–1000000 | When a backend is at its cap, up to this many requests wait for a connection. Beyond it: **503** at once |
| `pool.wait_timeout_ms` | integer | 1–600000 | A waiting request that gets no connection within this time: **503** |
| `pool.fail_fast_connect` | boolean | `true` / `false` | `true` (recommended): a refused backend connect fails at once, with a **502** about 2 ms after a backend died. `false` keeps Windows' default of retrying the SYN after a refusal, which delays the 502 by about 2 s (measured: 2,039 ms). The cost of `true` is that a SYN lost on the network also fails at once instead of being retried; that's rare on a LAN, and `backend_connect_ms` still bounds every connect. Health probes always fail fast |

## `maintenance` (plan V)
| Field | Type | Valid range | Meaning |
|---|---|---|---|
| `maintenance.interval_ms` | integer | 10–60000 | How often the maintenance thread runs: it closes idle pooled connections past `pool.idle_timeout_ms` (later also expired sticky-session and rate-limit entries) |

## `metrics`: latency histograms (plan IV.15)
Every IOCP worker records into its own log-bucketed histograms, with at most 1.6% relative error
and fixed memory; reading merges them. The proxy as a whole and each backend get two series:
**total** (first request byte in to last response byte out) and **backend** (backend connection
ready to response fully received). Each series is reported as p50/p95/p99/max/mean, both since
start and over a live window, together with requests per second, the error rate, and counts per
status class (1xx–5xx, plus aborted).

| Field | Type | Valid range | Meaning |
|---|---|---|---|
| `metrics.slice_ms` | integer | 100–60000 | Length of one time slice of the live window |
| `metrics.window_slices` | integer | 1–120 | Slices in the live window. Window = `slice_ms × window_slices` (default 10 s) |
| `metrics.max_backend_series` | integer | 1–4096, ≥ the number of backends | Per-backend latency series the engine can hold. Each backend id seen since start uses one (a removed id keeps its history, and gets it back if it returns), and each IOCP worker reserves this many series slots. A reload that would need more is rejected; restart to reset it |

## `event_log`: audit trail (plan IV.16)
One JSON object per line. A background thread does the writing (workers only queue), and files
rotate by size. Every entry has `ts` (UTC wall clock, for people and for correlating with k6),
`mono_ms` (monotonic ms since start, for ordering), `seq`, `event` and `message`, plus `backend`
and `request_id` (the X-Request-Id) when they apply.

| Field | Type | Valid range | Meaning |
|---|---|---|---|
| `event_log.path` | string | file path, or `""` | Log file (its directory is created). `""` keeps events in memory only (for the dashboard), with no file |
| `event_log.max_file_bytes` | integer | 4096–1073741824 | Rotate before the file would exceed this size |
| `event_log.max_files` | integer | 1–100 | Rotated files kept: `path.1` (newest) … `path.N` (oldest is deleted) |
| `event_log.max_queue` | integer | 100–10000000 | Entries waiting for the writer. Past this, new entries are dropped and counted (`events_dropped`) instead of blocking a worker |
| `event_log.recent_events` | integer | 0–100000 | Newest entries kept in memory for the dashboard's live list |
| `event_log.trace_requests` | boolean | `true` / `false` | Debug mode: log every pipeline step of every request as `request_step` (plan IV.18). High volume |

Events written: `engine_started`, `engine_stopped`, `backend_marked_unhealthy` (with `reason`,
`failures` and `check`: `active` for probes, `passive` for real requests, with the request that
tipped it over), `backend_marked_healthy` (`successes`, `check`), `no_backend_available`, `connection_rejected`
(over `max_client_connections`), `pool_rejected` (`queue_full` / `wait_timeout`), `backend_error`
(`status`, `reason`), `response_aborted`, `timeout` (client-side timeouts except keep-alive idle),
`retry` and `retry_result` (stale pooled connection), `config_watch_started`,
`config_reload_accepted` (`source`, `added`, `removed`, `reweighted`), `config_reload_rejected`
(`source`, `errors`), `health_checks_not_restarted`, `sticky_reassigned` (`group`, `from`, `to`,
`reason`), `sticky_table_full` (at most once per maintenance interval, with how many sessions
were not stored), and `request_step` in debug mode. In debug mode the steps are
`request_received`, `group_routed` (with the group and the rule, or `default`),
`affinity_checked` (sticky groups only: `sticky to <id>`, `new session on <id>`,
`no session cookie; <id>` or `reassigned from <id> (<reason>) to <id>`), `backend_selected`, `backend_connected`, `request_forwarded`, `response_received` and
`response_completed`, plus `error_response`, `aborted` and `timed_out` on error paths. Malformed
requests and keep-alive idle closes are counted but not logged, since they're common and
client-controlled.

## `dashboard`: operator UI (plan IV.17)
`LoadBalancer.exe --config <file.json> [--minimized]` hosts the engine and the dashboard. Without
`--config`, it asks for a file. A rejected config is shown with every error, and nothing starts.
The engine copies a snapshot on its own publisher thread and hands it to the UI thread as a
posted message; the UI repaints from that copy on a timer and never reads engine state directly.
Closing the window shuts the proxy down gracefully (`timeouts.shutdown_grace_ms`). The status line
counts applied and rejected reloads, and each reload appears in the event list. If the config file
can't be watched, a warning says so and the proxy keeps running without hot reload.

| Field | Type | Valid range | Meaning |
|---|---|---|---|
| `dashboard.publish_interval_ms` | integer | 50–10000 | How often the engine publishes a snapshot; the UI repaints at the same rate |
| `dashboard.event_rows` | integer | 10–100000 | Rows kept in the live event list (newest first) |

## `config_reload`: hot reload (plan IV.14)
`LoadBalancer.exe` and `lb_console` watch the file given with `--config`. The watcher listens on the
file's directory (`ReadDirectoryChangesW`), so it also sees saves that write a temporary file and
rename it over the config. Editors often save in several writes, so the file is read only after it
has been quiet for `debounce_ms`. A change whose content is byte-for-byte the active config's
(compared by a content hash) is skipped, so a save from the GUI doesn't cause a second reload.

| Field | Type | Valid range | Meaning |
|---|---|---|---|
| `config_reload.watch_file` | boolean | `true` / `false` | Reload the config file automatically when it changes. With `false`, reloads happen only on request (`reload` command) |
| `config_reload.debounce_ms` | integer | 10–60000 | Quiet time after the last change before the file is read. Every further write restarts it |

## `sticky_table`: session affinity map (plan IV.9, V)
One table for all groups, keyed by (group, session key). It is split into shards with a lock each,
so requests contend only within one shard; the maintenance thread removes expired sessions.

| Field | Type | Valid range | Meaning |
|---|---|---|---|
| `sticky_table.shards` | integer | 1–1024 | Number of independently locked shards |
| `sticky_table.max_entries` | integer | 1–100000000, ≥ `shards` | Sessions the table can hold. Each shard holds `max_entries / shards`. When a shard is full, a new session is served but not made sticky (no cookie is issued), counted as `sticky_not_stored`, and reported by `sticky_table_full`. Sessions already stored keep working |

## `balancing`: least response time (plan IV.7)
Used by every group with `"strategy": "least_response_time"`. Each backend's average is
time-weighted: a new sample's weight grows with the time since that backend's previous sample,
so the average covers about the same span of time whether the backend gets 10 or 10,000 requests
a second.

| Field | Type | Valid range | Meaning |
|---|---|---|---|
| `balancing.response_time_decay_ms` | integer | 100–600000 | EWMA time constant: the old average's weight is `exp(-elapsed / decay)`, so a sample this old counts for 1/e of what it did. Smaller reacts faster, larger is steadier |
| `balancing.response_time_expiry_ms` | integer | 100–3600000 | An average with no sample for this long is dropped. The backend is then scored like the best one and measured again, so a backend that was slow once isn't avoided forever |

## `timeouts` (plan VI)
Every wait has a limit, measured on the monotonic clock, so changing the system clock has no
effect. "Absolute" means a fixed deadline that more traffic does not extend; "idle" means the
deadline moves forward each time bytes move. The plan VI pooled-idle timeout is
`pool.idle_timeout_ms`, and the drain timeout arrives in step 2.6.

| Field | Type | Valid range | Kind | Applies to | On expiry |
|---|---|---|---|---|---|
| `timeouts.client_header_ms` | integer | 1–600000 | absolute | The whole request head. For a connection's first request it counts from the accept; for later requests, from their first byte | Close the connection, no response (slowloris defense) |
| `timeouts.client_body_idle_ms` | integer | 1–600000 | idle | Gaps while the client sends the request body | **408**, close |
| `timeouts.client_keepalive_idle_ms` | integer | 1–600000 | absolute | An idle client connection between requests, until the next request's first byte | Close the connection |
| `timeouts.client_write_idle_ms` | integer | 1–600000 | idle | A client that stops reading the response | Close the connection (RST) |
| `timeouts.backend_connect_ms` | integer | 1–600000 | absolute | Opening a new backend connection | Backend failure; **502** (phase 3: retry if eligible) |
| `timeouts.backend_response_ms` | integer | 1–600000 | absolute | From the request being fully sent until the response headers arrive | Backend failure; **504** |
| `timeouts.backend_idle_ms` | integer | 1–600000 | idle | A backend that stalls while taking the request body or sending the response body | Backend failure: **504** while sending the request, **502** before any response byte reached the client; otherwise the client connection is cut so it sees an incomplete response |
| `timeouts.shutdown_grace_ms` | integer | 0–600000 | absolute | On shutdown the proxy stops accepting and closes idle keep-alive connections at once. In-flight requests get this long to finish (their responses carry `Connection: close`) | Remaining connections are closed |

`client_write_idle_ms` and `backend_idle_ms` aren't rows in the plan VI table, but follow from
its rule that every wait has a limit.

## `trusted_proxies` (plan IV.6, VII)
| Field | Type | Valid range | Meaning |
|---|---|---|---|
| `trusted_proxies` | array of strings | IPv4 addresses or CIDRs (`10.0.0.0/8`, `192.168.1.7`); no host bits beyond the prefix; may be empty | Upstream proxies whose forwarding headers are believed. Only when the TCP peer is in this list are a client's `X-Forwarded-Proto`, `X-Forwarded-Host` and `X-Request-Id` kept, and does `X-Forwarded-For` decide the client's identity (the rate limiter in step 3.3). From anyone else these headers are untrusted |

Forwarding headers the backend receives:
- `X-Forwarded-For`: the client's values, merged into one field, with the TCP peer address appended. It is never replaced.
- `X-Forwarded-Proto`: `http` (`https` on TLS listeners in phase 4), unless a trusted proxy sent one.
- `X-Forwarded-Host`: the client's `Host`, unless a trusted proxy sent one.
- `X-Request-Id`: 32 hex characters, unique per request; a trusted proxy's well-formed id is kept. The same id is returned to the client in its response, including on proxy-generated errors.
- Hop-by-hop headers (`Connection`, `Keep-Alive`, `Proxy-Connection`, `TE`, `Trailer`, `Upgrade`, plus anything named in `Connection`) and `Expect` are removed, and the body is re-framed.

## `groups`: backend groups (plan IV.4, IV.7)
`groups` is a non-empty array. Each group:

| Field | Type | Valid range | Meaning |
|---|---|---|---|
| `groups[].name` | string | name rules; unique across groups | Group name, used by routing |
| `groups[].strategy` | string | `"round_robin"`, `"least_connections"`, `"weighted_round_robin"`, `"least_response_time"` or `"ip_hash"` | How a backend is picked within the group (plan IV.7); see the table below. Only healthy, non-draining backends are candidates; with none, the request gets **503** at once |
| `groups[].host_header` | string | `"preserve"` or `"backend"` | `preserve` forwards the client's `Host` (plan IV.6 default); `backend` rewrites it to the chosen backend's `address:port`. `X-Forwarded-Host` carries the original either way |
| `groups[].backends` | array | at least one entry | Backends in this group |

Strategies (plan IV.7). None takes a lock when it picks:

| Strategy | Picks | Notes |
|---|---|---|
| `round_robin` | Each eligible backend in turn | An excluded backend's share is spread evenly over the rest. Weights are ignored |
| `least_connections` | Fewest in-flight requests | Ties are broken by rotation, so the first backend is not always chosen. Weights are ignored |
| `weighted_round_robin` | Shares in proportion to `weight`, interleaved | A precomputed cycle (weights divided by their greatest common divisor) gives each backend exactly its weight in turns, spread evenly: 3:1 is `A A B A`, not `A A A B`. A turn that falls on an excluded backend goes to the others in proportion to their weights |
| `least_response_time` | Lowest *response-time average × (in-flight requests + 1)* | Each backend keeps an exponentially weighted moving average (EWMA) of its response time, from backend connection ready to the last response byte (see [`balancing`](#balancing-least-response-time-plan-iv7)). A failed request (connect error, timeout, broken response) counts as taking the full `timeouts.backend_response_ms`, so a backend that fails fast never looks fast. A backend with no average (new, or idle past `response_time_expiry_ms`) is scored with the best average in the group, so it gets traffic and is measured again. With no averages at all this behaves as least connections. Ties are broken by rotation |
| `ip_hash` | The backend chosen by the client's address | Keyed by the client identity: the TCP peer, or the `X-Forwarded-For` client when the peer is in `trusted_proxies` (the same trust rule as everywhere else). Weighted rendezvous hashing: a client keeps its backend as long as that backend is eligible, and when a backend is excluded or added only the clients it loses or gains move. Shares follow the weights. A reload maps every client the same way |

`groups[].health`: active health checks (plan IV.10), run on a dedicated thread with
non-blocking probes, so a slow probe never delays other probes or live traffic:

| Field | Type | Valid range | Meaning |
|---|---|---|---|
| `health.type` | string | `"http"` or `"tcp"` | `http`: `GET <path>`; a status of 200–399 is healthy. `tcp`: a completed connect is healthy |
| `health.path` | string | starts with `/`, visible ASCII, ≤ 1024 chars | Path for HTTP probes. Required for `tcp` too, though unused |
| `health.interval_ms` | integer | 10–3600000 | Time between probe starts for each backend. Each backend's first probe is spread over the first interval |
| `health.timeout_ms` | integer | 1–600000, ≤ `interval_ms` | One probe's limit, covering connect, request and status line. Longer counts as a failure |
| `health.unhealthy_threshold` | integer | 1–100 | **N**: consecutive failed probes before the backend is marked unhealthy. Its idle pooled connections are then closed and it gets no new requests |
| `health.healthy_threshold` | integer | 1–100 | **M**: consecutive successful probes before an unhealthy backend is marked healthy again |

A killed backend is excluded within `interval_ms × N + timeout_ms` (plan IV.10). Health
checks only move a backend between healthy and unhealthy; they never change a draining
backend. Every backend starts healthy. Probe sockets skip Windows' connect retries after a
refusal, so a dead backend fails a probe at once instead of after about a second.

`groups[].passive_health`: passive health checks (plan IV.10, phase 2). Real request outcomes
count toward health too, so a backend that fails real traffic is taken out without waiting for
probes:

| Field | Type | Valid range | Meaning |
|---|---|---|---|
| `passive_health.enabled` | boolean | `true` / `false` | Count real request outcomes toward health |
| `passive_health.consecutive_failures` | integer | 1–10000 | Failed requests to one backend in a row, with no successful one in between, before it is marked unhealthy (its idle pooled connections are closed and it gets no new requests) |
| `passive_health.count_5xx` | boolean | `true` / `false` | Whether a 5xx response from the backend counts as a failure. Off by default because a 5xx is often the application's answer, not a sick backend. Errors the proxy generates itself are never the backend's 5xx |

What counts:
- **Failure:** the connect fails or times out, sending the request fails, the response times out
  (`backend_response_ms`, `backend_idle_ms`), the response is malformed or cut off, and with
  `count_5xx` a 5xx status.
- **Success** (resets the count): any other complete response.
- **Neither:** requests the proxy answered itself (no eligible backend, pool full) and requests
  whose client went away, since those are not the backend's fault.

Under concurrent traffic "in a row" means no success was recorded between the failures, in the
order requests finished. Passive checks only take a backend out; they never bring one back,
because an unhealthy backend gets no traffic to judge it by. **Active probes bring it back**, and
only after `healthy_threshold` successful probes counted after the mark-down. Probes that
succeeded before it don't count, so a backend failing real requests while its health endpoint
still answers is not let straight back in. Like active checks, passive checks never change a
draining backend.

`groups[].sticky`: session affinity (plan IV.9). The lookup runs **after routing, inside the
group routing chose**, and before the balancer. A session is a cookie value, mapped in the
sticky table to a backend id; each use restarts its TTL.

| Field | Type | Valid range | Meaning |
|---|---|---|---|
| `sticky.mode` | string | `"off"`, `"application_cookie"` or `"inserted_cookie"` | `off`: no affinity. `application_cookie`: the session key is the application's own cookie, learned when the backend sets it. `inserted_cookie`: the proxy issues its own cookie |
| `sticky.cookie` | string or `null` | `null` when `mode` is `off`; otherwise a cookie name of 1–256 token characters | Cookie that carries the session key |
| `sticky.ttl_ms` | integer | 1000–604800000 (7 days) | A session not used for this long is forgotten; the next request is balanced afresh |

How a request is placed:
- **The session's backend is eligible** (healthy, not draining): the request goes there.
- **It is unhealthy, draining or no longer in the group**: the balancer picks a backend in the
  same group, the session moves there for good, and `sticky_reassigned` is logged with the old
  backend, the new one and the reason. If the old backend recovers, the session stays where it moved.
- **The cookie value is unknown** (new, expired, or issued before a restart): the balancer picks,
  and the session is mapped to that backend from then on.
- **No cookie**: the balancer picks. With `inserted_cookie`, the proxy maps a new random key
  (128 bits, 32 hex characters) to that backend and adds
  `Set-Cookie: <cookie>=<key>; Path=/; HttpOnly; SameSite=Lax` to the response. With
  `application_cookie`, a `Set-Cookie` for that cookie in the backend's response maps its value
  to the backend that sent it; the header passes through unchanged.
- A proxy cookie whose value is not a 32-character hex key is ignored and replaced.

The cookie is not marked `Secure` while the proxy only speaks HTTP (TLS is phase 4). Sessions
live in memory only: a restart forgets them, and each client is placed afresh on its next request
(plan XII). A reload keeps every session.

Each backend:

| Field | Type | Valid range | Meaning |
|---|---|---|---|
| `groups[].backends[].id` | string | name rules; unique across **all** groups | Stable backend identity. The registry keys live state (health, drain, counters) by this id |
| `groups[].backends[].address` | string | IPv4 literal, not `0.0.0.0` | Backend address |
| `groups[].backends[].port` | integer | 1–65535 | Backend port. The same `address:port` may not appear twice in one group (it may appear in different groups) |
| `groups[].backends[].weight` | integer | 1–1000 | Relative capacity, used by `weighted_round_robin` and `ip_hash` and shown on the dashboard |

## `routing`: content-aware routing (plan IV.8)
Routing picks the **group** for every request. It runs before session affinity and load
balancing (plan III), so a client with a sticky backend in one group still reaches the group its
request routes to. The rules are tried **in array order and the first match wins**; a request
that matches none goes to `default_group`. Routing never falls back to another group: if the
chosen group has no eligible backend, the request gets **503**, logged as `no_backend_available`
with the group and the rule that chose it. Every request on a keep-alive connection is routed on
its own. Matching allocates nothing and takes no lock. Rules apply live on reload, from the next
request.

| Field | Type | Valid range | Meaning |
|---|---|---|---|
| `routing.default_group` | string | name of an existing group | Group for requests that no rule matches |
| `routing.rules` | array | may be empty | Routing rules in priority order |

Each rule has all five fields:

| Field | Type | Valid range | Meaning |
|---|---|---|---|
| `rules[].id` | string | name rules; unique among rules | Shown in the debug trace and in `no_backend_available` events |
| `rules[].type` | string | `"path_prefix"`, `"path_glob"`, `"header"` or `"cookie"` | What the rule matches |
| `rules[].field` | string or `null` | `null` for path rules; a header or cookie name (1–256 token characters) otherwise | Which header or cookie to look at |
| `rules[].value` | string or `null` | path rules: starts with `/`, visible ASCII, no `?` or `#`, at most 1024 characters. Header and cookie rules: visible ASCII (headers may contain inner spaces) without leading or trailing spaces, at most 1024 characters, or `null` | What must match; `null` (header and cookie rules only) means the header or cookie just has to be present |
| `rules[].group` | string | name of an existing group | Group a matching request goes to |

How each type matches:

| Type | Matches when |
|---|---|
| `path_prefix` | The path equals `value` or continues it with a new segment: `/api` matches `/api`, `/api/` and `/api/v1`, but not `/apiary`. A `value` ending in `/` matches everything below it |
| `path_glob` | The whole path matches `value`, where `*` matches any run of characters, including `/`: `/reports/*.csv` matches `/reports/2026/q1.csv`. Worst case O(path × pattern), never exponential |
| `header` | A header named `field` (case-insensitive) is present and, unless `value` is `null`, one of its lines has exactly `value` (case-sensitive, surrounding spaces ignored) |
| `cookie` | A cookie named `field` (case-sensitive) is present in a `Cookie` header and, unless `value` is `null`, has exactly `value` (surrounding double quotes removed) |

The path is the request target up to `?` or `#`; an absolute-form target (`http://host/path`)
uses its path. It is compared **as sent**: no percent-decoding, no case folding and no `.`/`..`
removal. Routing chooses where a request goes; it is not an access-control mechanism.

Example (also in [`config/routing.example.json`](../config/routing.example.json)):
```json
"routing": {
  "default_group": "web",
  "rules": [
    { "id": "api-path",    "type": "path_prefix", "field": null,      "value": "/api",           "group": "api" },
    { "id": "api-header",  "type": "header",      "field": "X-Group", "value": "api",            "group": "api" },
    { "id": "beta-cookie", "type": "cookie",      "field": "beta",    "value": null,             "group": "api" },
    { "id": "reports",     "type": "path_glob",   "field": null,      "value": "/reports/*.csv", "group": "api" }
  ]
}
```
