// Portions Copyright 2026 The Hollycast Authors
import * as fs from 'node:fs/promises';
import { createReadStream } from 'node:fs';
import path from 'node:path';
import { createHash, randomBytes } from 'node:crypto';
import { spawn } from 'node:child_process';
import { setTimeout as delay } from 'node:timers/promises';
import { pathToFileURL, fileURLToPath } from 'node:url';
import os from 'node:os';

const BUTTONS = new Set(['A', 'B', 'C', 'D', 'X', 'Y', 'Z', 'START', 'UP', 'DOWN', 'LEFT', 'RIGHT',
  'UP2', 'DOWN2', 'LEFT2', 'RIGHT2', 'RELOAD', 'INSERT_CARD']);
const ID = /^[0-9a-f]{32}$/;

export function agentInvocation(nodeExecutable, cli, directory, command, args = [], wsl = false) {
  check([nodeExecutable, cli, directory, command, ...args].every(value => typeof value === 'string' && !/[\r\n\0]/.test(value)),
    'Agent invocation arguments must be literal strings without control characters');
  let executable = nodeExecutable;
  if (wsl && /^[A-Za-z]:[\\/]/.test(executable))
    executable = `/mnt/${executable[0].toLowerCase()}/${executable.slice(3).replaceAll('\\', '/')}`;
  // From WSL, launch Windows Node with Windows arguments. This preserves the
  // native session's path semantics and avoids a second Linux-side profile.
  return { executable, args: [cli, command, '--run', directory, ...args] };
}
const SETTINGS = {
  config: new Set(['UseReios', 'Dynarec.Enabled', 'aica.DSPEnabled', 'rend.ShowFPS', 'rend.Resolution',
    'pvr.rend', 'rend.vsync', 'rend.ThreadedRendering', 'ta.skip', 'pvr.AutoSkipFrame',
    'rend.WideScreen', 'rend.PerStripSorting', 'rend.UseMipmaps', 'pvr.MaxThreads',
    'HikaruSorting', 'HikaruMaxLayers', 'HikaruPlanetRegion', 'HikaruPlanetDpadStick', 'HikaruAudioPacing']),
  rend: /^(ResourceMonitorMode|Overlay[A-Za-z0-9_.]*|Perf[A-Za-z0-9_.]*)$/,
};

function check(condition, message) {
  if (!condition) throw new Error(message);
}

async function exists(file) {
  try { await fs.access(file); return true; }
  catch (error) { if (error.code === 'ENOENT') return false; throw error; }
}

async function readJson(file, maximum = 8 * 1024 * 1024) {
  const stat = await fs.lstat(file);
  check(stat.isFile() && !stat.isSymbolicLink() && stat.size <= maximum, `Invalid or oversized JSON: ${file}`);
  return JSON.parse(await fs.readFile(file, 'utf8'));
}

async function writeNewJson(file, value) {
  const temporary = `${file}.${randomBytes(8).toString('hex')}.tmp`;
  try {
    await fs.writeFile(temporary, `${JSON.stringify(value, null, 2)}\n`, { flag: 'wx' });
    // Linking publishes the complete file atomically and refuses replacement,
    // including when another process wins the race to this evidence path.
    try { await fs.link(temporary, file); }
    catch (error) { if (error.code === 'EEXIST') throw new Error(`Refusing to overwrite evidence: ${file}`); throw error; }
  } finally { await fs.rm(temporary, { force: true }); }
}

async function sha256(file) {
  const hash = createHash('sha256');
  for await (const chunk of createReadStream(file)) hash.update(chunk);
  return hash.digest('hex');
}

export function controllerState(options = {}) {
  check(options && typeof options === 'object' && !Array.isArray(options)
    && Object.keys(options).every(key => ['port', 'buttons', 'axes', 'triggers', 'leaseMs'].includes(key)),
    'Unknown controller option');
  const { port = 0, buttons = [], axes = [0, 0, 0, 0], triggers = [0, 0], leaseMs = 500 } = options;
  check(Number.isInteger(port) && port >= 0 && port <= 3, 'Port must be 0..3');
  check(Number.isInteger(leaseMs) && leaseMs >= 1 && leaseMs <= 2000, 'Input lease must be 1..2000 ms');
  check(Array.isArray(buttons) && buttons.every(button => BUTTONS.has(button)) && new Set(buttons).size === buttons.length,
    'Unknown or duplicate controller button');
  check(Array.isArray(axes) && axes.length === 4 && axes.every(value => Number.isInteger(value) && Math.abs(value) <= 32767),
    'Four stick axes must be integers in -32767..32767');
  check(Array.isArray(triggers) && triggers.length === 2 && triggers.every(value => Number.isInteger(value) && value >= 0 && value <= 65535),
    'Two triggers must be integers in 0..65535');
  return { port, buttons: [...buttons], axes: [...axes], triggers: [...triggers], lease_ms: leaseMs };
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

function isolatedConfig(profile, game) {
  const sections = { config: { 'Dreamcast.AutoSaveState': false, 'Dreamcast.AutoLoadState': false,
    UploadCrashLogs: false, UseReios: true, 'rend.ShowFPS': true, FetchBoxart: false } };
  if (game) check(!/[\r\n]/.test(game), 'Game path contains a newline');
  for (const [section, values] of Object.entries(profile)) {
    check(Object.hasOwn(SETTINGS, section) && values && typeof values === 'object' && !Array.isArray(values), 'Unsupported profile section');
    sections[section] ||= {};
    for (const [key, value] of Object.entries(values)) {
      const allowed = SETTINGS[section];
      check(allowed instanceof Set ? allowed.has(key) : allowed.test(key), `Unsupported setting: ${section}.${key}`);
      check(typeof value === 'boolean' || (Number.isFinite(value) && typeof value === 'number'), `Setting must be numeric or boolean: ${key}`);
      sections[section][key] = value;
    }
  }
  // Avoid network sessions and scripts in disposable runs. Credentials and
  // arbitrary user configuration files are never copied into the fixture.
  sections.network = { Enable: false, GGPO: false, EmulateBBA: false };
  sections.updates = { startup: false };
  sections.window = { fullscreen: false };
  const lines = ['; Portions Copyright 2026 The Hollycast Authors'];
  for (const [section, values] of Object.entries(sections)) {
    lines.push(`[${section}]`);
    for (const [key, value] of Object.entries(values))
      lines.push(`${key} = ${typeof value === 'boolean' ? (value ? 'yes' : 'no') : value}`);
  }
  return `${lines.join('\n')}\n`;
}

export class HollycastHarness {
  constructor(directory, manifest) {
    this.directory = directory;
    this.manifest = manifest;
    this.queue = Promise.resolve();
    this.observation = null;
    this.capturing = false;
  }

  static async prepare({ executable, out, game = null, firmwareDirectory = null, profile = {}, revision = 'unrecorded' }) {
    const exe = await fs.realpath(path.resolve(executable));
    check((await fs.stat(exe)).isFile(), 'Executable is not a regular file');
    const media = game ? await fs.realpath(path.resolve(game)) : null;
    if (media) check((await fs.stat(media)).isFile(), 'Game is not a regular file');
    const directory = path.resolve(out);
    check(!await exists(directory), 'Output directory must be new');
    // Validate the profile before creating any run artifacts.
    const config = isolatedConfig(profile, media);
    await fs.mkdir(directory, { recursive: false });
    await fs.mkdir(path.join(directory, 'home', 'data'), { recursive: true });
    await fs.mkdir(path.join(directory, 'screenshots'));
    const seeds = [];
    if (firmwareDirectory) {
      const fixture = await fs.realpath(path.resolve(firmwareDirectory));
      const files = [['dc_boot.bin', 2 * 1024 * 1024], ['dc_flash.bin', 128 * 1024], ['dc_nvmem.bin', 128 * 1024]];
      files.push(['pharrier.hikaru.nvmem', 65536], ['pharrier.mie.eeprom', 128]);
      for (const name of ['hikaru.zip', 'hikaru.7z', 'mie.zip', 'mie.7z']) files.push([name, null]);
      for (const name of ['airlbios', 'awbios', 'f355bios', 'f355dlx', 'hod2bios', 'naomi', 'naomidev', 'naomigd', 'naomi2', 'segasp'])
        for (const extension of ['zip', '7z']) files.push([`${name}.${extension}`, null]);
      for (const port of 'ABCD') for (const slot of [1, 2]) files.push([`vmu_save_${port}${slot}.bin`, 128 * 1024]);
      for (const [name, size] of files) {
        const source = path.join(fixture, name);
        if (!await exists(source)) continue;
        const stat = await fs.lstat(source);
        const sizeValid = size === null ? stat.size > 0 && stat.size <= 16 * 1024 * 1024 : stat.size === size;
        check(stat.isFile() && !stat.isSymbolicLink() && sizeValid, `Invalid firmware/save seed: ${name}`);
        const destination = path.join(directory, 'home', 'data', name);
        await fs.copyFile(source, destination, fs.constants.COPYFILE_EXCL);
        const digest = await sha256(source);
        check(await sha256(destination) === digest, `Seed changed during copy: ${name}`);
        seeds.push({ source, sha256: digest, size: stat.size });
      }
    }
    await fs.writeFile(path.join(directory, 'home', 'emu.cfg'), config, { flag: 'wx' });
    const manifest = { version: 1, session: randomBytes(16).toString('hex'), executable: exe,
      executable_sha256: await sha256(exe), source_revision: revision, game: media, game_sha256: media ? await sha256(media) : null, seeds,
      profile, created_at: new Date().toISOString(), host: { platform: os.platform(), release: os.release(), arch: os.arch() } };
    await writeNewJson(path.join(directory, 'session.json'), manifest);
    return new HollycastHarness(directory, manifest);
  }

  static async attach(directory) {
    directory = await fs.realpath(path.resolve(directory));
    const manifest = await readJson(path.join(directory, 'session.json'), 65536);
    check(manifest.version === 1 && ID.test(manifest.session) && typeof manifest.executable === 'string', 'Invalid session manifest');
    return new HollycastHarness(directory, manifest);
  }

  async launch({ timeoutMs = 30000, visible = true } = {}) {
    check(!await exists(path.join(this.directory, 'emulator.lock')), 'This session has already been launched');
    check(await sha256(this.manifest.executable) === this.manifest.executable_sha256, 'Executable changed after preparation');
    const env = { ...globalThis.process.env, HOLLYCAST_HARNESS_DIR: this.directory, HOLLYCAST_HARNESS_SESSION: this.manifest.session,
      HOLLYCAST_HARNESS_MEDIA: this.manifest.game || '', HOLLYCAST_HARNESS_MEDIA_SHA256: this.manifest.game_sha256 || '' };
    delete env.HOLLYBENCH_REQUEST;
    delete env.HOLLYBENCH_RESULT;
    let child;
    let launchError = null;
    const deadline = Date.now() + timeoutMs;
    try {
      const log = await fs.open(path.join(this.directory, 'process.log'), 'wx');
      try {
        // The CLI exits after readiness; the emulator owns its own lifetime.
        child = spawn(this.manifest.executable, this.manifest.game ? [this.manifest.game] : [],
          { cwd: path.join(this.directory, 'home'), env, stdio: ['ignore', log.fd, log.fd], detached: true, windowsHide: !visible });
        child.on('error', error => { launchError = error; });
      } finally { await log.close(); }
      while (!await exists(path.join(this.directory, 'ready.json'))) {
        if (launchError) throw launchError;
        check(child.exitCode === null && child.signalCode === null,
          `Emulator exited before readiness: ${child.exitCode ?? child.signalCode}; inspect process.log`);
        check(Date.now() < deadline, 'Emulator did not expose the test bridge; inspect process.log');
        await delay(50);
      }
      const ready = await readJson(path.join(this.directory, 'ready.json'), 65536);
      check(ready.version === 1 && ready.session === this.manifest.session && ready.pid === child.pid, 'Readiness identity mismatch');
      await writeNewJson(path.join(this.directory, 'launch.json'), { pid: child.pid, ready_at: new Date().toISOString(), capabilities: ready.capabilities });
      child.unref();
      return ready;
    } catch (error) {
      child?.kill();
      throw error;
    }
  }

  request(op, args = {}, { timeoutMs = 10000 } = {}) {
    const pending = this.queue.then(() => this.send(op, args, timeoutMs));
    this.queue = pending.catch(() => {});
    return pending;
  }

  async send(op, args, timeoutMs) {
    check(Number.isInteger(timeoutMs) && timeoutMs >= 1 && timeoutMs <= 30000, 'Timeout must be 1..30000 ms');
    const lock = path.join(this.directory, 'client.lock');
    const start = Date.now();
    // A monitor and controller may share a session. Wait for ownership, but
    // never steal an abandoned lock or retry a command with an unknown outcome.
    while (true) {
      try { await fs.mkdir(lock); break; }
      catch (error) {
        if (error.code !== 'EEXIST') throw error;
        check(Date.now() < start + timeoutMs, 'Harness client lock is busy; it has not been stolen');
        await delay(Math.min(10, Math.max(1, start + timeoutMs - Date.now())));
      }
    }
    const id = randomBytes(16).toString('hex');
    const request = { version: 1, session: this.manifest.session, id, expires_ms: start + timeoutMs, op, args };
    const responsePath = path.join(this.directory, `response-${id}.json`);
    const commandPath = path.join(this.directory, 'command.json');
    try {
      check(Date.now() < request.expires_ms, 'Request deadline elapsed while waiting for the client lock');
      check(!await exists(commandPath), 'An earlier command is still pending; commands are never retried automatically');
      await writeNewJson(commandPath, request);
      while (!await exists(responsePath)) {
        check(Date.now() < request.expires_ms, 'Command timed out; outcome is unknown. Inputs expire within their lease on the next emulator UI tick');
        await delay(10);
      }
      const response = await readJson(responsePath);
      check(response.version === 1 && response.session === this.manifest.session && response.id === id && typeof response.ok === 'boolean',
        'Response identity mismatch');
      await fs.appendFile(path.join(this.directory, 'events.jsonl'), `${JSON.stringify({ id, op, args, start_wall_ms: start,
        end_wall_ms: Date.now(), ok: response.ok, error: response.error ?? null,
        result: op === 'performance_end' ? { artifact: path.basename(responsePath) } : response.result })}\n`);
      check(response.ok, response.error || 'Native bridge rejected the command');
      if (op !== 'observe') this.observation = null;
      return response.result;
    } finally {
      await fs.rmdir(lock);
    }
  }

  async setController(options = {}) {
    return this.request('controller', controllerState(options));
  }

  async hold(options, milliseconds, { signal } = {}) {
    check(Number.isInteger(milliseconds) && milliseconds >= 1 && milliseconds <= 600000, 'Hold duration must be 1..600000 ms');
    const input = { ...controllerState(options), lease_ms: 1000 };
    let end = null;
    try {
      do {
        if (signal?.aborted) throw signal.reason || new Error('Input aborted');
        await this.request('controller', input);
        end ??= Date.now() + milliseconds;
        await delay(Math.min(250, Math.max(1, end - Date.now())), undefined, { signal });
      } while (Date.now() < end);
    } finally {
      await this.release();
    }
  }

  async press(button, milliseconds = 120, port = 0) {
    return this.hold({ port, buttons: [button] }, milliseconds);
  }

  async release() { return this.request('release'); }
  async menu() { return this.request('menu'); }
  async readMemory(offset, length, region = 'main_ram') { return this.request('memory', { offset, length, region }); }
  async setWatchProfile(profile) { return this.request('watch_profile', profile); }
  async live() { return this.request('live'); }
  async stop() { return this.request('quit'); }

  async doctor() {
    const ready = await readJson(path.join(this.directory, 'ready.json'), 65536);
    check(ready.version === 1 && ready.session === this.manifest.session && Number.isInteger(ready.pid) && ready.pid > 0, 'Native session identity mismatch');
    const required = ['controller', 'exclusive_controller_input', 'native_ui', 'live_telemetry', 'typed_memory_watches'];
    check(Array.isArray(ready.capabilities) && required.every(capability => ready.capabilities.includes(capability)),
      'This native build does not expose all required testing capabilities');
    const live = await this.live();
    return { protocol_version: 1, connected: true, pid: ready.pid, capabilities: ready.capabilities,
      game_id: live.state?.game_id, source_revision: this.manifest.source_revision,
      executable_sha256: this.manifest.executable_sha256, runtime: { node: globalThis.process.version, platform: os.platform() },
      live, desktop_control_required: false };
  }

  async createPlaytestJob({ out, objective = 'Test the current game with native live data and report reproducible findings.',
    durationMs = 300000, workspace = globalThis.process.cwd() }) {
    check(typeof objective === 'string' && objective.length > 0 && objective.length <= 4096, 'Job objective must be 1..4096 characters');
    check(Number.isInteger(durationMs) && durationMs >= 1000 && durationMs <= 3600000, 'Job duration must be 1000..3600000 ms');
    const status = await this.doctor();
    const cli = fileURLToPath(import.meta.url);
    const invocation = (command, args = [], wsl = false) => agentInvocation(globalThis.process.execPath, cli, this.directory, command, args, wsl);
    const job = { schema: 'hollytest.playtest-job.v1', job_id: randomBytes(16).toString('hex'),
      created_at: new Date().toISOString(), objective, bridge_workspace: workspace,
      session: { directory: this.directory, pid: status.pid, game_id: status.game_id,
        executable_sha256: status.executable_sha256, source_revision: status.source_revision },
      mapping: { profile_id: status.live.telemetry?.game_state?.profile_id ?? null,
        status: status.live.telemetry?.game_state?.status ?? 'unmapped',
        fields: Object.keys(status.live.telemetry?.game_state?.values ?? {}) },
      constraints: { duration_ms: durationMs, source_edits: false, immutable_test_binary: true,
        one_controller_owner: true, screenshots: 'fallback_only', unmapped_game_values: 'report_unknown',
        external_messages: false, preserve_original_game_and_save_files: true },
      commands: { windows: { doctor: invocation('doctor'), live: invocation('live'),
        watch: invocation('watch', ['--milliseconds', '5000', '--interval', '100']),
        inspect: invocation('observe'), screenshot: invocation('screenshot'),
        hold: invocation('hold', ['--state', JSON.stringify({ buttons: ['A'], axes: [0, 0, 0, 0] }), '--milliseconds', '500']),
        ui_escape: invocation('ui', ['--input', JSON.stringify({ action: 'key', key: 'Escape' })]),
        release: invocation('release'), verify: invocation('verify') },
        wsl: { doctor: invocation('doctor', [], true), live: invocation('live', [], true),
          watch: invocation('watch', ['--milliseconds', '5000', '--interval', '100'], true),
          inspect: invocation('observe', [], true), screenshot: invocation('screenshot', [], true),
          hold: invocation('hold', ['--state', JSON.stringify({ buttons: ['A'], axes: [0, 0, 0, 0] }), '--milliseconds', '500'], true),
          ui_escape: invocation('ui', ['--input', JSON.stringify({ action: 'key', key: 'Escape' })], true),
          release: invocation('release', [], true), verify: invocation('verify', [], true) } },
      procedure: ['Accept the bridge transfer in the same workspace before acting.',
        'Run doctor and verify, and check the expected game and executable identity.',
        'Use native live data and UI items as the primary observations; screenshots are a fallback.',
        'Control only this disposable emulator session. Use the documented controller and UI commands.',
        'Record steps, timestamps, observations, failures, uncertainty, and evidence paths.',
        'Release all inputs, verify original inputs, and return the result through the existing bridge.'],
      result_contract: { job_id: 'echo this job_id', outcome: 'PASS, FAIL, or INCONCLUSIVE with explicit tested coverage',
        findings: 'reproduction steps, expected/actual behavior, timestamps, and local artifact paths',
        unmapped_fields: 'game meanings that could not be established', original_inputs_verified: 'observed result',
        controller_released: 'observed result', screenshots_used: 'whether fallback capture was needed' },
      bridge_delivery: { mode: 'explicit_send_and_pull', receiver: 'muse-terminal',
        auto_dispatch: false, live_data_transport: 'native_harness_not_conversation_bridge' } };
    await writeNewJson(path.resolve(out), job);
    return job;
  }

  async observe({ screenshot = false } = {}) {
    const result = await this.request('inspect', { screenshot });
    check(ID.test(result.observation_id) && Array.isArray(result.ui?.items), 'Invalid native UI inspection');
    if (result.screenshot) {
      check(/^screenshots\/[0-9a-f]{32}\.png$/.test(result.screenshot.file), 'Invalid native screenshot path');
      const file = path.join(this.directory, result.screenshot.file);
      const stat = await fs.lstat(file);
      check(stat.isFile() && !stat.isSymbolicLink() && stat.size <= 64 * 1024 * 1024, 'Invalid native screenshot artifact');
      result.screenshot.sha256 = await sha256(file);
    }
    await fs.appendFile(path.join(this.directory, 'observations.jsonl'), `${JSON.stringify(result)}\n`);
    this.observation = result;
    return result;
  }

  async ui(action, options = {}) {
    check(['click', 'key', 'text', 'scroll', 'drag'].includes(action), 'Unsupported native UI action');
    const observation = this.observation;
    check(observation && Date.now() - observation.wall_ms <= 10000, 'Inspect the native UI immediately before input');
    this.observation = null;
    await this.request('ui', { ...options, observation_id: observation.observation_id, action });
    return this.observe();
  }

  async click(label) {
    if (!this.observation || Date.now() - this.observation.wall_ms > 4000) await this.observe();
    const matches = this.observation.ui.items.filter(item => item.label === label && item.visible && !item.disabled);
    check(matches.length === 1, `Expected one visible enabled UI item named ${label}; found ${matches.length}`);
    return this.ui('click', { item_id: matches[0].id });
  }

  async *watch({ intervalMs = 100, durationMs = 5000, signal } = {}) {
    check(Number.isInteger(intervalMs) && intervalMs >= 50 && intervalMs <= 10000, 'Live interval must be 50..10000 ms');
    check(Number.isInteger(durationMs) && durationMs >= 1 && durationMs <= 600000, 'Live duration must be 1..600000 ms');
    const end = Date.now() + durationMs;
    let nextSample = Date.now();
    do {
      if (signal?.aborted) throw signal.reason || new Error('Live stream aborted');
      let sample;
      try { sample = { kind: 'sample', wall_ms: Date.now(), ...await this.request('live', {}, { timeoutMs: 2000 }), received_wall_ms: Date.now() }; }
      catch (error) {
        sample = { kind: 'source_unavailable', wall_ms: Date.now(), error: error.message };
        await fs.appendFile(path.join(this.directory, 'live.jsonl'), `${JSON.stringify(sample)}\n`);
        yield sample;
        return;
      }
      await fs.appendFile(path.join(this.directory, 'live.jsonl'), `${JSON.stringify(sample)}\n`);
      yield sample;
      if (Date.now() >= end) return;
      // Schedule request starts, not an extra interval after every response.
      // Slow frames lower the effective rate without queuing catch-up bursts.
      nextSample = Math.max(nextSample + intervalMs, Date.now());
      await delay(Math.max(1, Math.min(nextSample, end) - Date.now()), undefined, { signal });
    } while (Date.now() < end);
  }

  async recordPerformance(milliseconds, { signal } = {}) {
    check(Number.isInteger(milliseconds) && milliseconds >= 100 && milliseconds <= 600000, 'Capture duration must be 100..600000 ms');
    check(!this.capturing, 'A performance capture is already active');
    await this.request('performance_begin');
    this.capturing = true;
    let interruption = null;
    try { await delay(milliseconds, undefined, { signal }); }
    catch (error) { interruption = error; }
    let result;
    try { result = await this.request('performance_end'); }
    finally { this.capturing = false; }
    if (interruption) {
      result.valid = false;
      result.invalid_reasons.push('client_capture_interrupted');
    }
    let summary = null;
    try { summary = summarizePerformance(result); }
    catch (error) {
      result.valid = false;
      result.invalid_reasons = [...(result.invalid_reasons || []), 'invalid_or_missing_presentation_samples'];
      result.summary_error = error.message;
    }
    const report = { ...result, summary, comparison_verdict: 'NOT_EVALUATED' };
    await writeNewJson(path.join(this.directory, `performance-${Date.now()}-${randomBytes(4).toString('hex')}.json`), report);
    if (interruption) throw interruption;
    return report;
  }

  async note(text, { verdict = 'OBSERVED' } = {}) {
    check(typeof text === 'string' && text.length > 0 && text.length <= 8192, 'Observation note must be 1..8192 characters');
    check(['OBSERVED', 'PASS', 'FAIL', 'UNCERTAIN'].includes(verdict), 'Invalid visual verdict');
    check(this.observation, 'Capture a current observation before annotating it');
    await fs.appendFile(path.join(this.directory, 'notes.jsonl'), `${JSON.stringify({ wall_ms: Date.now(), verdict, text,
      observation_id: this.observation.observation_id, screenshot: this.observation.screenshot })}\n`);
  }

  async verifySources() {
    const changed = [];
    if (await sha256(this.manifest.executable) !== this.manifest.executable_sha256) changed.push(this.manifest.executable);
    if (this.manifest.game && await sha256(this.manifest.game) !== this.manifest.game_sha256) changed.push(this.manifest.game);
    for (const seed of this.manifest.seeds) if (await sha256(seed.source) !== seed.sha256) changed.push(seed.source);
    check(changed.length === 0, `Original inputs changed: ${changed.join(', ')}`);
    return { executable_and_seed_hashes_unchanged: true, seed_files: this.manifest.seeds.length };
  }
}

async function main(argv) {
  const [command, ...args] = argv;
  if (!command || command === '--help') {
    console.log('Hollycast interactive testing harness\n\n'
      + 'launch --exe PATH --out NEW_DIRECTORY [--game PATH] [--firmware-dir PATH] [--profile JSON] [--revision TEXT]\n'
      + 'benchmark --exe PATH --game PATH --out NEW_DIRECTORY [--scenario current] [--control external] [--seconds 60] [--warmup 5]\n'
      + 'benchmark --run DIRECTORY [--scenario current] [--control external] [--seconds 60]\n'
      + 'playtest --run DIRECTORY --plan PLAN_JSON\n'
      + 'status --run DIRECTORY\n'
      + 'doctor|live|observe|screenshot|menu|release|stop|verify --run DIRECTORY\n'
      + 'job --run DIRECTORY --out NEW_JOB_JSON [--workspace PATH] [--objective TEXT] [--milliseconds 300000]\n'
      + 'watch --run DIRECTORY [--milliseconds 5000] [--interval 100]\n'
      + 'profile --run DIRECTORY --file PROFILE_JSON\n'
      + 'click --run DIRECTORY --label "Settings"\n'
      + 'press --run DIRECTORY --button A [--port 0..3] [--milliseconds 120]\n'
      + 'controller|hold --run DIRECTORY --state JSON [--milliseconds 500]\n'
      + 'ui --run DIRECTORY --input JSON\n'
      + 'record --run DIRECTORY --milliseconds 30000\n'
      + 'memory --run DIRECTORY --offset 0 --length 32\n\n'
      + 'All control, UI inspection, capture, and telemetry use the native emulator bridge.');
    return;
  }
  const options = {};
  for (let i = 0; i < args.length; i += 2) {
    check(args[i]?.startsWith('--') && args[i + 1] !== undefined, 'Expected --option value');
    check(!Object.hasOwn(options, args[i]), `Duplicate option: ${args[i]}`);
    options[args[i]] = args[i + 1];
  }
  if (command === 'benchmark') {
    const { runBenchmark, benchmarkSettings } = await import('./benchmark.mjs');
    const seconds = Number(options['--seconds'] || 60), warmup = Number(options['--warmup'] ?? 5);
    const scenario = options['--scenario'] || 'current';
    const control = options['--control'] || (scenario === 'planet-harriers' ? 'scripted' : 'external');
    check(Number.isInteger(seconds) && seconds >= 1 && seconds <= 600, 'Benchmark seconds must be 1..600');
    check(Number.isInteger(warmup) && warmup >= 0 && warmup <= 120, 'Warmup seconds must be 0..120');
    check(['planet-harriers', 'current'].includes(scenario), 'Unknown benchmark scenario');
    check(['scripted', 'external', 'none'].includes(control) && (control !== 'scripted' || scenario === 'planet-harriers'), 'Invalid benchmark control policy');
    check(!options['--capture'] || ['true', 'false'].includes(options['--capture']), 'Capture must be true or false');
    const after = options['--after'] || (options['--run'] ? 'pause' : 'stop');
    check(['stop', 'pause', 'leave'].includes(after), 'After must be stop, pause, or leave');
    const stateProfile = options['--state-profile'] ? await readJson(options['--state-profile'], 65536) : undefined;
    let harness;
    if (options['--run']) harness = await HollycastHarness.attach(options['--run']);
    else {
      check(options['--exe'] && options['--game'] && options['--out'], 'Benchmark requires --exe, --game, and a new --out directory');
      harness = await HollycastHarness.prepare({ executable: options['--exe'], game: options['--game'], out: options['--out'],
        firmwareDirectory: options['--firmware-dir'], revision: options['--revision'] || 'unrecorded',
        profile: benchmarkSettings(options['--profile'] ? await readJson(options['--profile'], 65536) : {}, scenario) });
      await harness.launch();
    }
    let lastPhase, lastProgress = -Infinity;
    const cancellation = new AbortController();
    const cancel = () => cancellation.abort(new Error('Benchmark cancelled'));
    globalThis.process.once('SIGINT', cancel);
    globalThis.process.once('SIGTERM', cancel);
    const report = await runBenchmark(harness, { durationMs: seconds * 1000, warmupMs: warmup * 1000, scenario, control, after,
      signal: cancellation.signal,
      capture: options['--capture'] === 'true',
      stateProfile,
      onProgress: status => {
        if (status.phase !== lastPhase || Date.now() - lastProgress >= 5000) {
          console.log(JSON.stringify({ kind: 'benchmark_progress', ...status }));
          lastPhase = status.phase; lastProgress = Date.now();
        }
      } }).finally(() => {
      globalThis.process.removeListener('SIGINT', cancel);
      globalThis.process.removeListener('SIGTERM', cancel);
    });
    console.log(JSON.stringify({ kind: 'benchmark_result', valid: report.valid, errors: report.errors,
      conditions_stable: report.conditions_stable, warnings: report.warnings,
      elapsed_ms: report.native?.elapsed_ms, guest_frame_boundary_hz: report.native?.guest_frame_boundary_hz,
      presentation: report.presentation, osd: report.osd, artifacts: report.artifacts }));
    if (!report.valid) globalThis.process.exitCode = 2;
    return;
  }
  if (command === 'launch') {
    check(options['--exe'] && options['--out'], 'launch requires --exe and --out');
    const harness = await HollycastHarness.prepare({ executable: options['--exe'], out: options['--out'], game: options['--game'],
      firmwareDirectory: options['--firmware-dir'], profile: options['--profile'] ? await readJson(options['--profile'], 65536) : {},
      revision: options['--revision'] || 'unrecorded' });
    const ready = await harness.launch();
    console.log(JSON.stringify({ directory: harness.directory, pid: ready.pid, capabilities: ready.capabilities }));
    return;
  }
  if (command === 'playtest') {
    check(options['--run'] && options['--plan'], 'playtest requires --run and --plan');
    const { validatePlaytestPlan, runPlaytestPlan } = await import('./playtest.mjs');
    const plan = await readJson(options['--plan'], 64 * 1024);
    validatePlaytestPlan(plan);
    const harness = await HollycastHarness.attach(options['--run']);
    const cancellation = new AbortController();
    const cancel = () => cancellation.abort(new Error('Playtest cancelled'));
    globalThis.process.once('SIGINT', cancel);
    globalThis.process.once('SIGTERM', cancel);
    let report;
    try {
      report = await runPlaytestPlan(harness, plan, { signal: cancellation.signal,
        onProgress: progress => console.log(JSON.stringify({ kind: 'playtest_progress', ...progress })) });
    } finally {
      globalThis.process.removeListener('SIGINT', cancel);
      globalThis.process.removeListener('SIGTERM', cancel);
    }
    console.log(JSON.stringify({ kind: 'playtest_result', outcome: report.outcome,
      plan_id: report.plan_id, report_directory: report.report_directory, artifacts: report.artifacts }));
    if (report.outcome !== 'PASS') globalThis.process.exitCode = 2;
    return;
  }
  check(options['--run'], 'Command requires --run');
  const harness = await HollycastHarness.attach(options['--run']);
  let result;
  switch (command) {
    case 'status': {
      result = await readJson(path.join(harness.directory, 'benchmark-status.json'));
      result.status_age_ms = Math.max(0, Date.now() - result.updated_wall_ms);
      break;
    }
    case 'doctor': result = await harness.doctor(); break;
    case 'job':
      check(options['--out'], 'job requires --out');
      result = await harness.createPlaytestJob({ out: options['--out'], objective: options['--objective'],
        durationMs: Number(options['--milliseconds'] || 300000), workspace: options['--workspace'] || globalThis.process.cwd() });
      break;
    case 'observe': result = await harness.observe(); break;
    case 'screenshot': result = await harness.observe({ screenshot: true }); break;
    case 'live': result = await harness.live(); break;
    case 'profile': result = await harness.setWatchProfile(await readJson(options['--file'], 65536)); break;
    case 'click': result = await harness.click(options['--label']); break;
    case 'watch':
      for await (const sample of harness.watch({ durationMs: Number(options['--milliseconds'] || 5000), intervalMs: Number(options['--interval'] || 100) })) {
        console.log(JSON.stringify(sample));
        if (sample.kind === 'source_unavailable') globalThis.process.exitCode = 2;
      }
      return;
    case 'menu': result = await harness.menu(); break;
    case 'release': result = await harness.release(); break;
    case 'stop': result = await harness.stop(); break;
    case 'verify': result = await harness.verifySources(); break;
    case 'press': result = await harness.press(options['--button'], Number(options['--milliseconds'] || 120), Number(options['--port'] || 0)); break;
    case 'controller':
    case 'hold': {
      check(options['--state']?.length <= 4096, 'Controller state requires JSON of at most 4096 characters');
      const input = JSON.parse(options['--state']);
      result = command === 'hold' ? await harness.hold(input, Number(options['--milliseconds'] || 500)) : await harness.setController(input);
      break;
    }
    case 'ui': {
      check(options['--input']?.length <= 4096, 'UI input requires JSON of at most 4096 characters');
      const input = JSON.parse(options['--input']);
      check(input && typeof input === 'object' && !Array.isArray(input), 'UI input must be an object');
      const { action, ...fields } = input;
      await harness.observe();
      result = await harness.ui(action, fields);
      break;
    }
    case 'record': {
      const report = await harness.recordPerformance(Number(options['--milliseconds']));
      result = report.summary || { valid: false, invalid_reasons: report.invalid_reasons };
      if (!report.valid) globalThis.process.exitCode = 2;
      break;
    }
    case 'memory': result = await harness.readMemory(Number(options['--offset']), Number(options['--length']), options['--region'] || 'main_ram'); break;
    default: throw new Error(`Unknown command: ${command}`);
  }
  console.log(JSON.stringify(result ?? { ok: true }, null, 2));
}

if (globalThis.process?.argv?.[1] && import.meta.url === pathToFileURL(path.resolve(globalThis.process.argv[1])).href) {
  main(globalThis.process.argv.slice(2)).catch(error => { console.error(error.message); globalThis.process.exitCode = 2; });
}
