# mock_backend

Configurable HTTP/1.1 backend for tests and load runs (plan IX). It uses blocking Winsock with one
thread per connection, supports keep-alive, and has its own small HTTP parser; it never uses the
engine's parser. Every response carries `X-Backend-Id`, so tests can see which backend answered.

```bash
build\debug\tools\mock_backend\mock_backend.exe --port 9001 --id web-1
```

| Option | Default | Meaning |
|---|---|---|
| `--port` | required | Listen port. `0` = ephemeral; the port is printed on the startup line |
| `--bind` | `127.0.0.1` | Listen address |
| `--id` | the port | `X-Backend-Id` value |
| `--seed` | `1` | Fault decisions are a deterministic function of seed and request number |
| `--max-connections` | `1024` | Concurrent connection cap. Over it: 503 and close |
| `--health-path` | `/health` | Health endpoint. It answers `health_status` and no other fault applies to it |

## Fault switches
Set them at startup (`--latency-ms 50`) or while the mock runs (`GET /__mock/set?latency_ms=50&error_rate=0.1`).

| Switch | Default | Effect |
|---|---|---|
| `latency_ms` | 0 | Sleep before answering |
| `error_rate` / `error_status` | 0 / 500 | Share of requests answered with `error_status` |
| `close_rate` | 0 | Share of requests answered by an abrupt close (RST), with no bytes sent |
| `partial_rate` | 0 | Share answered with the full head and half the promised body, then FIN |
| `echo_headers` | 0 | Response body = the request line and headers as received |
| `echo_body` | 0 | Response body = the received request body, de-chunked (after the head if `echo_headers` is also on) |
| `health_status` | 200 | Status of the health path |
| `body_bytes` | 2 | Size of a normal 200 body (`ok`, padded with `.`) |

## Control endpoints
`/__mock/set?k=v&...` (changes switches; returns them as JSON) · `/__mock/faults` · `/__mock/stats`
(requests, connections, status counts, aborted, partial) · `/__mock/reset` (clears the stats).

Throughput limit: not measured yet. It is measured with k6 in step 1.11, before the mock serves
as the direct-to-backend baseline, and recorded in `docs/benchmarks.md`.
