// Portions Copyright 2026 The Hollycast Authors
import assert from 'node:assert/strict';
import * as fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { setTimeout as delay } from 'node:timers/promises';
import { runPlaytestPlan, validatePlaytestPlan } from './playtest.mjs';

const PROFILE_ID = 'planet-harriers-live-state-v1';

function liveSample({ gameId = 'pharrier', fresh = true, profileId = PROFILE_ID, mapped = true,
  mode = 3, energy = 8 } = {}) {
  return {
    state: { game_id: gameId, running: true, menu_open: false },
    telemetry: {
      sequence: 120,
      sample_available: fresh,
      sample_age_ms: fresh ? 10 : null,
      stale: !fresh,
      game_state: {
        status: mapped ? 'matched_operator_mapping' : 'identity_mismatch',
        profile_id: profileId,
        values: {
          main_mode: { value: mode, valid: mapped },
          player_energy: { value: energy, valid: mapped },
        },
      },
    },
  };
}

function plan(steps, overrides = {}) {
  return {
    schema: 'hollytest.playtest-plan.v1',
    id: 'planet-smoke',
    identity: { game_id: 'pharrier', profile_id: PROFILE_ID },
    timeout_ms: 5000,
    steps,
    ...overrides,
  };
}

async function fakeHarness(t, { initial = liveSample(), samples = [] } = {}) {
  const directory = await fs.mkdtemp(path.join(os.tmpdir(), 'hollytest-playtest-'));
  t.after(async () => fs.rm(directory, { recursive: true, force: true }));
  const calls = [];
  let sampleIndex = 0;
  let activeInput = false;
  let releases = 0;
  const fallback = liveSample();
  const nextSample = () => samples[sampleIndex++] ?? fallback;
  return {
    directory,
    manifest: { source_revision: 'f9fd0267', executable_sha256: 'a'.repeat(64), game_sha256: 'b'.repeat(64) },
    calls,
    get activeInput() { return activeInput; },
    get releases() { return releases; },
    async doctor() {
      calls.push({ op: 'doctor' });
      return { connected: true, pid: 123, game_id: 'pharrier', source_revision: 'f9fd0267',
        executable_sha256: 'a'.repeat(64), capabilities: ['controller', 'exclusive_controller_input', 'native_ui', 'live_telemetry', 'typed_memory_watches'],
        live: initial };
    },
    async verifySources() { calls.push({ op: 'verify' }); return { executable_and_seed_hashes_unchanged: true }; },
    async live() { calls.push({ op: 'live' }); return nextSample(); },
    async hold(state, milliseconds, { signal } = {}) {
      calls.push({ op: 'hold', state, milliseconds });
      activeInput = true;
      try { await delay(milliseconds, undefined, { signal }); }
      finally { await this.release(); }
    },
    async setController(state) { calls.push({ op: 'controller', state }); activeInput = true; return { lease_ms: 500 }; },
    async release() { calls.push({ op: 'release' }); activeInput = false; releases++; return {}; },
    async click(label) { calls.push({ op: 'click', label }); return liveSample(); },
    async observe({ screenshot = false } = {}) {
      calls.push({ op: 'observe', screenshot });
      return { ...liveSample(), screenshot: screenshot ? { file: 'screenshots/example.png', sha256: 'c'.repeat(64) } : null };
    },
    async ui(action, fields) { calls.push({ op: 'ui', action, fields }); return liveSample(); },
  };
}

test('validates the full plan before using the session', async () => {
  let touched = false;
  const invalid = plan([{ id: 'bad', op: 'shell', command: 'whoami' }]);
  const fake = {
    get directory() { touched = true; throw new Error('session should not be read'); },
    doctor() { touched = true; },
  };
  await assert.rejects(runPlaytestPlan(fake, invalid), /Unsupported playtest operation/);
  assert.equal(touched, false);

  const invalidUi = plan([{ id: 'bad-ui', op: 'ui', action: 'eval', code: '1+1' }]);
  assert.throws(() => validatePlaytestPlan(invalidUi), /Unsupported native UI action/);
  assert.equal(touched, false);
});

test('accepts native game identities with spaces and rejects control characters', () => {
  const value = plan([{ id: 'running', op: 'observe' }], { identity: { game_id: 'MAXIMUM SPEED' } });
  assert.equal(validatePlaytestPlan(value), value);
  for (const gameId of ['', 'bad\nidentity', `bad\u0000identity`]) {
    const invalid = plan([{ id: 'running', op: 'observe' }], { identity: { game_id: gameId } });
    assert.throws(() => validatePlaytestPlan(invalid), /identity\.game_id/);
  }
});

test('runs mapped assertions and actions, writes a bounded report, and releases input', async t => {
  const harness = await fakeHarness(t);
  const report = await runPlaytestPlan(harness, plan([
    { id: 'active-stage', op: 'observe', assertions: [
      { path: ['state', 'game_id'], op: 'eq', value: 'pharrier' },
      { path: ['telemetry', 'game_state', 'values', 'main_mode', 'value'], op: 'eq', value: 3 },
    ] },
    { id: 'fire', op: 'hold', state: { port: 0, buttons: ['A'] }, duration_ms: 1 },
  ]));

  assert.equal(report.outcome, 'PASS');
  assert.deepEqual(report.steps.map(step => step.status), ['pass', 'pass']);
  assert.deepEqual(report.cleanup.input_touched_ports, [0]);
  assert.equal(report.cleanup.input_released, true);
  assert.equal(harness.activeInput, false);
  assert.equal(harness.releases, 1);
  const saved = JSON.parse(await fs.readFile(path.join(report.report_directory, 'report.json'), 'utf8'));
  assert.equal(saved.identity.profile_id, PROFILE_ID);
  assert.equal(saved.steps[0].result.assertions[1].actual, 3);
});

test('stops after a failed assertion and marks later steps not-run', async t => {
  const harness = await fakeHarness(t);
  const report = await runPlaytestPlan(harness, plan([
    { id: 'press', op: 'press', button: 'A', duration_ms: 1 },
    { id: 'wrong-energy', op: 'observe', assertions: [
      { path: ['telemetry', 'game_state', 'values', 'player_energy', 'value'], op: 'gt', value: 100 },
    ] },
    { id: 'later-ui', op: 'click', label: 'Settings' },
  ]));

  assert.equal(report.outcome, 'FAIL');
  assert.deepEqual(report.steps.map(step => step.status), ['pass', 'fail', 'not_run']);
  assert.equal(harness.calls.some(call => call.op === 'click'), false);
  assert.equal(report.cleanup.input_released, true);
  assert.equal(harness.activeInput, false);
});

test('stale or unverified mapped data is inconclusive, never a pass', async t => {
  const harness = await fakeHarness(t, { samples: [liveSample({ fresh: false })] });
  const report = await runPlaytestPlan(harness, plan([
    { id: 'fresh-energy', op: 'observe', assertions: [
      { path: ['telemetry', 'game_state', 'values', 'player_energy', 'value'], op: 'gt', value: 0 },
    ] },
    { id: 'later', op: 'press', button: 'A', duration_ms: 1 },
  ]));

  assert.equal(report.outcome, 'INCONCLUSIVE');
  assert.deepEqual(report.steps.map(step => step.status), ['fail', 'not_run']);
  assert.equal(harness.calls.some(call => call.op === 'hold'), false);

  const unverified = await fakeHarness(t, { initial: liveSample({ mapped: false }) });
  const profileReport = await runPlaytestPlan(unverified, plan([{ id: 'never-run', op: 'press', button: 'A' }]));
  assert.equal(profileReport.outcome, 'INCONCLUSIVE');
  assert.deepEqual(profileReport.steps.map(step => step.status), ['not_run']);
  assert.equal(unverified.calls.some(call => call.op === 'hold'), false);

  const missingField = liveSample();
  delete missingField.telemetry.game_state.values.player_energy;
  const missing = await fakeHarness(t, { initial: missingField });
  const missingReport = await runPlaytestPlan(missing, plan([
    { id: 'must-not-act', op: 'press', button: 'A', duration_ms: 1 },
    { id: 'energy', op: 'observe', assertions: [
      { path: ['telemetry', 'game_state', 'values', 'player_energy', 'value'], op: 'gt', value: 0 },
    ] },
  ]));
  assert.equal(missingReport.outcome, 'INCONCLUSIVE');
  assert.deepEqual(missingReport.steps.map(step => step.status), ['not_run', 'not_run']);
  assert.equal(missing.calls.some(call => call.op === 'hold'), false);
});

test('wait_for passes only after fresh observations satisfy every assertion', async t => {
  const harness = await fakeHarness(t, { samples: [liveSample({ mode: 1 }), liveSample({ mode: 3 })] });
  const report = await runPlaytestPlan(harness, plan([
    { id: 'wait-active', op: 'wait_for', timeout_ms: 500, interval_ms: 50, assertions: [
      { path: ['telemetry', 'game_state', 'values', 'main_mode', 'value'], op: 'eq', value: 3 },
    ] },
  ], { timeout_ms: 1000 }));

  assert.equal(report.outcome, 'PASS');
  assert.equal(report.steps[0].result.polls, 2);
});

test('wait timeout fails without running later actions', async t => {
  const harness = await fakeHarness(t, { samples: [liveSample({ mode: 1 }), liveSample({ mode: 1 })] });
  const report = await runPlaytestPlan(harness, plan([
    { id: 'wait-active', op: 'wait_for', timeout_ms: 100, interval_ms: 50, assertions: [
      { path: ['telemetry', 'game_state', 'values', 'main_mode', 'value'], op: 'eq', value: 3 },
    ] },
    { id: 'later', op: 'press', button: 'A', duration_ms: 1 },
  ], { timeout_ms: 1000 }));

  assert.equal(report.outcome, 'FAIL');
  assert.deepEqual(report.steps.map(step => step.status), ['fail', 'not_run']);
  assert.equal(harness.calls.some(call => call.op === 'hold'), false);
});

test('a passing observation arriving after the step deadline cannot pass the wait', async t => {
  const harness = await fakeHarness(t);
  harness.live = async () => {
    await delay(200);
    return liveSample();
  };
  const report = await runPlaytestPlan(harness, plan([
    { id: 'late-observation', op: 'wait_for', timeout_ms: 100, interval_ms: 50,
      assertions: [{ path: ['state', 'running'], op: 'is_true' }] },
    { id: 'later', op: 'press', button: 'A', duration_ms: 1 },
  ]));
  assert.equal(report.outcome, 'FAIL');
  assert.deepEqual(report.steps.map(step => step.status), ['fail', 'not_run']);
  assert.equal(harness.calls.some(call => call.op === 'hold'), false);
});

test('missing requested screenshot is inconclusive and prevents later input', async t => {
  const harness = await fakeHarness(t);
  harness.observe = async () => ({ screenshot: null });
  const report = await runPlaytestPlan(harness, plan([
    { id: 'capture', op: 'screenshot' },
    { id: 'later', op: 'press', button: 'A', duration_ms: 1 },
  ]));
  assert.equal(report.outcome, 'INCONCLUSIVE');
  assert.deepEqual(report.steps.map(step => step.status), ['fail', 'not_run']);
  assert.equal(harness.calls.some(call => call.op === 'hold'), false);
});

test('cancellation during a hold waits for the writer to release before reporting', async t => {
  const harness = await fakeHarness(t);
  const cancellation = new AbortController();
  const timer = setTimeout(() => cancellation.abort(new Error('Playtest cancelled')), 30);
  let report;
  try {
    report = await runPlaytestPlan(harness, plan([
      { id: 'long-hold', op: 'hold', state: { port: 2, buttons: ['A'] }, duration_ms: 5000 },
      { id: 'later', op: 'press', button: 'B', duration_ms: 1 },
    ], { timeout_ms: 10000 }), { signal: cancellation.signal });
  } finally {
    clearTimeout(timer);
  }

  assert.equal(report.outcome, 'INCONCLUSIVE');
  assert.deepEqual(report.steps.map(step => step.status), ['fail', 'not_run']);
  assert.deepEqual(report.cleanup.input_touched_ports, [2]);
  assert.equal(report.cleanup.input_released, true);
  assert.equal(harness.activeInput, false);
  assert.equal(harness.releases, 2);
});
