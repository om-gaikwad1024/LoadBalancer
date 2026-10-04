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

## `timeouts` (plan VI)
Every interval is measured on the monotonic clock. Step 1.6 adds the rest of the plan VI timeouts table.

| Field | Type | Valid range | Meaning |
|---|---|---|---|
| `timeouts.shutdown_grace_ms` | integer | 0–600000 | On shutdown: the proxy stops accepting and closes idle keep-alive connections at once. In-flight requests get this long to finish (their responses carry `Connection: close`), then the remaining connections are closed |

## `groups`: backend groups (plan IV.4, IV.7)
`groups` is a non-empty array. Each group:

| Field | Type | Valid range | Meaning |
|---|---|---|---|
| `groups[].name` | string | name rules; unique across groups | Group name, used by routing |
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
