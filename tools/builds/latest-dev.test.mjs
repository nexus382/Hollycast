// Portions Copyright 2026 The Hollycast Authors
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { publishLatestDev } from './latest-dev.mjs';

const repository = 'nexus382/Hollycast';
const base = `repos/${repository}`;
const commit = 'a'.repeat(40);
const oldCommit = 'b'.repeat(40);
const date = '2026-10-29T12:00:00Z';
const oldDate = '2026-10-28T12:00:00Z';
const packages = ['Hollycast-release.apk.zip', 'Hollycast-x86_64-w64-mingw32.zip.zip',
  'Hollycast-x86_64-pc-linux-gnu.zip', 'Hollycast-apple-darwin.zip.zip',
  'Hollycast-switch-standalone.zip', 'flycast-uwp-x64.zip'];
const filenames = ['Hollycast-android.apk.zip', 'Hollycast-windows-x64.zip', 'Hollycast-linux-x64.zip',
  'Hollycast-macos-universal.zip', 'Hollycast-switch.zip', 'Hollycast-uwp-x64.zip', 'SHA256SUMS.txt'];
const hash = text => `sha256:${createHash('sha256').update(text).digest('hex')}`;
const notFound = () => { throw Object.assign(new Error('Not found'), { status: 404 }); };

function builds(sha = commit, branch = 'dev', names = packages, timestamp = date) {
  return names.map((name, index) => {
    const id = index + (branch === 'dev' ? 1 : 10);
    const tag = `ci-${branch}-${id}`;
    const metadata = { branch, run: id, workflow: id, commit: sha, date: timestamp };
    return { id, tag_name: tag, draft: false, html_url: `https://github.com/${repository}/releases/tag/${tag}`,
      body: `<!-- hollycast-ci: ${JSON.stringify(metadata)} -->\n\nFix playback.`,
      assets: [{ id: id * 10, name: `${id}-${name}`, state: 'uploaded', size: 100, digest: hash(`${sha}:${name}`),
        browser_download_url: `https://github.com/${repository}/releases/download/${tag}/${name}` }],
    };
  });
}

function rolling(sha = oldCommit, timestamp = oldDate) {
  return { id: 500, tag_name: 'latest-dev', name: 'Latest Development Build', draft: false, assets: [],
    body: `<!-- hollycast-latest-dev: ${JSON.stringify({ commit: sha, date: timestamp })} -->\n` };
}

function fixture(releases = builds(), current = rolling()) {
  const state = { releases: structuredClone([...releases, ...(current ? [current] : [])]),
    ref: current ? { object: { type: 'commit', sha: oldCommit } } : null, events: [], uploads: [],
    failUpload: null, failMutation: null, corruptUpload: null };
  let nextId = 1000;
  const api = async (path, options = {}) => {
    if (options.method) {
      state.events.push({ path, ...options });
      if (state.failMutation?.(path, options, 'before')) throw new Error('GitHub unavailable');
    }
    let result;
    if (path.startsWith(`${base}/releases?`)) return structuredClone(state.releases);
    if (path.startsWith(`${base}/releases/tags/`)) {
      result = state.releases.find(item => !item.draft && item.tag_name === path.split('/').at(-1));
      if (!result) return notFound();
    } else if (path === `${base}/git/ref/tags/latest-dev`) {
      if (!state.ref) return notFound();
      result = state.ref;
    } else if (path === `${base}/git/refs` || path === `${base}/git/refs/tags/latest-dev`) {
      state.ref = options.method === 'DELETE' ? null : { object: { type: 'commit', sha: options.body.sha } };
      result = state.ref;
    } else if (path === `${base}/releases` && options.method === 'POST') {
      result = { id: nextId++, assets: [], ...options.body };
      state.releases.push(result);
    } else if (path.startsWith(`${base}/releases/assets/`) && options.method === 'DELETE') {
      for (const release of state.releases) release.assets = release.assets.filter(asset => asset.id !== Number(path.split('/').at(-1)));
    } else if (/\/releases\/\d+$/.test(path)) {
      result = state.releases.find(item => item.id === Number(path.split('/').at(-1)));
      assert.ok(result, `Unknown release: ${path}`);
      if (options.method === 'PATCH') Object.assign(result, options.body);
    } else assert.fail(`Unexpected API request: ${path}`);
    if (state.failMutation?.(path, options, 'after')) throw new Error('Response lost');
    return structuredClone(result);
  };
  const upload = async (release, asset, name) => {
    state.events.push({ upload: name });
    if (name === state.failUpload) throw new Error('Upload interrupted');
    state.uploads.push({ asset, name });
    const uploaded = { id: nextId++, name, size: asset.size, digest: asset.digest, state: 'uploaded' };
    state.corruptUpload?.(uploaded);
    state.releases.find(item => item.id === release.id).assets.push(uploaded);
  };
  return { state, api, upload, publish: () => publishLatestDev(repository, api, upload) };
}

test('each platform must be uploaded on dev at the same commit before rolling publication', async () => {
  for (let missing = 0; missing < packages.length; missing++) {
    const incomplete = builds();
    incomplete[missing].assets[0].state = 'starter';
    const otherCommit = builds('c'.repeat(40), 'dev', [packages[missing]]);
    const otherBranch = builds(commit, 'master', [packages[missing]]);
    const draft = builds(commit, 'dev', [packages[missing]]);
    draft[0].draft = true;
    const f = fixture([...incomplete, ...otherCommit, ...otherBranch, ...draft]);
    assert.equal(await f.publish(), null);
    assert.deepEqual(f.state.events, []);
    assert.equal(f.state.releases.find(item => item.tag_name === 'latest-dev').id, 500);
  }
  const f = fixture(builds(commit, 'master'));
  assert.equal(await f.publish(), null);
  assert.deepEqual(f.state.events, []);
});

test('a complete dev commit is staged with stable filenames and checksums before promotion', async () => {
  for (const current of [null, rolling()]) {
    const f = fixture([...builds(), ...builds('c'.repeat(40), 'master')], current);
    const result = await f.publish();
    assert.equal(result.tag_name, 'latest-dev');
    assert.equal(result.target_commitish, commit);
    assert.equal(result.prerelease, true);
    assert.equal(result.draft, false);
    assert.equal(result.make_latest, 'false');
    assert.equal(f.state.ref.object.sha, commit);
    assert.deepEqual(result.assets.map(item => item.name), filenames);
    assert.match(result.body, /\*\*dev\*\*/);
    assert.ok(result.body.includes(commit) && result.body.includes(date));
    const checksum = f.state.uploads.at(-1).asset.text;
    assert.equal(checksum.split('\n').filter(Boolean).length, 6);
    for (const { asset, name } of f.state.uploads.slice(0, 6)) assert.ok(checksum.includes(`${asset.digest.slice(7)}  ${name}\n`));
    const lastUpload = f.state.events.findLastIndex(event => event.upload);
    assert.ok(f.state.events.slice(0, lastUpload).every(event => !event.path?.includes('/git/')));
    if (current) {
      const archived = f.state.releases.find(item => item.id === current.id);
      assert.equal(archived.tag_name, `latest-dev-${oldCommit}`);
      assert.ok(f.state.events.findIndex(event => event.body?.tag_name === archived.tag_name) > lastUpload);
    }
  }
});

test('newer incomplete commits and older completions do not displace the latest complete dev build', async () => {
  const newer = builds('c'.repeat(40), 'dev', packages.slice(0, 5), '2026-10-30T12:00:00Z');
  const f = fixture([...newer, ...builds()], rolling(commit, date));
  assert.equal((await f.publish()).id, 500);
  assert.deepEqual(f.state.events, []);
  for (const timestamp of [date, '2026-10-30T12:00:00Z']) {
    const f = fixture(builds(), rolling(oldCommit, timestamp));
    assert.equal((await f.publish()).id, 500);
    assert.deepEqual(f.state.events, []);
  }
});

test('interrupted staging preserves the old release and retry resumes verified uploads', async () => {
  for (const name of filenames) {
    const f = fixture();
    f.state.failUpload = name;
    await assert.rejects(f.publish(), /Upload interrupted/);
    assert.equal(f.state.releases.find(item => item.tag_name === 'latest-dev').id, 500);
    assert.equal(f.state.ref.object.sha, oldCommit);
    assert.ok(f.state.releases.find(item => item.id === 1000).draft);
    f.state.failUpload = null;
    await f.publish();
    assert.deepEqual(f.state.uploads.map(item => item.name), filenames);
    assert.equal(f.state.releases.filter(item => item.tag_name === 'latest-dev').length, 1);
    assert.equal(f.state.ref.object.sha, commit);
  }
});

test('staged assets must match the source digests and sizes before replacing the old build', async () => {
  for (const corrupt of [asset => { asset.digest = hash('corrupt'); }, asset => { asset.size++; },
    asset => { asset.state = 'starter'; }]) {
    const f = fixture();
    f.state.corruptUpload = corrupt;
    await assert.rejects(f.publish(), /six verified downloads/);
    assert.equal(f.state.ref.object.sha, oldCommit);
    assert.equal(f.state.releases.find(item => item.tag_name === 'latest-dev').id, 500);
    await assert.rejects(f.publish(), /Staged asset mismatch|six verified downloads/);
  }
});

test('promotion failure restores the previous rolling release and tag, then permits retry', async () => {
  for (const phase of ['archive', 'tag', 'publish']) {
    const f = fixture();
    let failed = false;
    f.state.failMutation = (path, options, when) => {
      const match = phase === 'archive' ? options.body?.tag_name === `latest-dev-${oldCommit}` :
        phase === 'tag' ? path.endsWith('/git/refs/tags/latest-dev') : options.body?.draft === false;
      if (!failed && when === 'before' && match) { failed = true; return true; }
    };
    await assert.rejects(f.publish(), /GitHub unavailable/);
    assert.equal(f.state.ref.object.sha, oldCommit);
    assert.equal(f.state.releases.find(item => item.tag_name === 'latest-dev').id, 500);
    await f.publish();
    assert.equal(f.state.ref.object.sha, commit);
    assert.equal(f.state.uploads.length, 7);
  }
});

test('a lost publication response does not undo a successful promotion', async () => {
  const f = fixture();
  f.state.failMutation = (_, options, when) => when === 'after' && options.body?.draft === false;
  const result = await f.publish();
  assert.equal(result.id, 1000);
  assert.equal(f.state.ref.object.sha, commit);
});

test('initial promotion failures remove only the new rolling tag and leave a resumable draft', async () => {
  const f = fixture(builds(), null);
  f.state.failMutation = (_, options, when) => when === 'before' && options.body?.draft === false;
  await assert.rejects(f.publish(), /GitHub unavailable/);
  assert.equal(f.state.ref, null);
  assert.ok(f.state.releases.find(item => item.id === 1000).draft);
  f.state.failMutation = null;
  await f.publish();
  assert.equal(f.state.ref.object.sha, commit);
  assert.equal(f.state.uploads.length, 7);
});

test('a run interrupted after archiving can finish promotion on the next workflow run', async () => {
  for (const refCommit of [oldCommit, commit]) {
    const f = fixture();
    f.state.failUpload = 'SHA256SUMS.txt';
    await assert.rejects(f.publish(), /Upload interrupted/);
    f.state.failUpload = null;
    f.state.releases.find(item => item.id === 500).tag_name = `latest-dev-${oldCommit}`;
    f.state.ref.object.sha = refCommit;
    const result = await f.publish();
    assert.equal(result.tag_name, 'latest-dev');
    assert.equal(f.state.ref.object.sha, commit);
    assert.equal(f.state.uploads.length, 7);
  }
});

test('retry removes an empty starter upload from the development draft', async () => {
  const f = fixture();
  f.state.failUpload = filenames[0];
  await assert.rejects(f.publish(), /Upload interrupted/);
  f.state.releases.find(item => item.id === 1000).assets.push({ id: 900, name: filenames[0], state: 'starter' });
  f.state.failUpload = null;
  const result = await f.publish();
  assert.equal(result.assets.length, 7);
  assert.ok(f.state.events.some(event => event.path === `${base}/releases/assets/900` && event.method === 'DELETE'));
});

test('unrelated releases, tags and unverifiable source downloads are never promoted', async () => {
  const unrelated = fixture(builds(), { ...rolling(), body: 'Manual release' });
  await assert.rejects(unrelated.publish(), /unrelated release/);
  assert.deepEqual(unrelated.state.events, []);
  const missingDigest = fixture();
  delete missingDigest.state.releases[0].assets[0].digest;
  await assert.rejects(missingDigest.publish(), /Missing source asset or SHA256 digest/);
  assert.deepEqual(missingDigest.state.events, []);
  const unrelatedTag = fixture(builds(), null);
  unrelatedTag.state.ref = { object: { type: 'commit', sha: 'f'.repeat(40) } };
  await assert.rejects(unrelatedTag.publish(), /unrelated tag/);
  assert.equal(unrelatedTag.state.ref.object.sha, 'f'.repeat(40));
});
