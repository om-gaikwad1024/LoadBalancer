// Open-model load (plan IX: constant-arrival-rate only, so tail latency is not hidden by
// coordinated omission). Requests start on schedule whether or not earlier ones finished.
//
//   k6 run -e TARGET=http://127.0.0.1:8090/ -e RATE=2000 -e DURATION=30s -e SUMMARY=out.json constant_rate.js
//
// Env: TARGET, RATE (requests/s), DURATION, VUS / MAX_VUS (pre-allocated / maximum VUs),
//      SUMMARY (file for the JSON summary).
import http from 'k6/http';

export const options = {
  discardResponseBodies: true,
  summaryTrendStats: ['avg', 'min', 'med', 'p(50)', 'p(95)', 'p(99)', 'max'],
  scenarios: {
    load: {
      executor: 'constant-arrival-rate',
      rate: Number(__ENV.RATE || 1000),
      timeUnit: '1s',
      duration: __ENV.DURATION || '30s',
      preAllocatedVUs: Number(__ENV.VUS || 100),
      maxVUs: Number(__ENV.MAX_VUS || 2000),
    },
  },
};

const TARGET = __ENV.TARGET || 'http://127.0.0.1:8090/';

export default function () {
  http.get(TARGET);
}

export function handleSummary(data) {
  const m = data.metrics;
  const d = m.http_req_duration.values;
  const out = {
    target: TARGET,
    rate_target: Number(__ENV.RATE || 1000),
    duration: __ENV.DURATION || '30s',
    requests: m.http_reqs.values.count,
    achieved_rps: m.http_reqs.values.rate,
    failed_rate: m.http_req_failed.values.rate,
    dropped_iterations: m.dropped_iterations ? m.dropped_iterations.values.count : 0,
    vus_max: m.vus_max.values.max,
    avg_ms: d.avg,
    p50_ms: d['p(50)'],
    p95_ms: d['p(95)'],
    p99_ms: d['p(99)'],
    max_ms: d.max,
  };
  return {
    [__ENV.SUMMARY || 'k6-summary.json']: JSON.stringify(out, null, 2),
    stdout: JSON.stringify(out) + '\n',
  };
}
