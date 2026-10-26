// Portions Copyright 2026 The Hollycast Authors
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { buildCatalog, platformFor, releaseBuild } from './catalog.mjs';

test('historical development tags remain development releases', () => {
  for (const tag of ['v0.1.1-dev.1', 'v0.1.0-beta.1.dev.1', 'v2.0-rc1']) {
    assert.equal(releaseBuild({ tag_name: tag, assets: [] }).channel, 'development');
  }
  assert.equal(releaseBuild({ tag_name: 'v0.1.1', assets: [] }).channel, 'stable');
});

test('platforms preserve libretro host and UWP distinctions', () => {
  for (const [name, expected] of Object.entries({
    'Hollycast-release.apk': 'Android', 'Hollycast-linux-libretro-arm64.zip': 'Linux',
    'Hollycast-x86_64-w64-mingw32': 'Windows', 'Hollycast-apple-darwin': 'macOS',
    'flycast-uwp-ARM64': 'UWP', 'Hollycast-switch.nro': 'Switch',
  })) assert.equal(platformFor(name), expected);
});

test('catalog excludes expired, fork, PR and failed artifacts and compares matching branch/workflow', async () => {
  const now = new Date('2026-10-26T12:00:00Z');
  const artifact = (id, extra = {}) => ({
    id, name: 'Hollycast-release.apk', size_in_bytes: 2000, expired: false,
    expires_at: '2026-11-01T00:00:00Z',
    workflow_run: { id, head_branch: 'dev', head_repository_id: 1, repository_id: 1 }, ...extra,
  });
  const run = id => ({
    event: id === 4 ? 'pull_request' : 'push', status: 'completed', conclusion: id === 5 ? 'failure' : 'success',
    repository: { full_name: 'nexus382/Hollycast' }, head_repository: { full_name: 'nexus382/Hollycast' },
    head_branch: 'dev', name: 'Android', workflow_id: 10, run_number: id,
    head_sha: `${id}`.repeat(40), run_started_at: `2026-10-${20 + id}T00:00:00Z`,
    html_url: `https://github.com/nexus382/Hollycast/actions/runs/${id}`, head_commit: { message: `Fix ${id}` },
  });
  const requested = [];
  const api = async path => {
    requested.push(path);
    if (path.includes('/releases?')) return [];
    if (path.includes('/artifacts?')) return { artifacts: [artifact(1), artifact(2),
      artifact(3, { expired: true }), artifact(4), artifact(5),
      artifact(6, { workflow_run: { id: 6, head_branch: 'dev', head_repository_id: 2, repository_id: 1 } }),
      artifact(7, { expires_at: '2026-10-25T00:00:00Z' })] };
    if (path.includes('/compare/')) return { status: 'ahead', total_commits: 1, commits: [{ commit: { message: 'Fix playback\n\nKeep audio synchronized.' } }] };
    return run(Number(path.split('/').at(-1)));
  };
  const result = await buildCatalog('nexus382/Hollycast', api, now);
  assert.equal(result.builds.length, 2);
  assert.match(result.builds[0].notes, /Fix playback/);
  assert.match(result.builds[0].compare, /1111.*\.\.\.2222/);
  assert.match(result.builds[1].notes, /No earlier build/);
  assert.equal(requested.some(path => /runs\/(3|6|7)$/.test(path)), false);
});

test('API failures stop publication instead of replacing a working catalog', async () => {
  await assert.rejects(buildCatalog('nexus382/Hollycast', async () => { throw new Error('API unavailable'); }), /API unavailable/);
});
