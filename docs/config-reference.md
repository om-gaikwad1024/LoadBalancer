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

## `maintenance` (plan V)
| Field | Type | Valid range | Meaning |
|---|---|---|---|
| `maintenance.interval_ms` | integer | 10–60000 | How often the maintenance thread runs: it closes idle pooled connections past `pool.idle_timeout_ms` (later also expired sticky-session and rate-limit entries) |

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
| `groups[].host_header` | string | `"preserve"` or `"backend"` | `preserve` forwards the client's `Host` (plan IV.6 default); `backend` rewrites it to the chosen backend's `address:port`. `X-Forwarded-Host` carries the original either way |
| `groups[].backends` | array | at least one entry | Backends in this group |

Each backend:

| Field | Type | Valid range | Meaning |
|---|---|---|---|
| `groups[].backends[].id` | string | name rules; unique across **all** groups | Stable backend identity. The registry keys live state (health, drain, counters) by this id |
| `groups[].backends[].address` | string | IPv4 literal, not `0.0.0.0` | Backend address |
| `groups[].backends[].port` | integer | 1–65535 | Backend port. The same `address:port` may not appear twice in one group (it may appear in different groups) |
| `groups[].backends[].weight` | integer | 1–1000 | Relative capacity, used by weighted round robin (phase 2) and shown on the dashboard |

## `routing` (plan IV.8)
| Field | Type | Valid range | Meaning |
|---|---|---|---|
| `routing.default_group` | string | name of an existing group | Group that receives a request when no routing rule matches. In phase 1 every request goes here |
