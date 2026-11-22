// Portions Copyright 2026 The Hollycast Authors
import * as fs from 'node:fs/promises';
import path from 'node:path';
import { randomBytes } from 'node:crypto';
import { setTimeout as delay } from 'node:timers/promises';
import { controllerState, validateMappingArgs, validateSettingsPage, validateUiClickOptions, validateUiTarget, validateUiExpectedFields } from './hollytest.mjs';
import { publishJson, redactUiEvidence } from './evidence.mjs';
import { hasFreshTelemetry } from './telemetry.mjs';

const PLAN_SCHEMA = 'hollytest.playtest-plan.v1';
const MAX_PLAN_STEPS = 64;
const MAX_PLAN_TIMEOUT_MS = 15 * 60 * 1000;
const MAX_WAIT_TIMEOUT_MS = 10 * 60 * 1000;
const ID = /^[a-zA-Z0-9][a-zA-Z0-9._-]{0,63}$/;
const HASH = /^[a-f0-9]{64}$/;
const ASSERTION_OPERATORS = new Set(['eq', 'ne', 'gt', 'gte', 'lt', 'lte', 'is_true', 'is_false']);
const UI_ACTIONS = new Set(['key', 'text', 'scroll', 'drag']);

function check(condition, message) {
  if (!condition) throw new Error(message);
}

function object(value, name) {
  check(value !== null && typeof value === 'object' && !Array.isArray(value), `${name} must be an object`);
}

function fields(value, allowed, name) {
  object(value, name);
  for (const key of Object.keys(value)) check(allowed.has(key), `Unknown ${name} field: ${key}`);
}

function boundedInteger(value, minimum, maximum, name) {
  check(Number.isInteger(value) && value >= minimum && value <= maximum, `${name} must be ${minimum}..${maximum}`);
}

function planId(value, name) {
  check(typeof value === 'string' && ID.test(value), `${name} must be a short identifier`);
}

function safePath(pathValue) {
  check(Array.isArray(pathValue) && pathValue.length >= 1 && pathValue.length <= 8, 'Assertion path must contain 1..8 keys');
  for (const part of pathValue) {
    check(typeof part === 'string' && /^[a-zA-Z0-9_.-]{1,64}$/.test(part)
      && !['__proto__', 'prototype', 'constructor'].includes(part), 'Assertion path contains an invalid key');
  }
  if (pathValue[0] === 'telemetry' && pathValue[1] === 'game_state' && pathValue[2] === 'values') {
    check(pathValue.length === 5 && pathValue[4] === 'value',
      'Mapped game assertions must address telemetry.game_state.values.<name>.value');
    return true;
  }
  check(pathValue[0] === 'state' || pathValue[0] === 'telemetry', 'Assertion paths must start at state or telemetry');
  return false;
}

function validateAssertion(assertion, identity) {
  fields(assertion, new Set(['path', 'op', 'value']), 'assertion');
  const mapped = safePath(assertion.path);
  check(ASSERTION_OPERATORS.has(assertion.op), 'Unsupported assertion operator');
  if (mapped) check(typeof identity.profile_id === 'string', 'Mapped game assertions require identity.profile_id');

  if (assertion.op === 'is_true' || assertion.op === 'is_false') {
    check(!Object.hasOwn(assertion, 'value'), `${assertion.op} does not accept a value`);
  } else {
    check(Object.hasOwn(assertion, 'value'), `${assertion.op} requires a value`);
    const value = assertion.value;
    check(['string', 'boolean'].includes(typeof value)
      || (typeof value === 'number' && Number.isFinite(value)), 'Assertion value must be a finite JSON scalar');
    if (['gt', 'gte', 'lt', 'lte'].includes(assertion.op))
      check(typeof value === 'number', `${assertion.op} requires a numeric value`);
  }
}

function validateUiStep(step) {
  check(UI_ACTIONS.has(step.action), 'Unsupported native UI action');
  if (step.action === 'key') {
    fields(step, new Set(['id', 'op', 'action', 'key']), 'ui step');
    check(typeof step.key === 'string' && step.key.length >= 1 && step.key.length <= 64, 'UI key must contain 1..64 characters');
  } else if (step.action === 'text') {
    fields(step, new Set(['id', 'op', 'action', 'text']), 'ui step');
    check(typeof step.text === 'string' && step.text.length >= 1 && step.text.length <= 4096
      && !step.text.includes('\0'), 'UI text must contain 1..4096 characters without NUL');
  } else if (step.action === 'scroll') {
    fields(step, new Set(['id', 'op', 'action', 'x', 'y', 'delta_x', 'delta_y']), 'ui step');
    boundedInteger(step.x, 0, 8191, 'UI x');
    boundedInteger(step.y, 0, 8191, 'UI y');
    boundedInteger(step.delta_x, -100, 100, 'UI horizontal scroll');
    boundedInteger(step.delta_y, -100, 100, 'UI vertical scroll');
  } else {
    fields(step, new Set(['id', 'op', 'action', 'from_x', 'from_y', 'to_x', 'to_y', 'duration_ms']), 'ui step');
    for (const key of ['from_x', 'from_y', 'to_x', 'to_y']) boundedInteger(step[key], 0, 8191, `UI ${key}`);
    boundedInteger(step.duration_ms, 80, 5000, 'UI drag duration');
  }
}

export function validatePlaytestPlan(plan) {
  fields(plan, new Set(['schema', 'id', 'scope', 'identity', 'timeout_ms', 'steps']), 'plan');
  check(plan.schema === PLAN_SCHEMA, `Plan schema must be ${PLAN_SCHEMA}`);
  planId(plan.id, 'Plan id');
  const scope = plan.scope ?? 'game';
  check(scope === 'game' || scope === 'ui', 'Plan scope must be game or ui');
  fields(plan.identity, new Set(['game_id', 'profile_id', 'game_sha256', 'executable_sha256']), 'identity');
  if (scope === 'game' || Object.hasOwn(plan.identity, 'game_id')) {
    check(typeof plan.identity.game_id === 'string' && plan.identity.game_id.length >= 1
      && plan.identity.game_id.length <= 128 && !/[\u0000-\u001f\u007f]/.test(plan.identity.game_id),
    'identity.game_id must be a nonempty literal without control characters');
  }
  if (scope === 'ui') {
    check(typeof plan.identity.executable_sha256 === 'string' && HASH.test(plan.identity.executable_sha256),
      'UI scope requires identity.executable_sha256');
  }
  if (Object.hasOwn(plan.identity, 'profile_id')) {
    planId(plan.identity.profile_id, 'identity.profile_id');
    check(typeof plan.identity.game_id === 'string', 'identity.profile_id requires identity.game_id');
  }
  if (Object.hasOwn(plan.identity, 'game_sha256'))
    check(typeof plan.identity.game_sha256 === 'string' && HASH.test(plan.identity.game_sha256), 'identity.game_sha256 must be SHA-256');
  if (Object.hasOwn(plan.identity, 'executable_sha256'))
    check(typeof plan.identity.executable_sha256 === 'string' && HASH.test(plan.identity.executable_sha256), 'identity.executable_sha256 must be SHA-256');
  boundedInteger(plan.timeout_ms, 1000, MAX_PLAN_TIMEOUT_MS, 'Plan timeout');
  check(Array.isArray(plan.steps) && plan.steps.length >= 1 && plan.steps.length <= MAX_PLAN_STEPS,
    `Plan must contain 1..${MAX_PLAN_STEPS} steps`);

  const ids = new Set();
  let declaredDurationMs = 0;
  let screenshots = 0;
  let hasMappedAssertions = false;
  for (const step of plan.steps) {
    object(step, 'step');
    planId(step.id, 'Step id');
    check(!ids.has(step.id), `Duplicate step id: ${step.id}`);
    ids.add(step.id);
    check(typeof step.op === 'string', `Step ${step.id} requires op`);

    if (step.op === 'observe') {
      fields(step, new Set(['id', 'op', 'assertions']), 'observe step');
      if (Object.hasOwn(step, 'assertions')) {
        check(Array.isArray(step.assertions) && step.assertions.length >= 1 && step.assertions.length <= 32,
          'observe assertions must contain 1..32 entries');
        for (const assertion of step.assertions) {
          validateAssertion(assertion, plan.identity);
          hasMappedAssertions ||= safePath(assertion.path);
        }
      }
    } else if (step.op === 'wait_for') {
      fields(step, new Set(['id', 'op', 'timeout_ms', 'interval_ms', 'assertions']), 'wait_for step');
      boundedInteger(step.timeout_ms ?? 30000, 100, MAX_WAIT_TIMEOUT_MS, 'wait_for timeout');
      boundedInteger(step.interval_ms ?? 250, 50, 10000, 'wait_for interval');
      check(Array.isArray(step.assertions) && step.assertions.length >= 1 && step.assertions.length <= 32,
        'wait_for assertions must contain 1..32 entries');
      declaredDurationMs += step.timeout_ms ?? 30000;
      for (const assertion of step.assertions) {
        validateAssertion(assertion, plan.identity);
        hasMappedAssertions ||= safePath(assertion.path);
      }
    } else if (step.op === 'press') {
      fields(step, new Set(['id', 'op', 'button', 'port', 'duration_ms']), 'press step');
      check(typeof step.button === 'string', 'press requires a button');
      boundedInteger(step.port ?? 0, 0, 3, 'press port');
      boundedInteger(step.duration_ms ?? 120, 1, 600000, 'press duration');
      controllerState({ port: step.port ?? 0, buttons: [step.button] });
      declaredDurationMs += step.duration_ms ?? 120;
    } else if (step.op === 'hold') {
      fields(step, new Set(['id', 'op', 'state', 'duration_ms']), 'hold step');
      object(step.state, 'hold state');
      controllerState(step.state);
      boundedInteger(step.duration_ms, 1, 600000, 'hold duration');
      declaredDurationMs += step.duration_ms;
    } else if (step.op === 'controller') {
      fields(step, new Set(['id', 'op', 'state']), 'controller step');
      object(step.state, 'controller state');
      controllerState(step.state);
    } else if (step.op === 'release' || step.op === 'screenshot') {
      fields(step, new Set(['id', 'op']), `${step.op} step`);
      if (step.op === 'screenshot') screenshots++;
    } else if (step.op === 'click') {
      fields(step, new Set(['id', 'op', 'label', 'target', 'modifiers', 'mouse_button', 'click_count']), 'click step');
      check(Object.hasOwn(step, 'label') !== Object.hasOwn(step, 'target'), 'click requires exactly one of label or target');
      validateUiTarget(step.target ?? step.label);
      validateUiClickOptions({ ...(step.modifiers === undefined ? {} : { modifiers: step.modifiers }),
        ...(step.mouse_button === undefined ? {} : { mouse_button: step.mouse_button }),
        ...(step.click_count === undefined ? {} : { click_count: step.click_count }) });
    } else if (step.op === 'settings') {
      fields(step, new Set(['id', 'op', 'page']), 'settings step');
      validateSettingsPage(step.page);
    } else if (step.op === 'mapping') {
      fields(step, new Set(['id', 'op', 'input']), 'mapping step');
      validateMappingArgs(step.input);
    } else if (step.op === 'fill') {
      fields(step, new Set(['id', 'op', 'target', 'text', 'commit', 'modifiers']), 'fill step');
      validateUiTarget(step.target);
      if (Object.hasOwn(step, 'modifiers')) validateUiClickOptions({ modifiers: step.modifiers });
      check(typeof step.text === 'string' && Buffer.byteLength(step.text, 'utf8') <= 4096 && !step.text.includes('\0'),
        'fill text must be at most 4096 UTF-8 bytes without NUL');
      if (Object.hasOwn(step, 'commit')) check(typeof step.commit === 'boolean', 'fill commit must be boolean');
    } else if (step.op === 'set_toggle') {
      fields(step, new Set(['id', 'op', 'target', 'value']), 'set_toggle step');
      validateUiTarget(step.target);
      check(typeof step.value === 'boolean', 'set_toggle value must be boolean');
    } else if (step.op === 'expect_ui') {
      fields(step, new Set(['id', 'op', 'target', 'expected']), 'expect_ui step');
      validateUiTarget(step.target);
      validateUiExpectedFields(step.expected);
    } else if (step.op === 'scroll_window') {
      fields(step, new Set(['id', 'op', 'window', 'delta_x', 'delta_y']), 'scroll_window step');
      check(typeof step.window === 'string' && step.window.length >= 1 && step.window.length <= 128,
        'scroll_window requires a window name');
      boundedInteger(step.delta_x ?? 0, -100, 100, 'Horizontal scroll');
      boundedInteger(step.delta_y, -100, 100, 'Vertical scroll');
    } else if (step.op === 'ui') {
      validateUiStep(step);
    } else {
      throw new Error(`Unsupported playtest operation: ${step.op}`);
    }
  }
  check(screenshots <= 4, 'A plan may request at most four screenshots');
  check(declaredDurationMs <= plan.timeout_ms, 'Declared step durations exceed the plan timeout');
  if (hasMappedAssertions) check(typeof plan.identity.profile_id === 'string', 'Mapped game assertions require identity.profile_id');
  if (scope === 'ui') {
    const allowed = new Set(['settings', 'mapping', 'click', 'ui', 'fill', 'set_toggle', 'expect_ui', 'scroll_window', 'release', 'screenshot']);
    check(plan.steps.every(step => allowed.has(step.op)), 'UI scope permits only native settings and UI operations');
  }
  return plan;
}

class PlaytestResultError extends Error {
  constructor(message, outcome = 'FAIL', details = null) {
    super(message);
    this.name = 'PlaytestResultError';
    this.outcome = outcome;
    this.details = details;
  }
}

function isFreshObservation(sample, plan) {
  if (!sample || typeof sample !== 'object' || !sample.state || !sample.telemetry)
    return { fresh: false, reason: 'live_observation_missing' };
  const telemetry = sample.telemetry;
  if (!hasFreshTelemetry(sample))
    return { fresh: false, reason: 'live_telemetry_stale_or_unavailable' };
  if (sample.state.game_id !== plan.identity.game_id)
    return { fresh: false, reason: 'game_identity_mismatch' };
  if (plan.identity.profile_id && telemetry.game_state?.profile_id !== plan.identity.profile_id)
    return { fresh: false, reason: 'profile_identity_mismatch' };
  if (plan.identity.profile_id && telemetry.game_state?.status !== 'matched_operator_mapping')
    return { fresh: false, reason: 'game_mapping_unverified' };
  return { fresh: true };
}

function resolveAssertion(sample, assertion, plan) {
  const freshness = isFreshObservation(sample, plan);
  if (!freshness.fresh) return { status: 'unknown', reason: freshness.reason };

  let value;
  const keys = assertion.path;
  if (keys[0] === 'telemetry' && keys[1] === 'game_state' && keys[2] === 'values') {
    const gameState = sample.telemetry.game_state;
    if (gameState?.status !== 'matched_operator_mapping' || gameState.profile_id !== plan.identity.profile_id)
      return { status: 'unknown', reason: 'game_mapping_unverified' };
    const field = gameState.values?.[keys[3]];
    if (!field || field.valid !== true || !Object.hasOwn(field, 'value') || field.value === null)
      return { status: 'unknown', reason: 'mapped_field_unavailable' };
    value = field.value;
  } else {
    value = sample;
    for (const key of keys) {
      if (value === null || typeof value !== 'object' || !Object.hasOwn(value, key))
        return { status: 'unknown', reason: 'assertion_value_unavailable' };
      value = value[key];
    }
  }
  if (value === null || (typeof value === 'number' && !Number.isFinite(value)))
    return { status: 'unknown', reason: 'assertion_value_unavailable' };
  if (typeof value === 'object') return { status: 'unknown', reason: 'assertion_value_not_scalar' };

  let passed;
  switch (assertion.op) {
    case 'eq': passed = value === assertion.value; break;
    case 'ne': passed = value !== assertion.value; break;
    case 'gt': passed = typeof value === 'number' && value > assertion.value; break;
    case 'gte': passed = typeof value === 'number' && value >= assertion.value; break;
    case 'lt': passed = typeof value === 'number' && value < assertion.value; break;
    case 'lte': passed = typeof value === 'number' && value <= assertion.value; break;
    case 'is_true': passed = value === true; break;
    case 'is_false': passed = value === false; break;
    default: return { status: 'unknown', reason: 'unsupported_assertion_operator' };
  }
  return { status: passed ? 'pass' : 'fail', actual: value, expected: assertion.value ?? null,
    path: keys, op: assertion.op };
}

function evaluateAssertions(sample, assertions, plan) {
  const results = assertions.map(assertion => resolveAssertion(sample, assertion, plan));
  return { results, passed: results.every(result => result.status === 'pass'),
    unknown: results.some(result => result.status === 'unknown'),
    failed: results.some(result => result.status === 'fail') };
}

function mappedAssertionNames(plan) {
  const names = new Set();
  for (const step of plan.steps) {
    for (const assertion of step.assertions ?? []) {
      if (assertion.path[0] === 'telemetry' && assertion.path[1] === 'game_state' && assertion.path[2] === 'values')
        names.add(assertion.path[3]);
    }
  }
  return names;
}

function abortError(signal) {
  if (signal?.aborted) throw signal.reason instanceof Error ? signal.reason : new Error('Playtest cancelled');
}

function requireUiOperationPass(result, operation) {
  if (result?.outcome === 'PASS') return result;
  const outcome = result?.outcome === 'FAIL' ? 'FAIL' : 'INCONCLUSIVE';
  throw new PlaytestResultError(`${operation} did not pass: ${result?.reason ?? 'native readback unavailable'}`, outcome, { result });
}

async function createReportDirectory(sessionDirectory) {
  const name = `playtest-${Date.now()}-${randomBytes(6).toString('hex')}`;
  const directory = path.join(sessionDirectory, name);
  await fs.mkdir(directory, { recursive: false });
  return { directory, relative: name };
}

async function runWaitFor(harness, step, plan, signal, deadline, onProgress) {
  const timeoutMs = step.timeout_ms ?? 30000;
  const intervalMs = step.interval_ms ?? 250;
  const end = Math.min(Date.now() + timeoutMs, deadline);
  let polls = 0;
  let lastObservation = null;
  let lastEvaluation = null;
  let lastProgress = 0;
  while (Date.now() < end) {
    abortError(signal);
    lastObservation = await harness.live();
    polls++;
    abortError(signal);
    if (Date.now() >= end) break;
    lastEvaluation = evaluateAssertions(lastObservation, step.assertions, plan);
    if (lastEvaluation.passed) return { polls, observation: lastObservation, assertions: lastEvaluation.results };
    if (Date.now() - lastProgress >= 5000) {
      onProgress?.({ phase: 'waiting', step_id: step.id, polls, unknown: lastEvaluation.unknown });
      lastProgress = Date.now();
    }
    const remaining = end - Date.now();
    if (remaining <= 0) break;
    await delay(Math.min(intervalMs, remaining), undefined, { signal });
  }
  const outcome = lastEvaluation?.unknown ? 'INCONCLUSIVE' : 'FAIL';
  throw new PlaytestResultError(`wait_for timed out after ${polls} observations`, outcome, {
    polls, observation: lastObservation, assertions: lastEvaluation?.results ?? [] });
}

function evidencePlan(plan) {
  const saved = structuredClone(plan);
  for (const step of saved.steps) {
    if (step.op === 'fill' || (step.op === 'ui' && step.action === 'text')) {
      const text = step.text;
      step.text = '[redacted]';
      step.text_redacted = true;
      step.text_length = Buffer.byteLength(text, 'utf8');
    } else if (step.op === 'expect_ui') {
      let redacted = false;
      step.expected = Object.fromEntries(Object.entries(step.expected).map(([key, value]) => {
        if (typeof value !== 'string') return [key, value];
        redacted = true;
        return [key, '[redacted]'];
      }));
      if (redacted) step.expected_values_redacted = true;
    }
  }
  return saved;
}

async function runStep(harness, step, plan, signal, deadline, onProgress, markInputTouched, markInputReleased,
  markMappingCreated, markMappingCancelled) {
  abortError(signal);
  if (Date.now() >= deadline) throw new PlaytestResultError('Plan deadline elapsed', 'INCONCLUSIVE');
  switch (step.op) {
    case 'observe': {
      const observation = await harness.live();
      abortError(signal);
      const freshness = isFreshObservation(observation, plan);
      if (!freshness.fresh) throw new PlaytestResultError(freshness.reason, 'INCONCLUSIVE', { observation });
      if (!step.assertions) return { observation };
      const evaluation = evaluateAssertions(observation, step.assertions, plan);
      if (!evaluation.passed) {
        const outcome = evaluation.unknown ? 'INCONCLUSIVE' : 'FAIL';
        throw new PlaytestResultError('Observation assertions did not all pass', outcome,
          { observation, assertions: evaluation.results });
      }
      return { observation, assertions: evaluation.results };
    }
    case 'wait_for':
      return runWaitFor(harness, step, plan, signal, deadline, onProgress);
    case 'press': {
      markInputTouched(step.port ?? 0);
      await harness.hold({ port: step.port ?? 0, buttons: [step.button] }, step.duration_ms ?? 120, { signal, deadline });
      markInputReleased();
      abortError(signal);
      return { button: step.button, port: step.port ?? 0, duration_ms: step.duration_ms ?? 120 };
    }
    case 'hold': {
      markInputTouched(step.state.port ?? 0);
      await harness.hold(step.state, step.duration_ms, { signal, deadline });
      markInputReleased();
      abortError(signal);
      return { state: step.state, duration_ms: step.duration_ms };
    }
    case 'controller': {
      const state = controllerState(step.state);
      markInputTouched(state.port);
      const result = await harness.setController(step.state, { signal, deadline });
      abortError(signal);
      return { state: step.state, result };
    }
    case 'release': {
      const result = await harness.release();
      markInputReleased();
      abortError(signal);
      return { released: true, result };
    }
    case 'click': {
      const target = step.target ?? step.label;
      const options = { ...(step.modifiers === undefined ? {} : { modifiers: step.modifiers }),
        ...(step.mouse_button === undefined ? {} : { mouse_button: step.mouse_button }),
        ...(step.click_count === undefined ? {} : { click_count: step.click_count }) };
      const observation = await harness.click(target, options, { signal, deadline });
      abortError(signal);
      return { target: typeof target === 'string' ? { label: target } : target, observation };
    }
    case 'settings': {
      const result = await harness.settings(step.page, { signal, deadline });
      abortError(signal);
      return result;
    }
    case 'mapping': {
      const result = await harness.mapping(step.input, { signal, deadline });
      if (step.input.action === 'create') {
        markMappingCreated();
        if (result.registered !== true) throw new PlaytestResultError('Mapping device registration was not confirmed', 'INCONCLUSIVE', { result });
      }
      if (step.input.action === 'cancel') {
        if (result.cancelled !== true || result.detecting !== false)
          throw new PlaytestResultError('Mapping cancellation was not confirmed stopped', 'INCONCLUSIVE', { result });
        markMappingCancelled();
      }
      if (step.input.action === 'button' || step.input.action === 'axis') {
        const isRelease = step.input.action === 'button' && step.input.pressed === false;
        if (isRelease ? result.released !== true : result.delivered !== true)
          throw new PlaytestResultError('Mapping input was not confirmed delivered', 'INCONCLUSIVE', { result });
      }
      abortError(signal);
      return { action: step.input.action, result };
    }
    case 'fill': {
      const result = await harness.fill(step.target, step.text, { commit: step.commit ?? true, modifiers: step.modifiers ?? [], signal, deadline });
      abortError(signal);
      const safe = { outcome: result.outcome, reason: result.reason, target: result.target,
        committed: result.committed, verification: result.verification, post_effect_readback: result.post_effect_readback,
        text_length: result.text_length };
      if (safe.post_effect_readback === 'required') {
        const stepIndex = plan.steps.findIndex(candidate => candidate.id === step.id);
        if (plan.steps[stepIndex + 1]?.op !== 'expect_ui')
          throw new PlaytestResultError('Fill needs a following expect_ui post-effect assertion', 'INCONCLUSIVE', { result: safe });
      }
      return requireUiOperationPass(safe, 'fill');
    }
    case 'set_toggle': {
      const result = await harness.setToggle(step.target, step.value, { signal, deadline });
      abortError(signal);
      return requireUiOperationPass(result, 'set_toggle');
    }
    case 'expect_ui': {
      const result = await harness.expectUi(step.target, step.expected, { signal, deadline });
      abortError(signal);
      return requireUiOperationPass(result, 'expect_ui');
    }
    case 'scroll_window': {
      const result = await harness.scrollWindow(step.window, step.delta_y, { deltaX: step.delta_x ?? 0, signal, deadline });
      abortError(signal);
      return requireUiOperationPass(result, 'scroll_window');
    }
    case 'ui': {
      await harness.observe({ signal, deadline });
      abortError(signal);
      const { id, op, action, ...fields } = step;
      const observation = await harness.ui(action, fields, { signal, deadline });
      abortError(signal);
      return { action, observation };
    }
    case 'screenshot': {
      const observation = await harness.observe({ screenshot: true, signal, deadline });
      abortError(signal);
      if (!observation.screenshot)
        throw new PlaytestResultError('Native screenshot is unavailable', 'INCONCLUSIVE', { observation });
      return { screenshot: observation.screenshot };
    }
    default:
      throw new PlaytestResultError(`Unsupported playtest operation: ${step.op}`);
  }
}

function errorDetails(error) {
  if (error instanceof PlaytestResultError)
    return { outcome: error.outcome, message: error.message, details: redactUiEvidence(error.details ?? null) };
  if (/Command timed out; outcome is unknown|client lock is busy|Request deadline elapsed|Plan timeout elapsed|Playtest cancelled|aborted/i.test(error.message))
    return { outcome: 'INCONCLUSIVE', message: error.message, details: null };
  return { outcome: 'FAIL', message: error.message || String(error), details: null };
}

export async function runPlaytestPlan(harness, plan, { signal, onProgress } = {}) {
  validatePlaytestPlan(plan);
  check(harness && typeof harness.directory === 'string' && typeof harness.doctor === 'function'
    && typeof harness.verifySources === 'function', 'A live HollycastHarness session is required');

  const startedAt = new Date().toISOString();
  const startedMs = Date.now();
  const deadline = startedMs + plan.timeout_ms;
  const reportLocation = await createReportDirectory(harness.directory);
  const steps = plan.steps.map(step => ({ id: step.id, op: step.op, status: 'not_run' }));
  const report = {
    schema: 'hollytest.playtest-report.v1', plan_id: plan.id, outcome: 'INCONCLUSIVE',
    started_at: startedAt, finished_at: null, duration_ms: null,
    identity: { source_revision_declared: harness.manifest?.source_revision ?? null,
      executable_sha256: harness.manifest?.executable_sha256 ?? null,
      game_sha256: harness.manifest?.game_sha256 ?? null },
    artifacts: { report: path.join(reportLocation.relative, 'report.json'), steps: path.join(reportLocation.relative, 'steps.jsonl') },
    preflight: { status: 'not_run' }, steps, cleanup: { input_touched_ports: [], input_released: null,
      mapping_cancelled: null }, errors: [],
  };
  await publishJson(path.join(reportLocation.directory, 'plan.json'), evidencePlan(plan));

  const timeoutController = new AbortController();
  const timeout = setTimeout(() => timeoutController.abort(new Error('Plan timeout elapsed')), plan.timeout_ms);
  const onAbort = () => timeoutController.abort(signal.reason ?? new Error('Playtest cancelled'));
  signal?.addEventListener('abort', onAbort, { once: true });
  if (signal?.aborted) onAbort();
  const runSignal = timeoutController.signal;
  const touchedPorts = new Set();
  let inputTouched = false;
  let mappingCreated = false;
  const recordStep = async (stepRecord) => {
    await fs.appendFile(path.join(reportLocation.directory, 'steps.jsonl'), `${JSON.stringify(stepRecord)}\n`, { flag: 'a' });
  };

  let preflightPassed = false;
  try {
    try {
      abortError(runSignal);
      const doctor = await harness.doctor();
      abortError(runSignal);
      const sourceCheck = await harness.verifySources();
      abortError(runSignal);
      const initial = doctor.live;
      if (plan.scope === 'ui') {
        check(doctor.executable_sha256 === plan.identity.executable_sha256, 'Executable identity does not match the plan');
        if (plan.identity.game_id !== undefined)
          check(doctor.game_id === plan.identity.game_id, 'Attached session game_id does not match the plan');
        if (plan.identity.game_sha256)
          check(harness.manifest?.game_sha256 === plan.identity.game_sha256, 'Game content identity does not match the plan');
        if (plan.identity.profile_id)
          check(initial.telemetry?.game_state?.profile_id === plan.identity.profile_id
            && initial.telemetry?.game_state?.status === 'matched_operator_mapping', 'Attached session profile_id is not identity-matched');
        report.identity = { scope: 'ui', source_revision_declared: doctor.source_revision ?? null,
          executable_sha256: doctor.executable_sha256 ?? null, game_sha256: harness.manifest?.game_sha256 ?? null,
          game_id: doctor.game_id ?? null, profile_id: initial.telemetry?.game_state?.profile_id ?? null,
          verified_inputs: sourceCheck };
      } else {
        const freshness = isFreshObservation(initial, plan);
        check(freshness.fresh, freshness.reason || 'Initial live observation is unavailable');
        check(doctor.game_id === plan.identity.game_id, 'Attached session game_id does not match the plan');
        if (plan.identity.executable_sha256)
          check(doctor.executable_sha256 === plan.identity.executable_sha256, 'Executable identity does not match the plan');
        if (plan.identity.game_sha256)
          check(harness.manifest?.game_sha256 === plan.identity.game_sha256, 'Game content identity does not match the plan');
        if (plan.identity.profile_id)
          check(initial.telemetry.game_state?.status === 'matched_operator_mapping', 'The requested game profile is not identity-matched');
        for (const name of mappedAssertionNames(plan))
          check(Object.hasOwn(initial.telemetry.game_state?.values ?? {}, name), `Mapped field is not in the active profile: ${name}`);
        report.identity = { scope: 'game', source_revision_declared: doctor.source_revision ?? null,
          executable_sha256: doctor.executable_sha256 ?? null, game_sha256: harness.manifest?.game_sha256 ?? null,
          game_id: doctor.game_id ?? null, profile_id: initial.telemetry.game_state?.profile_id ?? null,
          verified_inputs: sourceCheck };
      }
      report.preflight = { status: 'pass', scope: plan.scope ?? 'game', capabilities: doctor.capabilities, verified_inputs: sourceCheck };
      preflightPassed = true;
    } catch (error) {
      const details = errorDetails(error);
      report.preflight = { status: 'fail', error: details.message };
      report.errors.push({ phase: 'preflight', ...details });
      report.outcome = details.outcome === 'FAIL' ? 'INCONCLUSIVE' : details.outcome;
    }

    for (let index = 0; preflightPassed && index < plan.steps.length; index++) {
      const step = plan.steps[index];
      const stepRecord = { id: step.id, op: step.op, status: 'not_run', started_at: new Date().toISOString() };
      onProgress?.({ phase: 'step_started', plan_id: plan.id, step_id: step.id, index: index + 1, total: plan.steps.length });
      await recordStep(stepRecord);
      try {
        const result = await runStep(harness, step, plan, runSignal, deadline,
          progress => onProgress?.({ ...progress, plan_id: plan.id }), port => {
            inputTouched = true;
            touchedPorts.add(port);
            report.cleanup.input_released = null;
          }, () => {
            inputTouched = false;
            report.cleanup.input_released = true;
          }, () => {
            mappingCreated = true;
            report.cleanup.mapping_cancelled = null;
          }, () => {
            mappingCreated = false;
            report.cleanup.mapping_cancelled = true;
          });
        stepRecord.status = 'pass';
        stepRecord.result = redactUiEvidence(result);
        stepRecord.finished_at = new Date().toISOString();
        steps[index] = stepRecord;
        await recordStep(stepRecord);
        onProgress?.({ phase: 'step_finished', plan_id: plan.id, step_id: step.id, status: 'pass' });
      } catch (error) {
        const details = errorDetails(error);
        if (step.op === 'fill' || (step.op === 'ui' && step.action === 'text')) {
          details.message = `${step.op} failed; text input is redacted`;
          details.details = null;
        }
        stepRecord.status = 'fail';
        stepRecord.outcome = details.outcome;
        stepRecord.error = details.message;
        if (details.details) stepRecord.details = details.details;
        stepRecord.finished_at = new Date().toISOString();
        steps[index] = stepRecord;
        report.outcome = details.outcome;
        report.errors.push({ phase: 'step', step_id: step.id, ...details });
        await recordStep(stepRecord);
        onProgress?.({ phase: 'step_finished', plan_id: plan.id, step_id: step.id, status: 'fail', outcome: details.outcome });
        break;
      }
    }
    if (report.outcome === 'INCONCLUSIVE' && steps.every(step => step.status === 'pass')) report.outcome = 'PASS';
  } finally {
    if (inputTouched) {
      try {
        await harness.release();
        report.cleanup.input_released = true;
      } catch (error) {
        report.cleanup.input_released = false;
        report.errors.push({ phase: 'cleanup', outcome: 'FAIL', message: error.message || String(error) });
        report.outcome = 'FAIL';
      }
    }
    if (mappingCreated) {
      try {
        const result = await harness.mapping({ action: 'cancel' });
        if (result.cancelled !== true || result.detecting !== false)
          throw new Error('Mapping cancellation was not confirmed stopped');
        report.cleanup.mapping_cancelled = true;
      } catch (error) {
        report.cleanup.mapping_cancelled = false;
        report.errors.push({ phase: 'cleanup', outcome: 'FAIL', message: error.message || String(error) });
        report.outcome = 'FAIL';
      }
    }
    report.cleanup.input_touched_ports = [...touchedPorts].sort((a, b) => a - b);
    report.finished_at = new Date().toISOString();
    report.duration_ms = Date.now() - startedMs;
    clearTimeout(timeout);
    signal?.removeEventListener('abort', onAbort);
    await publishJson(path.join(reportLocation.directory, 'report.json'), report);
  }
  return { ...report, report_directory: reportLocation.directory };
}
