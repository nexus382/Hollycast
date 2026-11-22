// Portions Copyright 2026 The Hollycast Authors
import { test } from 'node:test';
import assert from 'node:assert/strict';
import * as fs from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import { setTimeout as delay } from 'node:timers/promises';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { fileURLToPath } from 'node:url';
import { HollycastHarness, controllerState, summarizePerformance, agentInvocation,
  validateSettingsPage, validateUiTarget, validateUiClickOptions, validateMappingArgs, validateRewindArgs, validateUiExpectedFields } from './hollytest.mjs';
import { runBenchmark, summarizeOsd, renderBenchmarkReport, benchmarkSettings, auditConditions } from './benchmark.mjs';

const capture = () => ({ domain: 'host_present_interval_ms', samples_ms: [10, 20, 30, 40],
  valid: true, invalid_reasons: [], observations_during_capture: 0 });

async function fixture(t, extra = {}) {
  const parent = path.resolve(process.env.HOLLYTEST_TEST_ROOT || os.tmpdir());
  const directory = await fs.mkdtemp(path.join(parent, 'hollytest-'));
  t.after(async () => {
    // Delete only the directory created by this fixture, never a caller's run.
    assert.equal(path.dirname(directory), parent);
    assert.ok(path.basename(directory).startsWith('hollytest-'));
    await fs.rm(directory, { recursive: true, force: true });
  });
  const executable = path.join(directory, 'Hollycast.exe');
  await fs.writeFile(executable, 'test fixture, not an executable');
  const harness = await HollycastHarness.prepare({ executable, out: path.join(directory, 'run'), ...extra });
  return { directory, harness, executable };
}

async function fakeBridge(t, harness, handler = async () => ({}), transform = response => response) {
  let running = true;
  const requests = [];
  let failure;
  const loop = (async () => {
    while (running) {
      try {
        const file = path.join(harness.directory, 'command.json');
        let bytes;
        try { bytes = await fs.readFile(file, 'utf8'); }
        catch (error) { if (error.code !== 'ENOENT') throw error; }
        if (bytes) {
          const request = JSON.parse(bytes);
          requests.push(request);
          await fs.unlink(file);
          const result = await handler(request);
          const response = transform({ version: 1, session: request.session, id: request.id, ok: true, result }, request);
          const temporary = path.join(harness.directory, `fake-${request.id}.tmp`);
          await fs.writeFile(temporary, JSON.stringify(response), { flag: 'wx' });
          await fs.rename(temporary, path.join(harness.directory, `response-${request.id}.json`));
        }
        await delay(2);
      } catch (error) { failure = error; running = false; }
    }
  })();
  t.after(async () => { running = false; await loop; if (failure) throw failure; });
  return requests;
}

async function fakeUiBridge(t, harness, overrides = {}) {
  const state = { page: 'video', focused: false, password: false, readOnly: false, value: 'old text', value_type: undefined,
    readbackOverride: undefined, toggle: undefined, changeToggle: true, scroll: [0, 0], loseFocusOnSelectAll: false, updateText: true, failText: false, ...overrides };
  const itemId = 73;
  const inspection = request => {
    const page = overrides.mismatchPage ? (state.page === 'video' ? 'audio' : 'video') : state.page;
    return { observation_id: request.id, wall_ms: Date.now(), screenshot: null, state: { settings_page: page },
    ui: { settings_page: page, active_item_id: state.focused ? itemId : 0,
      text_input: state.focused ? { active_item_id: itemId, window_id: 1, window: 'Settings', password: state.password } : null,
      windows: [{ id: 1, name: 'Settings', rect: [0, 0, 800, 600], inner_rect: [10, 20, 790, 590],
        scroll: [...state.scroll], scroll_max: [0, 100] }],
      items: [{ id: itemId, label: 'Friendly label', raw_label: 'Raw label', semantic_id: 'setting.example',
        semantic_label: 'Friendly label', window: 'Settings', window_id: 1, visible: true, disabled: false,
        checkable: state.toggle !== undefined, checked: state.toggle, kind: state.toggle !== undefined ? 'toggle' : 'text',
        ...(state.password ? {} : { value: state.readbackOverride === undefined
          ? (state.toggle !== undefined ? state.toggle : state.value) : state.readbackOverride }),
        ...(state.value_type === undefined ? {} : { value_type: state.value_type }),
        inputable: state.toggle === undefined, read_only: state.readOnly, password: state.password,
        active: state.focused, text_input_focused: state.focused }] } };
  };
  const requests = await fakeBridge(t, harness, async request => {
    if (request.op === 'settings') {
      state.page = request.args.page;
      return { settings_page: request.args.page };
    }
    if (request.op === 'inspect') return inspection(request);
    if (request.op !== 'ui') return {};
    const { action, key, item_id: selected, delta_y: deltaY = 0 } = request.args;
    if (action === 'click' && selected === itemId) {
      if (state.toggle !== undefined) { if (state.changeToggle) state.toggle = !state.toggle; }
      else state.focused = true;
    } else if (action === 'key' && key === 'Ctrl+A') {
      if (state.loseFocusOnSelectAll) state.focused = false;
      else if (!state.password) state.value = '';
    } else if (action === 'key' && key === 'Backspace') {
      if (!state.password) state.value = '';
    } else if (action === 'text' && state.updateText && !state.password)
      state.value = state.value_type === 'float32' ? Math.fround(Number(request.args.text))
        : state.value_type === 'int32' ? Number(request.args.text) : request.args.text;
    else if (action === 'key' && key === 'Enter') state.focused = false;
    else if (action === 'scroll') state.scroll[1] = Math.max(0, Math.min(100, state.scroll[1] + deltaY));
    return {};
  }, (response, request) => state.failText && request.op === 'ui' && request.args.action === 'text'
    ? { ...response, ok: false, error: `echo:${request.args.text}` } : response);
  return { state, requests };
}

test('settings, UI selectors, vectors, and mapping inputs validate exact contracts', () => {
  assert.equal(validateSettingsPage('advanced'), 'advanced');
  for (const page of ['Settings', 'render', '', 3]) assert.throws(() => validateSettingsPage(page));
  assert.deepEqual(validateUiTarget({ semantic_id: 'setting.example', window: 'Settings' }),
    { semantic_id: 'setting.example', window: 'Settings' });
  for (const target of [{ label: 'A', item_id: 1 }, { semantic_id: 'x'.repeat(129) }, { label: 'x', typo: true }])
    assert.throws(() => validateUiTarget(target));
  assert.deepEqual(validateUiClickOptions({ modifiers: ['Ctrl'], mouse_button: 'middle', click_count: 2 }),
    { modifiers: ['Ctrl'], mouse_button: 'middle', click_count: 2 });
  assert.throws(() => validateUiClickOptions({ modifiers: ['Ctrl', 'Ctrl'] }));
  assert.deepEqual(validateMappingArgs({ action: 'axis', code: 65535, value: -32768 }), { action: 'axis', code: 65535, value: -32768 });
  for (const bad of [{ action: 'button', code: true, pressed: true }, { action: 'button', code: 1, pressed: 1 },
    { action: 'axis', code: 1, value: 32768 }, { action: 'create', extra: 1 }]) assert.throws(() => validateMappingArgs(bad));
  assert.deepEqual(validateUiExpectedFields({ kind: 'vector2', value: [1, 2] }), { kind: 'vector2', value: [1, 2] });
  assert.throws(() => validateUiExpectedFields({ value: [[1], [2]] }));
  assert.deepEqual(validateRewindArgs({ held: true }), { held: true });
  for (const bad of [{}, { held: 'true' }, { held: 1 }, { held: true, extra: 1 }, [], null])
    assert.throws(() => validateRewindArgs(bad));
});

test('rewind profile keys are written and the native hold round-trips', async t => {
  const { harness } = await fixture(t, { profile: { config: {
    'Dreamcast.RewindEnable': true, 'Dreamcast.RewindInterval': 250,
    'Dreamcast.RewindCompression': 2, 'Dreamcast.RewindDuration': 10, 'rend.ThreadedRendering': false } } });
  const config = await fs.readFile(path.join(harness.directory, 'home', 'emu.cfg'), 'utf8');
  for (const line of ['Dreamcast.RewindEnable = yes', 'Dreamcast.RewindInterval = 250',
    'Dreamcast.RewindCompression = 2', 'Dreamcast.RewindDuration = 10'])
    assert.ok(config.includes(line), `missing profile line: ${line}`);

  await fs.writeFile(path.join(harness.directory, 'ready.json'), JSON.stringify(
    { version: 1, session: harness.manifest.session, pid: 1, capabilities: ['controller'] }));
  await assert.rejects(harness.rewind({ held: true }), /does not expose rewind/);

  await fs.writeFile(path.join(harness.directory, 'ready.json'), JSON.stringify(
    { version: 1, session: harness.manifest.session, pid: 1, capabilities: ['rewind'] }));
  let heldByHarness = false;
  const requests = await fakeBridge(t, harness, async request => {
    if (request.op !== 'rewind') return {};
    heldByHarness = request.args.held;
    return { enabled: true, held_by_harness: heldByHarness, snapshots: heldByHarness ? 3 : 4 };
  });
  const held = await harness.rewind({ held: true });
  assert.equal(held.held_by_harness, true);
  const released = await harness.rewind({ held: false });
  assert.equal(released.held_by_harness, false);
  assert.deepEqual(requests.filter(request => request.op === 'rewind').map(request => request.args),
    [{ held: true }, { held: false }]);
});

test('settings page command checks fresh readback when exposed', async t => {
  const { harness } = await fixture(t);
  const { requests } = await fakeUiBridge(t, harness);
  const result = await harness.settings('video');
  assert.equal(result.page, 'video');
  assert.equal(result.observation.ui.settings_page, 'video');
  assert.deepEqual(requests.filter(request => ['settings', 'inspect'].includes(request.op)).map(request => request.op), ['settings', 'inspect']);

  const mismatch = await fixture(t);
  await fakeUiBridge(t, mismatch.harness, { mismatchPage: true });
  await assert.rejects(mismatch.harness.settings('video'), /page readback mismatch/);
});

test('fill verifies readable fields while redacting text action payloads', async t => {
  const { harness } = await fixture(t);
  const { requests } = await fakeUiBridge(t, harness);
  const result = await harness.fill({ semantic_id: 'setting.example' }, 'replacement-value');
  assert.equal(result.outcome, 'PASS');
  assert.equal(result.verification, 'native_value_readback');
  assert.equal(result.text_length, Buffer.byteLength('replacement-value'));
  assert.deepEqual(requests.filter(request => request.op === 'ui').map(request => request.args.action),
    ['click', 'key', 'text', 'key']);
  const events = await fs.readFile(path.join(harness.directory, 'events.jsonl'), 'utf8');
  const observations = await fs.readFile(path.join(harness.directory, 'observations.jsonl'), 'utf8');
  const textEvent = events.split('\n').filter(Boolean).map(line => JSON.parse(line))
    .find(event => event.op === 'ui' && event.args?.action === 'text');
  assert.equal(textEvent.args.text, '[redacted]');
  assert.equal(textEvent.args.text_redacted, true);
  assert.equal(textEvent.result.text_redacted, true);
  assert.equal(textEvent.error, null);
  assert.equal(observations.includes('replacement-value'), false);
  assert.equal(events.includes('replacement-value'), false);
});

test('fill clears empty values, rejects read-only items, and stops when focus changes', async t => {
  const cleared = await fixture(t);
  const bridge = await fakeUiBridge(t, cleared.harness);
  const result = await cleared.harness.fill({ label: 'Friendly label' }, '');
  assert.equal(result.outcome, 'PASS');
  assert.equal(bridge.state.value, '');
  assert.ok(bridge.requests.some(request => request.op === 'ui' && request.args.key === 'Backspace'));

  const readOnly = await fixture(t);
  const readOnlyBridge = await fakeUiBridge(t, readOnly.harness, { readOnly: true });
  await assert.rejects(readOnly.harness.fill('Friendly label', 'x'), /writable/);
  assert.equal(readOnlyBridge.requests.some(request => request.op === 'ui'), false);

  const lost = await fixture(t);
  const lostBridge = await fakeUiBridge(t, lost.harness, { loseFocusOnSelectAll: true });
  const lostResult = await lost.harness.fill('Friendly label', 'should-not-type');
  assert.equal(lostResult.outcome, 'INCONCLUSIVE');
  assert.deepEqual(lostBridge.requests.filter(request => request.op === 'ui').map(request => request.args.action), ['click', 'key']);
});

test('password fill is write-only and raw native text errors never echo payloads', async t => {
  const { harness } = await fixture(t);
  const { state } = await fakeUiBridge(t, harness, { password: true, value: undefined });
  const result = await harness.fill('Friendly label', 'password-fixture-only');
  assert.equal(result.outcome, 'PASS');
  assert.equal(result.verification, 'write_only');
  assert.equal(Object.hasOwn((await harness.observe()).ui.items[0], 'value'), false);
  assert.equal(state.password, true);
  for (const name of ['events.jsonl', 'observations.jsonl'])
    assert.equal((await fs.readFile(path.join(harness.directory, name), 'utf8')).includes('password-fixture-only'), false);

  const failed = await fixture(t);
  await fakeUiBridge(t, failed.harness, { failText: true });
  await failed.harness.observe();
  const secret = 'bridge-error-secret';
  await assert.rejects(failed.harness.fill('Friendly label', secret), /Native UI text action rejected/);
  const events = await fs.readFile(path.join(failed.harness.directory, 'events.jsonl'), 'utf8');
  assert.equal(events.includes(secret), false);
  const textEvent = events.split('\n').filter(Boolean).map(line => JSON.parse(line))
    .find(event => event.op === 'ui' && event.args?.action === 'text');
  assert.ok(textEvent);
  assert.equal(await fs.stat(path.join(failed.harness.directory, `response-${textEvent.id}.json`)).then(() => true, () => false), false);

  const echo = await fixture(t);
  await fakeBridge(t, echo.harness, async request => ({ echo: request.args.text ?? null }));
  const echoSecret = 'response-secret-fixture';
  assert.deepEqual(await echo.harness.request('ui', { action: 'text', text: echoSecret }), { text_redacted: true });
  assert.equal((await fs.readdir(echo.harness.directory)).some(name => name.startsWith('response-')), false);
  assert.equal((await fs.readFile(path.join(echo.harness.directory, 'events.jsonl'), 'utf8')).includes(echoSecret), false);
});

test('float32 fill readback uses native rounding while integer readback remains exact', async t => {
  const float = await fixture(t);
  const floatBridge = await fakeUiBridge(t, float.harness, { value: 0.25, value_type: 'float32' });
  const entered = '0.333';
  const floatResult = await float.harness.fill('Friendly label', entered);
  assert.equal(floatResult.outcome, 'PASS');
  assert.equal(floatBridge.state.value, Math.fround(Number(entered)));

  const integer = await fixture(t);
  await fakeUiBridge(t, integer.harness, { value: 0, value_type: 'int32', readbackOverride: 1 });
  const integerResult = await integer.harness.fill('Friendly label', '0.333');
  assert.equal(integerResult.outcome, 'FAIL');
  assert.equal(integerResult.reason, 'text_readback_mismatch');
});
test('toggle is idempotent and scroll uses fresh window geometry', async t => {
  const { harness } = await fixture(t);
  const bridge = await fakeUiBridge(t, harness, { toggle: false });
  assert.equal((await harness.setToggle('Friendly label', true)).changed, true);
  assert.equal((await harness.setToggle('Friendly label', true)).changed, false);
  assert.equal(bridge.requests.filter(request => request.op === 'ui' && request.args.action === 'click').length, 1);

  const mismatch = await fixture(t);
  const noChange = await fakeUiBridge(t, mismatch.harness, { toggle: false, changeToggle: false });
  assert.equal((await mismatch.harness.setToggle('Friendly label', true)).outcome, 'FAIL');
  assert.equal(noChange.requests.filter(request => request.op === 'ui' && request.args.action === 'click').length, 1);

  const scroll = await fixture(t);
  const scrollBridge = await fakeUiBridge(t, scroll.harness);
  const result = await scroll.harness.scrollWindow('Settings', 4);
  const action = scrollBridge.requests.find(request => request.op === 'ui' && request.args.action === 'scroll');
  assert.deepEqual([action.args.x, action.args.y, action.args.delta_y], [400, 305, 4]);
  assert.equal(result.after[1], 4);
});

test('a queued or lock-delayed UI request expires before publishing input', async t => {
  const queued = await fixture(t);
  let releaseQueue;
  queued.harness.queue = new Promise(resolve => { releaseQueue = resolve; });
  const deadline = Date.now() + 40;
  const pending = queued.harness.request('ui', { action: 'text', text: 'queued-secret' }, { deadline });
  await delay(60);
  releaseQueue();
  await assert.rejects(pending, /timeout/i);
  await assert.rejects(fs.access(path.join(queued.harness.directory, 'command.json')));

  const locked = await fixture(t);
  await fs.mkdir(path.join(locked.harness.directory, 'client.lock'));
  const lockPending = locked.harness.request('ui', { action: 'text', text: 'lock-secret' }, { deadline: Date.now() + 40 });
  await assert.rejects(lockPending, /timeout|lock is busy/i);
  await fs.rmdir(path.join(locked.harness.directory, 'client.lock'));
  await assert.rejects(fs.access(path.join(locked.harness.directory, 'command.json')));
});

test('controller snapshots preserve simultaneous buttons, both sticks and analog triggers', () => {
  assert.deepEqual(controllerState({ port: 3, buttons: ['A', 'START'], axes: [-32767, 32767, 100, -100],
    triggers: [32768, 65535], leaseMs: 2000 }), { port: 3, buttons: ['A', 'START'], axes: [-32767, 32767, 100, -100],
    triggers: [32768, 65535], lease_ms: 2000 });
});

test('invalid controller states are rejected before transport', () => {
  for (const bad of [{ port: 4 }, { port: -1 }, { leaseMs: 0 }, { leaseMs: 2001 }, { buttons: ['A', 'A'] },
    { buttons: ['fake'] }, { axes: [0, 0] }, { axes: [32768, 0, 0, 0] }, { axes: [0.5, 0, 0, 0] },
    { triggers: [0, 65536] }, { triggers: [true, 0] }, null, [], { typo: 1 }]) assert.throws(() => controllerState(bad));
});

test('performance summary uses measured samples and never invents a comparison verdict', () => {
  const summary = summarizePerformance(capture());
  assert.equal(summary.mean_ms, 25);
  assert.equal(summary.median_ms, 20);
  assert.equal(summary.p99_ms, 40);
  assert.equal(summary.presented_intervals_per_second, 40);
  assert.equal(summary.gpu_timing_measured, false);
  assert.equal(summary.physical_latency_measured, false);
});

test('invalid native captures remain invalid and malformed metrics cannot pass', () => {
  assert.equal(summarizePerformance({ ...capture(), valid: false, invalid_reasons: ['gameplay_interrupted'] }).valid, false);
  assert.equal(summarizePerformance({ ...capture(), invalid_reasons: ['changed'] }).valid, false);
  for (const samples_ms of [[], [1], [1, 0], [1, -1], [1, NaN], [1, Infinity]])
    assert.throws(() => summarizePerformance({ ...capture(), samples_ms }));
  assert.throws(() => summarizePerformance({ ...capture(), domain: 'gpu_time_ms' }));
});

test('preparation creates isolated roots and preserves source hashes', async t => {
  const { harness } = await fixture(t);
  assert.equal((await fs.stat(path.join(harness.directory, 'home', 'data'))).isDirectory(), true);
  const ini = await fs.readFile(path.join(harness.directory, 'home', 'emu.cfg'), 'utf8');
  assert.match(ini, /Dreamcast.AutoSaveState = no/);
  assert.match(ini, /UploadCrashLogs = no/);
  assert.match(ini, /\[updates\]\s+startup = no/);
  assert.match(ini, /Enable = no/);
  assert.equal((await harness.verifySources()).executable_and_seed_hashes_unchanged, true);
  await assert.rejects(HollycastHarness.prepare({ executable: harness.manifest.executable, out: harness.directory }), /must be new/);
});

test('startup errors reject cleanly without an unhandled process error', async t => {
  const { harness } = await fixture(t);
  await assert.rejects(harness.launch({ timeoutMs: 1000, visible: false }));
});

test('a launched process remains controllable after the launcher exits', async t => {
  const { directory } = await fixture(t);
  const fakeGame = path.join(directory, 'fake-native.cjs');
  await fs.writeFile(fakeGame, `
    const fs = require('node:fs');
    const path = require('node:path');
    const root = process.env.HOLLYCAST_HARNESS_DIR;
    const session = process.env.HOLLYCAST_HARNESS_SESSION;
    const publish = (name, value) => {
      fs.writeFileSync(path.join(root, name + '.tmp'), JSON.stringify(value));
      fs.renameSync(path.join(root, name + '.tmp'), path.join(root, name));
    };
    fs.mkdirSync(path.join(root, 'emulator.lock'));
    publish('ready.json', { version: 1, session, pid: process.pid, capabilities: [] });
    const timer = setInterval(() => {
      const file = path.join(root, 'command.json');
      if (!fs.existsSync(file)) return;
      const request = JSON.parse(fs.readFileSync(file, 'utf8'));
      fs.unlinkSync(file);
      publish('response-' + request.id + '.json', { version: 1, session, id: request.id, ok: true, result: { alive: true } });
      if (request.op === 'quit') { clearInterval(timer); process.exitCode = 0; }
    }, 5);
    // A failed parent test must not leave a fixture process behind indefinitely.
    setTimeout(() => process.exit(0), 15000).unref();
  `);
  const launcher = path.join(directory, 'launcher.mjs');
  const out = path.join(directory, 'detached-run');
  const module = new URL('./hollytest.mjs', import.meta.url).href;
  await fs.writeFile(launcher, `
    import { HollycastHarness } from ${JSON.stringify(module)};
    const harness = await HollycastHarness.prepare({ executable: process.execPath, out: process.argv[2], game: process.argv[3] });
    await harness.launch({ visible: false, timeoutMs: 5000 });
  `);
  await promisify(execFile)(process.execPath, [launcher, out, fakeGame], { windowsHide: true, timeout: 10000 });
  const harness = await HollycastHarness.attach(out);
  try { assert.deepEqual(await harness.request('observe'), { alive: true }); }
  finally { await harness.stop(); }
});

test('profile overrides produce one section and reject paths or secrets', async t => {
  const { directory, executable } = await fixture(t);
  const harness = await HollycastHarness.prepare({ executable, out: path.join(directory, 'profile'),
    profile: { config: { 'rend.ShowFPS': false, 'pvr.rend': 0 }, rend: { ResourceMonitorMode: 2 } } });
  const ini = await fs.readFile(path.join(harness.directory, 'home', 'emu.cfg'), 'utf8');
  assert.equal(ini.match(/\[config\]/g).length, 1);
  assert.match(ini, /rend.ShowFPS = no/);
  assert.match(ini, /ResourceMonitorMode = 2/);
  for (const profile of [{ config: { LuaFileName: 'x.lua' } }, { config: { 'rend.ShowFPS': 'yes' } }, { unknown: {} }])
    await assert.rejects(HollycastHarness.prepare({ executable, out: path.join(directory, 'rejected'), profile }));
  await assert.rejects(fs.stat(path.join(directory, 'rejected')), { code: 'ENOENT' });
});

test('firmware seeding copies only supported files and detects original mutation', async t => {
  const { directory, executable } = await fixture(t);
  const seeds = path.join(directory, 'seeds');
  await fs.mkdir(seeds);
  const expectedSeeds = new Map([
    ['vmu_save_A1.bin', Buffer.alloc(128 * 1024, 5)],
    ['dc_flash.bin', Buffer.alloc(128 * 1024, 6)],
    ['naomi.zip', Buffer.from('fixture Naomi BIOS archive')],
    ['awbios.7z', Buffer.from('fixture Atomiswave BIOS archive')],
    ['segasp.zip', Buffer.from('fixture SystemSP BIOS archive')],
  ]);
  for (const [name, contents] of expectedSeeds) await fs.writeFile(path.join(seeds, name), contents);
  const original = path.join(seeds, 'vmu_save_A1.bin');
  await fs.writeFile(path.join(seeds, 'private.txt'), 'do not copy');
  const harness = await HollycastHarness.prepare({ executable, out: path.join(directory, 'seeded'), firmwareDirectory: seeds });
  const data = path.join(harness.directory, 'home', 'data');
  assert.deepEqual(harness.manifest.seeds.map(seed => path.basename(seed.source)).sort(), [...expectedSeeds.keys()].sort());
  assert.deepEqual((await fs.readdir(data)).sort(), [...expectedSeeds.keys()].sort());
  for (const [name, contents] of expectedSeeds) assert.deepEqual(await fs.readFile(path.join(data, name)), contents);
  await assert.rejects(fs.stat(path.join(data, 'private.txt')), { code: 'ENOENT' });
  await fs.writeFile(path.join(data, 'vmu_save_A1.bin'), Buffer.alloc(128 * 1024, 7));
  await harness.verifySources();
  assert.equal((await fs.readFile(original))[0], 5);
  await fs.writeFile(original, Buffer.alloc(128 * 1024, 8));
  await assert.rejects(harness.verifySources(), /Original inputs changed/);
});

test('transport binds responses to the session and serializes concurrent requests', async t => {
  const { harness } = await fixture(t);
  const requests = await fakeBridge(t, harness, async request => ({ operation: request.op }));
  const result = await Promise.all([harness.request('observe'), harness.request('release')]);
  assert.deepEqual(result, [{ operation: 'observe' }, { operation: 'release' }]);
  assert.equal(requests.length, 2);
  assert.equal(requests[0].version, 1);
  assert.equal(requests[0].session, harness.manifest.session);
  assert.notEqual(requests[0].id, requests[1].id);
  assert.ok(requests[0].expires_ms > 0);
});

test('unknown outcome is retained and a timed out command is not retried', async t => {
  const { harness } = await fixture(t);
  await assert.rejects(harness.request('controller', {}, { timeoutMs: 30 }), /outcome is unknown/);
  const pending = JSON.parse(await fs.readFile(path.join(harness.directory, 'command.json'), 'utf8'));
  await assert.rejects(harness.request('observe'), /earlier command is still pending/);
  assert.equal(JSON.parse(await fs.readFile(path.join(harness.directory, 'command.json'), 'utf8')).id, pending.id);
});

test('mismatched response identities are rejected', async t => {
  const { harness } = await fixture(t);
  await fakeBridge(t, harness, async () => ({}), response => ({ ...response, session: '0'.repeat(32) }));
  await assert.rejects(harness.request('observe'), /identity mismatch/);
});

test('native rejection is preserved and a subsequent command can proceed', async t => {
  const { harness } = await fixture(t);
  await fakeBridge(t, harness, async () => ({}), (response, request) => request.op === 'memory'
    ? { ...response, ok: false, error: 'RAM range rejected' } : response);
  await assert.rejects(harness.readMemory(-1, 32), /RAM range rejected/);
  assert.deepEqual(await harness.request('observe'), {});
});

test('cross-process exclusion never steals an existing client lock', async t => {
  const { harness } = await fixture(t);
  await fs.mkdir(path.join(harness.directory, 'client.lock'));
  await assert.rejects(harness.request('observe', {}, { timeoutMs: 30 }), /lock is busy/);
  assert.equal((await fs.stat(path.join(harness.directory, 'client.lock'))).isDirectory(), true);
});

test('independent monitoring and controller clients wait for transport ownership', async t => {
  const { harness } = await fixture(t);
  const second = await HollycastHarness.attach(harness.directory);
  const requests = await fakeBridge(t, harness, async () => { await delay(30); return {}; });
  await Promise.all([harness.live(), second.setController({ axes: [12000, 0, 0, 0] })]);
  assert.equal(requests.length, 2);
  assert.deepEqual(new Set(requests.map(request => request.op)), new Set(['live', 'controller']));
  assert.notEqual(requests[0].id, requests[1].id);
});

test('OSD aggregation excludes duplicate, stale, unavailable, and unsupported readings', () => {
  const sample = (stamp, age, values = {}) => ({ telemetry: { osd: { collecting: true,
    sampled_at_monotonic_ms: stamp, sample_age_ms: age, game_fps: 60, gpu_busy_percent: null, ...values } } });
  const summary = summarizeOsd([sample(1000, 10), sample(1000, 20), sample(2000, 3000), sample(3000, 10, { game_fps: 40 }),
    { telemetry: { osd: { collecting: false } } }, sample(4000, 0, { game_fps: NaN })]);
  assert.equal(summary.metrics.game_fps.mean, 50);
  assert.equal(summary.metrics.game_fps.count, 2);
  assert.equal(summary.duplicate_samples, 1);
  assert.equal(summary.stale_samples, 1);
  assert.equal(summary.unavailable_samples, 1);
  assert.equal(summary.metrics.gpu_busy_percent.mean, null);
});

test('condition audit retains transient resizing and setting changes even after restoration', () => {
  const initial = { settings: { renderer: 0, vsync: true }, display: { width: 1200, height: 900 } };
  const sample = (elapsed_ms, state) => ({ elapsed_ms, state });
  assert.equal(auditConditions([sample(0, initial)], initial).conditions_stable, true);
  const audit = auditConditions([sample(0, initial), sample(1000, { ...initial, display: { width: 1400, height: 900 } }),
    sample(2000, { ...initial, settings: { renderer: 0, vsync: false } }), sample(3000, initial)], initial);
  assert.equal(audit.conditions_stable, false);
  assert.deepEqual(audit.condition_changes.map(change => change.elapsed_ms), [1000, 2000, 3000]);
  assert.equal(audit.warnings.length, 1);
});

async function benchmarkFixture(t, { playing = true, nativeValid = true, sourceMutation = false, interrupt = false } = {}) {
  const fixtureState = await fixture(t);
  const { harness, executable } = fixtureState;
  await fs.writeFile(path.join(harness.directory, 'ready.json'), JSON.stringify({ version: 1, session: harness.manifest.session,
    pid: 123, capabilities: ['controller', 'exclusive_controller_input', 'exclusive_ui_input', 'native_ui', 'live_telemetry', 'typed_memory_watches', 'benchmark_frame_counts'] }));
  let recording = false, sequence = 100, mode = playing ? 3 : 0;
  const state = () => ({ state: { running: true, menu_open: false, game_id: 'pharrier', input_restricted: false,
    exclusive_controller_input: true, settings: { renderer: 0 }, display: { width: 640, height: 480 } },
  telemetry: { sample_available: true, stale: false, sample_age_ms: 10, sequence: sequence++, machine: { available: true },
    osd: { collecting: true, sampled_at_monotonic_ms: sequence * 1000, sample_age_ms: 10, game_fps: 60 },
    game_state: { profile_id: 'test', status: 'matched_operator_mapping', values: { main_mode: { value: mode, valid: true },
      sub_mode_raw: { value: 4, valid: true }, player_energy: { value: 30, valid: true } } } } });
  const requests = await fakeBridge(t, harness, async request => {
    if (request.op === 'performance_begin') { recording = true; return {}; }
    if (request.op === 'performance_end') {
      if (sourceMutation) await fs.writeFile(executable, 'unexpected replacement');
      return { ...capture(), valid: nativeValid, invalid_reasons: nativeValid ? [] : ['gameplay_interrupted'],
        elapsed_ms: 150, guest_frame_boundary_count: 9, guest_frame_boundary_hz: 60, guest_frame_boundary_source: 'hikaru_engine_frame' };
    }
    if (request.op === 'controller' && mode === 0 && request.args.buttons.includes('START')) mode = 1;
    else if (request.op === 'controller' && mode === 1 && request.args.buttons.includes('A')) mode = 3;
    if (request.op === 'live') {
      const result = state();
      if (interrupt && recording) result.state.menu_open = true;
      return result;
    }
    return {};
  });
  return { ...fixtureState, requests, state };
}

test('benchmark performs native startup, excludes it from capture, and retains a readable report', async t => {
  const { harness, requests } = await benchmarkFixture(t, { playing: false });
  const report = await runBenchmark(harness, { durationMs: 100, warmupMs: 0, scenario: 'planet-harriers', control: 'scripted', after: 'leave' });
  assert.equal(report.valid, true);
  const begin = requests.findIndex(request => request.op === 'performance_begin');
  const coin = requests.findIndex(request => request.op === 'controller' && request.args.buttons.includes('D'));
  const start = requests.findIndex(request => request.op === 'controller' && request.args.buttons.includes('START'));
  const select = requests.findIndex(request => request.op === 'controller' && request.args.buttons.includes('A'));
  assert.ok(coin >= 0 && coin < start && start < select && select < begin);
  assert.equal(requests.some(request => ['ui', 'inspect', 'memory'].includes(request.op)), false);
  assert.equal(report.presentation.mean_ms, 25);
  assert.equal(report.comparison_verdict, 'NOT_EVALUATED');
  assert.equal((await fs.readFile(report.artifacts.report, 'utf8')).includes('VALID MEASUREMENT'), true);
  const status = JSON.parse(await fs.readFile(report.artifacts.status, 'utf8'));
  assert.equal(status.phase, 'completed');
  assert.equal(JSON.stringify(status).includes(harness.manifest.session), false);
  await assert.rejects(runBenchmark(harness), { code: 'EEXIST' });
});

test('invalid or interrupted native captures remain invalid and release inputs', async t => {
  for (const options of [{ nativeValid: false }, { interrupt: true }]) {
    const { harness, requests } = await benchmarkFixture(t, options);
    const report = await runBenchmark(harness, { durationMs: 100, warmupMs: 0, scenario: 'planet-harriers', control: 'scripted', after: 'leave' });
    assert.equal(report.valid, false);
    assert.ok(report.errors.length > 0);
    assert.equal(report.cleanup.inputs_released, true);
    assert.equal(requests.filter(request => request.op === 'performance_end').length, 1);
    assert.equal(JSON.parse(await fs.readFile(report.artifacts.status, 'utf8')).phase, 'failed');
  }
});

test('benchmark defaults to a current-game capture without scripted controller input', async t => {
  const { harness, requests } = await benchmarkFixture(t);
  const report = await runBenchmark(harness, { durationMs: 100, warmupMs: 0, after: 'leave' });
  assert.equal(report.scenario, 'current');
  assert.equal(report.control, 'external');
  assert.equal(requests.some(request => request.op === 'controller'), false);
});

test('current-game benchmark waits for fresh telemetry before starting capture', async t => {
  const { harness, requests } = await benchmarkFixture(t);
  const originalRequest = harness.request.bind(harness);
  let liveReads = 0;
  harness.request = async (...args) => {
    const result = await originalRequest(...args);
    if (args[0] === 'live' && liveReads++ < 2)
      return { ...result, telemetry: { ...result.telemetry, sample_available: false, stale: true, sample_age_ms: null } };
    return result;
  };
  const report = await runBenchmark(harness, { durationMs: 100, warmupMs: 0, after: 'leave' });
  assert.equal(report.valid, true);
  assert.ok(requests.filter(request => request.op === 'live').length >= 4);
  assert.equal(requests.some(request => request.op === 'controller'), false);
});

test('stale benchmark telemetry fails and releases controller input', async t => {
  const { harness, requests } = await benchmarkFixture(t);
  const originalRequest = harness.request.bind(harness);
  let liveReads = 0;
  harness.request = async (...args) => {
    const result = await originalRequest(...args);
    if (args[0] === 'live' && liveReads++ >= 2)
      return { ...result, telemetry: { ...result.telemetry, stale: true, sample_age_ms: 501 } };
    return result;
  };
  const report = await runBenchmark(harness, { durationMs: 100, warmupMs: 0, scenario: 'current', control: 'external', after: 'leave' });
  assert.equal(report.valid, false);
  assert.ok(report.errors.some(error => error.includes('stale or unavailable')));
  assert.equal(report.cleanup.inputs_released, true);
  assert.equal(requests.filter(request => request.op === 'release').length, 1);
});

test('current-game startup timeout retains an invalid report and releases input', async t => {
  const { harness, requests, state } = await benchmarkFixture(t);
  const originalRequest = harness.request.bind(harness);
  let startupReads = 0;
  harness.request = async (op, args, options) => {
    if (op === 'live') {
      if (options?.timeoutMs !== undefined) startupReads++;
      const sample = state();
      return { ...sample, telemetry: { ...sample.telemetry, stale: true, sample_age_ms: 501 } };
    }
    return originalRequest(op, args, options);
  };
  const report = await runBenchmark(harness, { durationMs: 100, warmupMs: 0, startupTimeoutMs: 300, after: 'leave' });
  assert.equal(report.valid, false);
  assert.ok(report.errors.some(error => error === 'Timed out waiting for a fresh running game state'), JSON.stringify(report.errors));
  assert.ok(startupReads > 0);
  assert.equal(requests.some(request => request.op === 'performance_begin'), false);
  assert.equal(requests.filter(request => request.op === 'release').length, 1);
  assert.equal(report.cleanup.inputs_released, true);
});

test('current-game startup rejects a fresh sample returned after the deadline', async t => {
  const { harness, requests, state } = await benchmarkFixture(t);
  const originalRequest = harness.request.bind(harness);
  let liveReads = 0;
  harness.request = async (op, args, options) => {
    if (op === 'live') {
      const sample = state();
      if (++liveReads === 2) await delay(125);
      return sample;
    }
    return originalRequest(op, args, options);
  };
  const report = await runBenchmark(harness, { durationMs: 100, warmupMs: 0, startupTimeoutMs: 100, after: 'leave' });
  assert.equal(report.valid, false);
  assert.ok(report.errors.some(error => error === 'Timed out waiting for a fresh running game state'), JSON.stringify(report.errors));
  assert.equal(liveReads, 2);
  assert.equal(requests.some(request => request.op === 'performance_begin'), false);
  assert.equal(requests.filter(request => request.op === 'release').length, 1);
  assert.equal(report.cleanup.inputs_released, true);
});

test('bounded current-game live request timeout is reported and inputs are released', async t => {
  const { harness, requests, state } = await benchmarkFixture(t);
  const originalRequest = harness.request.bind(harness);
  const startupTimeoutMs = 1000;
  let liveReads = 0;
  let requestBudget;
  const transportError = 'Command timed out; outcome is unknown. Inputs expire within their lease on the next emulator UI tick';
  harness.request = async (op, args, options) => {
    if (op === 'live') {
      if (++liveReads === 1) return state();
      requestBudget = options?.timeoutMs;
      throw new Error(transportError);
    }
    return originalRequest(op, args, options);
  };
  const report = await runBenchmark(harness, { durationMs: 100, warmupMs: 0, startupTimeoutMs, after: 'leave' });
  assert.equal(report.valid, false);
  assert.ok(report.errors.includes(transportError), JSON.stringify(report.errors));
  assert.ok(Number.isInteger(requestBudget) && requestBudget > 0 && requestBudget <= startupTimeoutMs);
  assert.equal(requests.some(request => request.op === 'performance_begin'), false);
  assert.equal(requests.filter(request => request.op === 'release').length, 1);
  assert.equal(report.cleanup.inputs_released, true);
});

test('external-control benchmarking sends no controller snapshots and detects modified inputs', async t => {
  const { harness, requests } = await benchmarkFixture(t, { sourceMutation: true });
  const report = await runBenchmark(harness, { durationMs: 100, warmupMs: 0, scenario: 'current', control: 'external', after: 'leave' });
  assert.equal(requests.some(request => request.op === 'controller'), false);
  assert.equal(report.valid, false);
  assert.ok(report.errors.some(error => error.includes('Original inputs changed')));
});

test('benchmark report escapes game text and exposes missing metrics without invented zeros', () => {
  const html = renderBenchmarkReport({ game_id: '<script>alert(1)</script>', errors: ['bad <state>'], valid: false,
    osd: summarizeOsd([]), requested_duration_ms: 60000 });
  assert.equal(html.includes('<script>alert(1)</script>'), false);
  assert.equal(html.includes('&lt;script&gt;'), true);
  assert.equal(html.includes('Unavailable'), true);
  const settings = benchmarkSettings({ config: { 'rend.Resolution': 720 }, rend: { OverlayShowRam: false } });
  assert.equal(settings.config['rend.Resolution'], 720);
  assert.equal(Object.hasOwn(settings.config, 'HikaruSorting'), false);
  assert.equal(benchmarkSettings({}, 'planet-harriers').config.HikaruSorting, 0);
  assert.equal(settings.rend.OverlayShowRam, false);
  assert.equal(settings.rend.OverlayShowFPS, true);
});

test('cancelled benchmarks retain an invalid report and release their controls', async t => {
  const { harness } = await benchmarkFixture(t);
  const cancellation = new AbortController();
  const pending = runBenchmark(harness, { durationMs: 1000, warmupMs: 0, after: 'leave', signal: cancellation.signal,
    scenario: 'planet-harriers', control: 'scripted',
    onProgress: status => { if (status.phase === 'running') cancellation.abort(new Error('cancelled by test')); } });
  const report = await pending;
  assert.equal(report.valid, false);
  assert.equal(report.cleanup.inputs_released, true);
  assert.ok(report.errors.includes('cancelled by test'));
  assert.equal(JSON.parse(await fs.readFile(report.artifacts.json, 'utf8')).valid, false);
});

test('aborted holds still release controller state', async t => {
  const { harness } = await fixture(t);
  const requests = await fakeBridge(t, harness);
  const controller = new AbortController();
  controller.abort(new Error('cancelled'));
  await assert.rejects(harness.hold({ buttons: ['A'] }, 500, { signal: controller.signal }), /cancelled/);
  assert.deepEqual(requests.map(request => request.op), ['release']);
});

test('held inputs renew bounded leases and finish with neutral state', async t => {
  const { harness } = await fixture(t);
  const requests = await fakeBridge(t, harness);
  await harness.hold({ buttons: ['A'], triggers: [0, 40000] }, 280);
  assert.ok(requests.filter(request => request.op === 'controller').length >= 2);
  assert.equal(requests[0].args.lease_ms, 1000);
  assert.equal(requests.at(-1).op, 'release');
});

test('recording retains raw samples and marks comparison as unevaluated', async t => {
  const { harness } = await fixture(t);
  await fakeBridge(t, harness, async request => request.op === 'performance_end' ? capture() : {});
  const report = await harness.recordPerformance(100);
  assert.deepEqual(report.samples_ms, [10, 20, 30, 40]);
  assert.equal(report.comparison_verdict, 'NOT_EVALUATED');
  assert.equal(report.summary.mean_ms, 25);
  const artifacts = (await fs.readdir(harness.directory)).filter(file => /^performance-.*\.json$/.test(file));
  assert.equal(artifacts.length, 1);
});

test('incomplete captures retain an invalid evidence report', async t => {
  const { harness } = await fixture(t);
  await fakeBridge(t, harness, async request => request.op === 'performance_end'
    ? { ...capture(), samples_ms: [], valid: false, invalid_reasons: ['insufficient_samples'] } : {});
  const report = await harness.recordPerformance(100);
  assert.equal(report.valid, false);
  assert.equal(report.summary, null);
  assert.ok(report.invalid_reasons.includes('insufficient_samples'));
  const files = (await fs.readdir(harness.directory)).filter(file => /^performance-.*\.json$/.test(file));
  assert.equal(files.length, 1);
  assert.deepEqual(JSON.parse(await fs.readFile(path.join(harness.directory, files[0]), 'utf8')).samples_ms, []);
});

test('native UI actions require a fresh inspection and use widget IDs', async t => {
  const { harness } = await fixture(t);
  const requests = await fakeBridge(t, harness, async request => request.op === 'inspect'
    ? { observation_id: request.id, wall_ms: Date.now(), state: { running: true }, screenshot: null,
      ui: { items: [{ id: 42, label: 'Settings', visible: true, disabled: false }], text: 'Settings' } } : {});
  await assert.rejects(harness.ui('click', { item_id: 42 }), /inspect/i);
  const observed = await harness.observe();
  assert.equal(observed.state.running, true);
  await harness.click('Settings');
  const click = requests.find(request => request.op === 'ui');
  assert.equal(click.args.item_id, 42);
  assert.equal(click.args.action, 'click');
  assert.equal(click.args.observation_id, observed.observation_id);
  assert.equal(requests.at(-1).op, 'inspect');
  assert.equal(requests[0].args.screenshot, false);
  await harness.setController();
  await assert.rejects(harness.ui('click', { item_id: 42 }), /inspect/i);
});

test('semantic clicks reject ambiguous or disabled items', async t => {
  const { harness } = await fixture(t);
  await fakeBridge(t, harness, async request => ({ observation_id: request.id, wall_ms: Date.now(), screenshot: null,
    ui: { items: [{ id: 1, label: 'Duplicate', visible: true, disabled: false },
      { id: 2, label: 'Duplicate', visible: true, disabled: false }, { id: 3, label: 'Disabled', visible: true, disabled: true }] } }));
  await harness.observe();
  await assert.rejects(harness.click('Duplicate'), /found 2/);
  await assert.rejects(harness.click('Disabled'), /disabled/);
  await harness.note('Native item fixture', { verdict: 'UNCERTAIN' });
});

test('UI mismatch reports expected and actual vector values', async t => {
  const { harness } = await fixture(t);
  await fakeUiBridge(t, harness, { value: [1, 2] });
  const result = await harness.expectUi('Friendly label', { value: [1, 3] });
  assert.equal(result.outcome, 'FAIL');
  assert.deepEqual(result.mismatches, [{ field: 'value', expected: [1, 3], actual: [1, 2] }]);
});
test('native capture rejects paths outside the session evidence directory', async t => {
  const { harness } = await fixture(t);
  await fakeBridge(t, harness, async request => ({ observation_id: request.id, wall_ms: Date.now(), ui: { items: [] },
    screenshot: { file: '../../outside.png' } }));
  await assert.rejects(harness.observe({ screenshot: true }), /screenshot path/);
});

test('live feed yields structured states without requesting screenshots', async t => {
  const { harness } = await fixture(t);
  let sequence = 0;
  const requests = await fakeBridge(t, harness, async () => ({ state: { game_id: 'pharrier' }, telemetry: {
    sequence: ++sequence, stale: false, sample_age_ms: 1, game_state: { values: { timer: { value: 80 } } } } }));
  const samples = [];
  for await (const sample of harness.watch({ intervalMs: 50, durationMs: 160 })) samples.push(sample);
  assert.ok(samples.length >= 2);
  assert.equal(samples[0].telemetry.game_state.values.timer.value, 80);
  assert.equal(samples.at(-1).telemetry.sequence, samples.length);
  assert.ok(requests.every(request => request.op === 'live'));
  assert.equal((await fs.readdir(path.join(harness.directory, 'screenshots'))).length, 0);
});

test('unavailable live data is reported rather than replaced with a guessed state', async t => {
  const { harness } = await fixture(t);
  await fakeBridge(t, harness, async () => ({}), response => ({ ...response, ok: false, error: 'Source unavailable' }));
  const samples = [];
  for await (const sample of harness.watch({ durationMs: 100 })) samples.push(sample);
  assert.equal(samples.length, 1);
  assert.equal(samples[0].kind, 'source_unavailable');
  assert.match(samples[0].error, /Source unavailable/);
  assert.equal(samples[0].telemetry, undefined);
});

test('Muse WSL invocations use Windows Node and preserve literal Windows arguments', () => {
  const command = agentInvocation('C:\\Tools With Spaces\\node.exe', 'D:\\harness\\hollytest.mjs',
    'D:\\tmp\\session', 'press', ['--button', 'A'], true);
  assert.equal(command.executable, '/mnt/c/Tools With Spaces/node.exe');
  assert.deepEqual(command.args, ['D:\\harness\\hollytest.mjs', 'press', '--run', 'D:\\tmp\\session', '--button', 'A']);
  assert.throws(() => agentInvocation('node', 'cli', 'run', 'live\nother'));
});

test('agent play-test jobs contain identities and native commands without dispatching or exposing the session token', async t => {
  const { harness, directory } = await fixture(t);
  await fs.writeFile(path.join(harness.directory, 'ready.json'), JSON.stringify({ version: 1, session: harness.manifest.session,
    pid: 123, capabilities: ['controller', 'exclusive_controller_input', 'native_ui', 'live_telemetry', 'typed_memory_watches'] }));
  const requests = await fakeBridge(t, harness, async () => ({ state: { game_id: 'pharrier' },
    telemetry: { sequence: 1, game_state: { profile_id: 'planet-test', status: 'matched_operator_mapping', values: { main_mode: { value: 3 } } } } }));
  const out = path.join(directory, 'muse-job.json');
  const job = await harness.createPlaytestJob({ out, workspace: 'D:\\path\\to\\Hollycast' });
  assert.equal(job.schema, 'hollytest.playtest-job.v1');
  assert.equal(job.session.game_id, 'pharrier');
  assert.deepEqual(job.mapping, { profile_id: 'planet-test', status: 'matched_operator_mapping', fields: ['main_mode'] });
  assert.equal(job.constraints.source_edits, false);
  assert.match(job.objective, /current game/);
  assert.equal(job.constraints.screenshots, 'fallback_only');
  assert.equal(job.bridge_delivery.auto_dispatch, false);
  assert.equal(job.commands.windows.live.args[1], 'live');
  assert.equal(JSON.stringify(job).includes(harness.manifest.session), false);
  assert.ok(requests.every(request => request.op === 'live'));
  await assert.rejects(harness.createPlaytestJob({ out }), /overwrite evidence/);
  assert.equal((await fs.readdir(directory)).some(file => file.endsWith('.tmp')), false);
});

test('agent CLI exposes analog holds, controller snapshots and native UI actions', async t => {
  const { harness } = await fixture(t);
  // The bridge's wall clock can read ahead of the CLI's on Windows.
  const requests = await fakeBridge(t, harness, async request => request.op === 'inspect'
    ? { observation_id: request.id, wall_ms: Date.now() + 1000, screenshot: null, ui: { items: [] } } : {});
  const invoke = (command, args) => promisify(execFile)(process.execPath,
    [fileURLToPath(new URL('./hollytest.mjs', import.meta.url)), command, '--run', harness.directory, ...args]);
  await invoke('controller', ['--state', JSON.stringify({ port: 1, axes: [12000, -8000, 100, -100], triggers: [0, 45000] })]);
  assert.deepEqual(requests.at(-1).args.axes, [12000, -8000, 100, -100]);
  assert.equal(requests.at(-1).args.port, 1);
  await invoke('press', ['--button', 'A', '--port', '2', '--milliseconds', '1']);
  assert.equal(requests.filter(request => request.op === 'controller').at(-1).args.port, 2);
  await invoke('hold', ['--state', JSON.stringify({ buttons: ['A', 'B'] }), '--milliseconds', '100']);
  assert.equal(requests.at(-1).op, 'release');
  await invoke('ui', ['--input', JSON.stringify({ action: 'key', key: 'Escape' })]);
  assert.equal(requests.at(-2).op, 'ui');
  assert.equal(requests.at(-2).args.key, 'Escape');
  assert.equal(requests.at(-1).op, 'inspect');
});
