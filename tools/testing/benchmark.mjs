// Portions Copyright 2026 The Hollycast Authors
import * as fs from 'node:fs/promises';
import path from 'node:path';
import { randomBytes } from 'node:crypto';
import { performance } from 'node:perf_hooks';
import os from 'node:os';
import { isDeepStrictEqual } from 'node:util';
import { setTimeout as delay } from 'node:timers/promises';

const METRICS = {
  game_fps: 'Game FPS', output_fps: 'Output FPS', vblank_hz: 'Guest vblank Hz',
  emulation_speed_percent: 'Emulation speed (%)', process_cpu_percent: 'Process CPU (%)',
  system_cpu_percent: 'System CPU (%)', gpu_busy_percent: 'GPU busy (%)', gpu_render_ms: 'GPU render (ms)',
  frame_average_ms: 'OSD average frame time (ms)', process_resident_bytes: 'Resident memory (bytes)',
};
const check = (condition, message) => { if (!condition) throw new Error(message); };
const hasFreshTelemetry = sample => sample?.telemetry?.sample_available === true
  && sample.telemetry.stale === false && Number.isFinite(sample.telemetry.sample_age_ms)
  && sample.telemetry.sample_age_ms >= 0 && sample.telemetry.sample_age_ms <= 500;

export function benchmarkSettings(overrides = {}, scenario = 'current') {
  const config = { 'pvr.rend': 0, 'rend.Resolution': 480, 'rend.vsync': true, ...overrides.config };
  if (scenario === 'planet-harriers')
    Object.assign(config, { HikaruSorting: 0, HikaruPlanetRegion: 1, HikaruPlanetDpadStick: false }, overrides.config);
  return { ...overrides, config,
  rend: { ResourceMonitorMode: 2, OverlayShowFPS: true, OverlayShowOutputFPS: true,
    OverlayShowEmulationSpeed: true, OverlayShowHostCpuUsage: true, OverlayShowGpuUsage: true,
    OverlayShowFrameTimes: true, OverlayShowRam: true, ...overrides.rend } };
}

export function summarizeOsd(samples) {
  const values = Object.fromEntries(Object.keys(METRICS).map(key => [key, []]));
  const stamps = new Set();
  let duplicate = 0, stale = 0, unavailable = 0;
  for (const sample of samples) {
    const osd = sample.telemetry?.osd;
    if (!osd?.collecting || !Number.isFinite(osd.sampled_at_monotonic_ms) || osd.sampled_at_monotonic_ms <= 0) { unavailable++; continue; }
    if (!Number.isFinite(osd.sample_age_ms) || osd.sample_age_ms < 0 || osd.sample_age_ms > 2500) { stale++; continue; }
    if (stamps.has(osd.sampled_at_monotonic_ms)) { duplicate++; continue; }
    stamps.add(osd.sampled_at_monotonic_ms);
    for (const key of Object.keys(values)) {
      const value = osd[key];
      if (typeof value === 'number' && Number.isFinite(value) && value >= 0) values[key].push(value);
    }
  }
  return { unique_samples: stamps.size, duplicate_samples: duplicate, stale_samples: stale, unavailable_samples: unavailable,
    metrics: Object.fromEntries(Object.entries(values).map(([key, numbers]) => [key, { count: numbers.length,
      mean: numbers.length ? numbers.reduce((sum, value) => sum + value, 0) / numbers.length : null,
      min: numbers.length ? Math.min(...numbers) : null, max: numbers.length ? Math.max(...numbers) : null }])) };
}

export function auditConditions(samples, initial) {
  let previous = initial;
  const changes = [];
  for (const sample of samples) {
    const current = { settings: sample.state.settings, display: sample.state.display };
    if (!isDeepStrictEqual(current, previous)) {
      changes.push({ elapsed_ms: sample.elapsed_ms, ...current });
      previous = current;
    }
  }
  return { conditions_stable: changes.length === 0, condition_changes: changes,
    warnings: changes.length ? ['Output dimensions or emulator settings changed during capture. Use fixed conditions for performance comparisons.'] : [] };
}

async function publishJson(file, value, replace = false) {
  const temporary = `${file}.${randomBytes(8).toString('hex')}.tmp`;
  try {
    await fs.writeFile(temporary, `${JSON.stringify(value, null, 2)}\n`, { flag: 'wx' });
    if (replace) await fs.rename(temporary, file);
    else await fs.link(temporary, file);
  } finally { await fs.rm(temporary, { force: true }); }
}

export function renderBenchmarkReport(report) {
  const escape = value => String(value ?? '').replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
  const number = value => typeof value === 'number' && Number.isFinite(value) ? value.toFixed(2) : 'Unavailable';
  const rows = Object.entries(METRICS).map(([key, label]) => {
    const metric = report.osd?.metrics[key];
    const scale = key === 'process_resident_bytes' ? 1024 * 1024 : 1;
    const cells = ['mean', 'min', 'max'].map(field => `<td>${number(typeof metric?.[field] === 'number' ? metric[field] / scale : null)}</td>`).join('');
    return `<tr><th>${key === 'process_resident_bytes' ? 'Resident memory (MiB)' : label}</th>${cells}<td>${metric?.count ?? 0}</td></tr>`;
  }).join('');
  const timeline = report.timeline ?? [];
  const charts = [['game_fps', 'Game FPS'], ['process_cpu_percent', 'Process CPU (%)']].map(([key, label]) => {
    const points = timeline.filter(point => Number.isFinite(point[key]));
    if (!points.length) return '';
    const max = Math.max(1, ...points.map(point => point[key]));
    const duration = Math.max(1, report.native?.elapsed_ms ?? report.requested_duration_ms);
    // Use actual elapsed timestamps. Missing samples are not filled with zeros.
    const coordinates = points.map(point => `${(44 + point.elapsed_ms / duration * 596).toFixed(1)},${(128 - point[key] / max * 100).toFixed(1)}`).join(' ');
    return `<figure><figcaption>${label}</figcaption><svg viewBox="0 0 664 154" role="img" aria-label="${label} over elapsed seconds"><path d="M44 20V128H640" fill="none" stroke="#57677b"/><text x="0" y="28">${number(max)}</text><text x="26" y="132">0</text><text x="44" y="150">0 s</text><text x="590" y="150">${number(duration / 1000)} s</text><polyline points="${coordinates}" fill="none" stroke="#70dfbe" stroke-width="2"/></svg></figure>`;
  }).join('');
  const p = report.presentation;
  return `<!doctype html>
<!-- Portions Copyright 2026 The Hollycast Authors -->
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Hollycast benchmark</title>
<style>body{margin:0;background:#101820;color:#e9eef5;font:16px/1.5 system-ui,sans-serif}main{max-width:1040px;margin:48px auto;padding:0 24px}h1{font-size:38px;margin:4px 0}h2{margin-top:32px}small,.muted{color:#a9bacb}.badge{display:inline-block;padding:5px 12px;border:1px solid #70dfbe;border-radius:20px;color:#70dfbe}.cards,.charts{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:16px}.card,figure{margin:0;background:#192631;border:1px solid #304454;padding:18px;border-radius:10px}.value{font-size:30px;font-weight:650}table{width:100%;border-collapse:collapse}th,td{padding:10px;text-align:right;border-bottom:1px solid #304454}th:first-child{text-align:left}svg{width:100%;font:12px system-ui;fill:#a9bacb}pre{white-space:pre-wrap;overflow-wrap:anywhere;background:#192631;padding:16px}img{width:100%;border-radius:10px}a{color:#70dfbe}</style>
<main><small>HOLLYCAST / NATIVE BENCHMARK</small><h1>${escape(report.game_id || 'Emulator test')}</h1>
<p class="badge">${report.valid ? 'VALID MEASUREMENT' : 'INCOMPLETE / INVALID MEASUREMENT'}</p>
${(report.warnings ?? []).map(warning => `<p style="padding:12px;border-left:3px solid #e4b66b;color:#e4b66b;background:#342d21">${escape(warning)}</p>`).join('')}
<p class="muted">${escape(report.scenario)} · ${escape(report.control)} input · ${number(report.native?.elapsed_ms / 1000)} seconds recorded. Boot and warm-up excluded.</p>
<div class="cards"><div class="card">Native guest boundaries / second<div class="value">${number(report.native?.guest_frame_boundary_hz)}</div><small>${escape(report.native?.guest_frame_boundary_source)}</small></div>
<div class="card">Presentation interval p95<div class="value">${number(p?.p95_ms)} ms</div><small>${p?.samples ?? 0} measured intervals</small></div>
<div class="card">OSD game FPS average<div class="value">${number(report.osd?.metrics.game_fps.mean)}</div><small>${report.osd?.unique_samples ?? 0} distinct OSD updates</small></div></div>
<h2>During gameplay</h2><div class="charts">${charts}</div><h2>OSD measurements</h2>
<table><thead><tr><th>Metric</th><th>Mean</th><th>Min</th><th>Max</th><th>Samples</th></tr></thead><tbody>${rows}</tbody></table>
<h2>Frame pacing</h2><p>Mean ${number(p?.mean_ms)} ms · median ${number(p?.median_ms)} ms · p99 ${number(p?.p99_ms)} ms · maximum ${number(p?.max_ms)} ms.</p>
<p>Intervals over 33.33 / 50 / 100 ms: ${escape(JSON.stringify(report.slow_intervals ?? {}))}.</p>
${report.screenshot ? `<h2>After measurement</h2><img alt="Native game and OSD capture after the timing window" src="${escape(report.screenshot.file)}">` : ''}
<h2>Conditions and evidence</h2><p>Live reads during capture: ${report.native?.observations_during_capture ?? 'unknown'}. Monitoring and controller overhead are included. GPU timing is available only when the native collector supplies it. Presentation intervals measure host pacing, not physical latency.</p>
<p>Sampled gameplay validation does not prove every game frame. Comparison verdict: ${escape(report.comparison_verdict)}. A single run does not establish a regression or speedup.</p>
${report.errors.length ? `<pre>${escape(report.errors.join('\n'))}</pre>` : ''}<pre>${escape(JSON.stringify({ identity: report.identity, settings: report.settings, display: report.display,
    conditions_stable: report.conditions_stable, condition_changes: report.condition_changes, sources: report.sources, cleanup: report.cleanup }, null, 2))}</pre>
<p><a href="benchmark.json">Full JSON and raw presentation intervals</a> · <a href="benchmark-telemetry.jsonl">Raw telemetry</a> · <a href="events.jsonl">Native command audit</a></p></main></html>`;
}

export async function runBenchmark(harness, { durationMs = 60000, warmupMs = 5000, startupTimeoutMs = 180000,
  scenario = 'current', control = 'external', stateProfile, after = 'stop', capture = false, signal, onProgress } = {}) {
  check(Number.isInteger(durationMs) && durationMs >= 100 && durationMs <= 600000, 'Invalid benchmark duration');
  check(Number.isInteger(warmupMs) && warmupMs >= 0 && warmupMs <= 120000, 'Invalid warm-up duration');
  check(Number.isInteger(startupTimeoutMs) && startupTimeoutMs >= 100 && startupTimeoutMs <= 600000, 'Invalid startup timeout');
  check(['planet-harriers', 'current'].includes(scenario), 'Unknown benchmark scenario');
  check(['scripted', 'external', 'none'].includes(control) && (control !== 'scripted' || scenario === 'planet-harriers'), 'Invalid control policy');
  check(['stop', 'pause', 'leave'].includes(after), 'Invalid completion action');
  // An immutable report belongs to exactly one benchmark attempt per session.
  await fs.mkdir(path.join(harness.directory, 'benchmark.lock'));
  const report = { schema: 'hollytest.benchmark.v1', scenario, control, requested_duration_ms: durationMs,
    warmup_ms: warmupMs, started_at: new Date().toISOString(), valid: false, errors: [],
    comparison_verdict: 'NOT_EVALUATED', timeline: [], cleanup: {}, artifacts: {
      report: path.join(harness.directory, 'report.html'), json: path.join(harness.directory, 'benchmark.json'),
      status: path.join(harness.directory, 'benchmark-status.json'), telemetry: path.join(harness.directory, 'benchmark-telemetry.jsonl') } };
  let latest, nativeStarted = false;
  const samples = [];
  const publish = async (phase, elapsedMs = 0) => {
    const status = { schema: 'hollytest.benchmark-status.v1', phase, updated_wall_ms: Date.now(), elapsed_ms: elapsedMs,
      requested_duration_ms: durationMs, scenario, control, game_id: latest?.state.game_id,
      game_state: latest?.telemetry.game_state, osd: latest?.telemetry.osd,
      telemetry_stale: latest?.telemetry.stale, sample_age_ms: latest?.telemetry.sample_age_ms, artifacts: report.artifacts };
    await publishJson(report.artifacts.status, status, true);
    onProgress?.(status);
  };
  const gameplay = sample => {
    check(sample.state.running && !sample.state.menu_open && !sample.state.input_restricted, 'Gameplay interrupted by a menu, pause, or restricted session');
    check(hasFreshTelemetry(sample), 'Native game progress is stale or unavailable');
    if (scenario === 'planet-harriers') {
      const game = sample.telemetry.game_state;
      check(sample.state.game_id === 'pharrier' && game.status === 'matched_operator_mapping', 'Planet Harriers profile identity mismatch');
      check(game.values.main_mode?.valid && game.values.main_mode.value === 3 && game.values.sub_mode_raw?.valid
        && game.values.sub_mode_raw.value === 4 && game.values.player_energy?.valid && game.values.player_energy.value > 0,
      'Planet Harriers left active stage gameplay');
    }
  };
  const window = async (milliseconds, phase) => {
    const start = performance.now(), end = start + milliseconds;
    let nextRead = start, nextInput = start;
    while (performance.now() < end) {
      signal?.throwIfAborted();
      const now = performance.now();
      if (control === 'scripted' && now >= nextInput) {
        const seconds = (now - start) / 1000;
        await harness.setController({ buttons: ['A'], axes: [Math.round(Math.sin(seconds / 3) * 16000),
          Math.round(Math.cos(seconds / 4) * 10000), 0, 0], leaseMs: 1500 });
        nextInput = performance.now() + 400;
      }
      if (now >= nextRead) {
        const requested = performance.now();
        latest = await harness.request('live', {}, { timeoutMs: 2000 });
        gameplay(latest);
        const sample = { phase, elapsed_ms: performance.now() - start, request_latency_ms: performance.now() - requested,
          received_wall_ms: Date.now(), ...latest };
        await fs.appendFile(report.artifacts.telemetry, `${JSON.stringify(sample)}\n`);
        if (phase === 'running') samples.push(sample);
        await publish(phase, sample.elapsed_ms);
        nextRead = performance.now() + 1000;
      }
      const wait = Math.min(end, nextRead, control === 'scripted' ? nextInput : Infinity) - performance.now();
      if (wait > 0) await delay(Math.max(1, wait), undefined, { signal });
    }
    latest = await harness.request('live', {}, { timeoutMs: 2000 });
    gameplay(latest);
    const sample = { phase, elapsed_ms: performance.now() - start, received_wall_ms: Date.now(), ...latest };
    await fs.appendFile(report.artifacts.telemetry, `${JSON.stringify(sample)}\n`);
    if (phase === 'running') samples.push(sample);
  };
  try {
    await publish('starting');
    signal?.throwIfAborted();
    const doctor = await harness.doctor();
    check(doctor.capabilities.includes('benchmark_frame_counts'), 'Rebuild the native harness with benchmark frame counters');
    check(doctor.capabilities.includes('exclusive_ui_input'), 'Rebuild the native harness with isolated native UI input');
    report.identity = { executable_sha256: harness.manifest.executable_sha256, game_sha256: harness.manifest.game_sha256,
      source_revision: harness.manifest.source_revision, host: { ...harness.manifest.host,
        cpu_model: os.cpus()[0]?.model ?? 'unknown', logical_processors: os.cpus().length,
        total_memory_bytes: os.totalmem() } };
    if (scenario === 'planet-harriers') {
      const profile = stateProfile ?? JSON.parse(await fs.readFile(new URL('./profiles/planet-harriers-live.json', import.meta.url), 'utf8'));
      await harness.setWatchProfile(profile);
      report.profile_id = profile.id;
      const deadline = performance.now() + startupTimeoutMs;
      let nextAction = 0;
      do {
        signal?.throwIfAborted();
        latest = await harness.request('live', {}, { timeoutMs: 2000 });
        check(!latest.state.input_restricted, 'Interactive testing is restricted');
        const game = latest.telemetry.game_state;
        if (latest.state.game_id === 'pharrier' && latest.telemetry.machine.available) {
          check(game.status === 'matched_operator_mapping', 'Planet Harriers profile identity mismatch');
          check(!latest.state.menu_open, 'Close the native menu before automatic game startup');
          const main = game.values.main_mode?.value, sub = game.values.sub_mode_raw?.value;
          if (main === 3 && sub === 4) break;
          if (latest.state.running && latest.telemetry.sequence >= 60 && performance.now() >= nextAction) {
            if (main === 0) {
              // Fresh cabinet NVRAM may have no credits. These are ordinary
              // emulated coin/start inputs, never a game-memory patch.
              await harness.press('D', 120);
              await harness.press('START', 180);
            }
            else if (main === 1) await harness.press('A', 180);
            nextAction = performance.now() + 2000;
          }
        }
        await publish('waiting_for_game');
        check(performance.now() < deadline, 'Timed out waiting for active Planet Harriers gameplay');
        await delay(250, undefined, { signal });
      } while (true);
    } else {
      const deadline = performance.now() + startupTimeoutMs;
      const deadlineMessage = 'Timed out waiting for a fresh running game state';
      do {
        signal?.throwIfAborted();
        const remainingMs = deadline - performance.now();
        check(remainingMs > 0, deadlineMessage);
        latest = await harness.request('live', {}, { timeoutMs: Math.max(1, Math.min(2000, Math.floor(remainingMs))) });
        signal?.throwIfAborted();
        check(performance.now() < deadline, deadlineMessage);
        check(!latest.state.input_restricted, 'Interactive testing is restricted');
        if (latest.state.running && !latest.state.menu_open && hasFreshTelemetry(latest)) break;
        await publish('waiting_for_game');
        const waitMs = deadline - performance.now();
        check(waitMs > 0, deadlineMessage);
        await delay(Math.min(250, waitMs), undefined, { signal });
        signal?.throwIfAborted();
        check(performance.now() < deadline, deadlineMessage);
      } while (true);
    }
    gameplay(latest);
    report.game_id = latest.state.game_id;
    report.settings = latest.state.settings;
    report.display = latest.state.display;
    await publish('warming_up');
    if (warmupMs) await window(warmupMs, 'warming_up');
    report.game_start = latest.telemetry.game_state;
    await harness.request('performance_begin');
    nativeStarted = true;
    await window(durationMs, 'running');
    report.game_end = latest.telemetry.game_state;
  } catch (error) {
    // Node's abortable timers wrap the caller's reason in AbortError. Retain
    // the original reason so cancellation evidence explains the interruption.
    const reason = signal?.aborted ? signal.reason : error;
    report.errors.push(reason instanceof Error ? reason.message : String(reason));
  }
  finally {
    if (nativeStarted) {
      try { report.native = await harness.request('performance_end'); }
      catch (error) { report.errors.push(`Native capture incomplete: ${error.message}`); }
    }
    try { await harness.release(); report.cleanup.inputs_released = true; }
    catch (error) { report.cleanup.release_error = error.message; }
  }
  if (report.native) {
    // The existing presentation summarizer owns its domain/validity checks.
    const { summarizePerformance } = await import('./hollytest.mjs');
    try {
      report.presentation = summarizePerformance(report.native);
      if (!report.presentation.valid) report.errors.push(...report.native.invalid_reasons);
      if (!Number.isFinite(report.native.elapsed_ms) || report.native.elapsed_ms < durationMs - 5) report.errors.push('Requested gameplay duration was not captured');
      if (!Number.isFinite(report.native.guest_frame_boundary_hz) || !Number.isInteger(report.native.guest_frame_boundary_count)
          || report.native.guest_frame_boundary_count <= 0) report.errors.push('Native guest frame counters are missing');
      report.slow_intervals = Object.fromEntries([['over_33_33_ms', 1000 / 30], ['over_50_ms', 50], ['over_100_ms', 100]]
        .map(([name, threshold]) => [name, report.native.samples_ms.filter(ms => ms > threshold).length]));
    } catch (error) { report.errors.push(error.message); }
  }
  report.osd = summarizeOsd(samples);
  Object.assign(report, auditConditions(samples, { settings: report.settings, display: report.display }));
  report.timeline = samples.map(sample => ({ elapsed_ms: sample.elapsed_ms, ...Object.fromEntries(Object.keys(METRICS).map(key => [key,
    sample.telemetry.osd?.collecting && sample.telemetry.osd.sample_age_ms <= 2500 ? sample.telemetry.osd[key] ?? null : null])) }));
  try { report.sources = await harness.verifySources(); }
  catch (error) { report.errors.push(error.message); }
  if (capture && report.native) {
    try { report.screenshot = (await harness.observe({ screenshot: true })).screenshot; }
    catch (error) { report.cleanup.capture_error = error.message; }
  }
  try {
    if (after === 'stop') { await harness.stop(); report.cleanup.stopped = true; }
    else if (after === 'pause' && (await harness.live()).state.running) { await harness.menu(); report.cleanup.paused = true; }
  } catch (error) { report.cleanup.completion_error = error.message; }
  report.valid = Boolean(report.native && report.presentation?.valid && report.errors.length === 0 && report.cleanup.inputs_released);
  report.finished_at = new Date().toISOString();
  await publishJson(report.artifacts.json, report);
  await fs.writeFile(report.artifacts.report, renderBenchmarkReport(report), { flag: 'wx' });
  await publish(report.valid ? 'completed' : 'failed', report.native?.elapsed_ms ?? 0);
  return report;
}
