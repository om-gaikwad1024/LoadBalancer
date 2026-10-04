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

## `workers`: IOCP worker pool (plan II.2, V)
| Field | Type | Valid range | Meaning |
|---|---|---|---|
| `workers.threads` | integer or `"auto"` | 1–256, or exactly `"auto"` | Fixed number of IOCP worker threads created at startup. `"auto"` = one per logical processor |

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
