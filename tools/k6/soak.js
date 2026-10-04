// Soak load (plan IX "Soak"): steady keep-alive traffic plus a stream of requests that open
// and close a connection each (Connection: close), so sockets, sessions and pooled backend
// connections are created and destroyed for the whole run. Both are constant-arrival-rate.
//
//   k6 run -e TARGET=http://127.0.0.1:8092/ -e RATE=1000 -e CHURN_RATE=50 -e DURATION=30m soak.js
import http from 'k6/http';

const DURATION = __ENV.DURATION || '30m';

export const options = {
  discardResponseBodies: true,
  summaryTrendStats: ['avg', 'p(50)', 'p(95)', 'p(99)', 'max'],
  scenarios: {
    steady: {
      executor: 'constant-arrival-rate',
      exec: 'steady',
      rate: Number(__ENV.RATE || 1000),
      timeUnit: '1s',
      duration: DURATION,
      preAllocatedVUs: 50,
      maxVUs: 1000,
    },
    churn: {
      executor: 'constant-arrival-rate',
      exec: 'churn',
      rate: Number(__ENV.CHURN_RATE || 50),
      timeUnit: '1s',
      duration: DURATION,
      preAllocatedVUs: 10,
      maxVUs: 200,
    },
  },
};

const TARGET = __ENV.TARGET || 'http://127.0.0.1:8092/';

export function steady() {
  http.get(TARGET);
}

export function churn() {
  http.get(TARGET, { headers: { Connection: 'close' } });
}

export function handleSummary(data) {
  const m = data.metrics;
  const d = m.http_req_duration.values;
  const out = {
    requests: m.http_reqs.values.count,
    achieved_rps: m.http_reqs.values.rate,
    failed_rate: m.http_req_failed.values.rate,
    dropped_iterations: m.dropped_iterations ? m.dropped_iterations.values.count : 0,
    p50_ms: d['p(50)'],
    p95_ms: d['p(95)'],
    p99_ms: d['p(99)'],
    max_ms: d.max,
  };
  return {
    [__ENV.SUMMARY || 'k6-soak-summary.json']: JSON.stringify(out, null, 2),
    stdout: JSON.stringify(out) + '\n',
  };
}
