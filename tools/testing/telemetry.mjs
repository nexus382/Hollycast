// Portions Copyright 2026 The Hollycast Authors
const MAX_TELEMETRY_AGE_MS = 500;

export function hasFreshTelemetry(sample) {
  const telemetry = sample?.telemetry;
  return telemetry?.sample_available === true && telemetry.stale === false
    && Number.isFinite(telemetry.sample_age_ms) && telemetry.sample_age_ms >= 0
    && telemetry.sample_age_ms <= MAX_TELEMETRY_AGE_MS;
}

function check(condition, message) {
  if (!condition) throw new Error(message);
}

export function summarizePerformance(result) {
  check(result?.domain === 'host_present_interval_ms', 'Unexpected performance measurement domain');
  check(Array.isArray(result.samples_ms) && result.samples_ms.length >= 2 && result.samples_ms.length <= 36000,
    'Performance capture needs 2..36000 actual presentation intervals');
  check(result.samples_ms.every(sample => Number.isFinite(sample) && sample > 0), 'Nonpositive or nonfinite frame interval');
  check(typeof result.valid === 'boolean' && Array.isArray(result.invalid_reasons), 'Missing native validity evidence');
  const sorted = [...result.samples_ms].sort((a, b) => a - b);
  const percentile = fraction => sorted[Math.max(0, Math.ceil(sorted.length * fraction) - 1)];
  const mean = sorted.reduce((sum, sample) => sum + sample, 0) / sorted.length;
  return { domain: result.domain, samples: sorted.length, mean_ms: mean, median_ms: percentile(0.5),
    p95_ms: percentile(0.95), p99_ms: percentile(0.99), max_ms: sorted.at(-1),
    presented_intervals_per_second: 1000 / mean, valid: result.valid && result.invalid_reasons.length === 0,
    invalid_reasons: result.invalid_reasons, observations_during_capture: result.observations_during_capture,
    gpu_timing_measured: false, physical_latency_measured: false };
}
