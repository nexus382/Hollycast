// Portions Copyright 2026 The Hollycast Authors
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { archiveName, eligibleRun, publishBuilds, publishMergedPullRequest, publishRun, uploadArtifact, uploadReleaseAsset } from './publish.mjs';

const repository = 'nexus382/Hollycast';
function run(overrides = {}) {
  return {
    id: 123, event: 'push', status: 'completed', conclusion: 'success', path: '.github/workflows/android.yml',
    repository: { full_name: repository }, head_repository: { full_name: repository },
    head_branch: 'dev', head_sha: 'a'.repeat(40), workflow_id: 10, run_number: 9, name: 'Android',
    run_started_at: '2026-10-26T12:00:00Z', head_commit: { message: 'Fix playback' }, ...overrides,
  };
}
function release(overrides = {}) {
  const metadata = { branch: 'dev', run: 123, workflow: 10, commit: 'a'.repeat(40), date: '2026-10-26T12:00:00Z' };
  return {
    id: 99, tag_name: 'ci-dev-123', assets: [], draft: true,
    body: `<!-- hollycast-ci: ${JSON.stringify(metadata)} -->\n`, ...overrides,
  };
}
const artifact = { id: 456, name: 'Hollycast-release.apk', size_in_bytes: 100,
  expires_at: '2099-01-01T00:00:00Z', expired: false, digest: 'sha256:abc' };
const notFound = () => { throw Object.assign(new Error('Not found'), { status: 404 }); };

test('only successful same-repository master/dev platform pushes and dispatches can publish directly', async () => {
  for (const overrides of [
    { event: 'pull_request' }, { conclusion: 'failure' }, { status: 'in_progress' },
    { head_repository: { full_name: 'someone/fork' } }, { head_repository: null },
    { head_branch: 'feature' }, { path: '.github/workflows/untrusted.yml' }, { head_sha: 'invalid' },
  ]) {
    const candidate = run(overrides);
    assert.equal(eligibleRun(candidate, repository), false);
    assert.equal(await publishRun(repository, candidate, () => assert.fail('API must not be called'), () => assert.fail('No upload'), []), null);
  }
  assert.equal(eligibleRun(run({ head_branch: 'master' }), repository), true);
  assert.equal(eligibleRun(run({ event: 'workflow_dispatch' }), repository), true);
  assert.equal(eligibleRun(run({ event: 'workflow_dispatch', head_branch: 'ci-candidate/pr-23-example' }), repository), false);
});

test('draft publication targets the exact commit and happens only after all uploads', async () => {
  const steps = [];
  const api = async (path, options) => {
    if (path.includes('/tags/')) return notFound();
    if (path.includes('/artifacts?')) return { artifacts: [artifact, { ...artifact, id: 457 }, { ...artifact, expired: true }] };
    steps.push(options.method);
    if (options.method === 'POST') {
      assert.equal(options.body.target_commitish, run().head_sha);
      assert.equal(options.body.draft, true);
      assert.equal(options.body.prerelease, true);
      assert.equal(options.body.make_latest, 'false');
      return release();
    }
    assert.deepEqual(steps, ['POST', 'upload', 'upload', 'PATCH']);
    assert.equal(options.body.draft, false);
    return release({ draft: false });
  };
  const result = await publishRun(repository, run(), api, async () => { steps.push('upload'); }, []);
  assert.equal(result.draft, false);
});

test('failed uploads leave a draft and retries reuse uploaded assets', async () => {
  const existing = release({ assets: [{ name: archiveName(artifact), state: 'uploaded', digest: artifact.digest }] });
  let publishCount = 0;
  const api = async (path, options) => {
    if (path.includes('/tags/')) return notFound();
    if (path.includes('/artifacts?')) return { artifacts: [artifact, { ...artifact, id: 457 }] };
    assert.equal(options.method, 'PATCH');
    publishCount++;
    return release({ draft: false });
  };
  await assert.rejects(publishRun(repository, run(), api, async () => { throw new Error('Network failure'); }, [existing]), /Network failure/);
  assert.equal(publishCount, 0);
  const uploads = [];
  await publishRun(repository, run(), api, async (_, item) => uploads.push(item.id), [existing]);
  assert.deepEqual(uploads, [457]);
  assert.equal(publishCount, 1);
});

test('published builds are immutable and unrelated reserved tags are never overwritten', async () => {
  await publishRun(repository, run(), async path => {
    assert.match(path, /\/tags\//);
    return release({ draft: false });
  }, () => assert.fail('Already published'), []);
  await assert.rejects(publishRun(repository, run(), async () => release({ body: 'Unrelated release' }),
    () => assert.fail('Unrelated release'), []), /Refusing to change unrelated release/);
  await assert.rejects(publishRun(repository, run(), notFound, () => assert.fail('Unrelated draft'),
    [release({ body: 'Unrelated draft' })]), /Refusing to change unrelated release/);
});

test('empty assets from interrupted draft uploads are removed before retrying', async () => {
  const steps = [];
  await publishRun(repository, run(), async (path, options) => {
    if (path.includes('/tags/')) return release({ assets: [{ id: 22, name: archiveName(artifact), state: 'starter' }] });
    if (path.includes('/artifacts?')) return { artifacts: [artifact] };
    steps.push(options.method);
    if (options.method === 'DELETE') assert.match(path, /\/releases\/assets\/22$/);
    return release({ draft: false });
  }, async () => steps.push('upload'), []);
  assert.deepEqual(steps, ['DELETE', 'upload', 'PATCH']);
});

test('archive transfer verifies the digest and uploads original ZIP bytes without extraction', async () => {
  const bytes = Buffer.from([80, 75, 3, 4, 0, 1, 2, 3]);
  const digest = `sha256:${createHash('sha256').update(bytes).digest('hex')}`;
  for (const valid of [true, false]) {
    let uploads = 0;
    const transfer = uploadArtifact(repository, 'test-token', {
      upload_url: `https://uploads.github.com/repos/${repository}/releases/99/assets{?name,label}`,
    }, { ...artifact, digest: valid ? digest : artifact.digest }, archiveName(artifact), async (url, options) => {
      if (options.method !== 'POST') return new Response(bytes);
      uploads++;
      assert.equal(new URL(url).searchParams.get('name'), archiveName(artifact));
      assert.deepEqual(Buffer.from(await options.body.arrayBuffer()), bytes);
      return new Response('', { status: 201 });
    });
    if (valid) { await transfer; assert.equal(uploads, 1); }
    else { await assert.rejects(transfer, /digest mismatch/); assert.equal(uploads, 0); }
  }
});

test('changelogs compare earlier branch commits and tolerate history disappearing', async () => {
  const prior = release({ tag_name: 'ci-dev-100', draft: false,
    body: `<!-- hollycast-ci: ${JSON.stringify({ branch: 'dev', run: 100, workflow: 11, commit: 'b'.repeat(40), date: '2026-10-25T12:00:00Z' })} -->\n` });
  for (const gone of [false, true]) {
    let body;
    const api = async (path, options) => {
      if (path.includes('/tags/')) return notFound();
      if (path.includes('/artifacts?')) return { artifacts: [artifact] };
      if (path.includes('/compare/')) {
        if (gone) return notFound();
        return { status: 'ahead', total_commits: 1, commits: [{ commit: { message: 'Fix audio' } }] };
      }
      body = options.body.body;
      return release({ draft: options.method === 'POST' });
    };
    await publishRun(repository, run(), api, async () => {}, [prior]);
    assert.match(body, gone ? /previous commit is no longer available/ : /Fix audio/);
  }
});

test('rolling release transfers verify original release ZIPs and upload checksums as text', async () => {
  const bytes = Buffer.from('original ZIP bytes');
  const digest = `sha256:${createHash('sha256').update(bytes).digest('hex')}`;
  const release = { upload_url: `https://uploads.github.com/repos/${repository}/releases/99/assets{?name,label}` };
  let uploads = 0;
  const fetchImpl = async (url, options) => {
    if (options.method !== 'POST') {
      assert.equal(url, `https://api.github.com/repos/${repository}/releases/assets/123`);
      assert.equal(options.headers.Accept, 'application/octet-stream');
      return new Response(bytes);
    }
    uploads++;
    if (options.headers['Content-Type'] === 'text/plain') assert.equal(options.body, 'checksum text\n');
    else assert.deepEqual(Buffer.from(await options.body.arrayBuffer()), bytes);
    return new Response('', { status: 201 });
  };
  await uploadReleaseAsset(repository, 'test-token', release, { id: 123, digest, size: bytes.length }, 'Hollycast-linux-x64.zip', fetchImpl);
  await uploadReleaseAsset(repository, 'test-token', release, { text: 'checksum text\n' }, 'SHA256SUMS.txt', fetchImpl);
  assert.equal(uploads, 2);
  await assert.rejects(uploadReleaseAsset(repository, 'test-token', release, { id: 123, digest: artifact.digest }, 'bad.zip', fetchImpl), /digest mismatch/);
  assert.equal(uploads, 2);
});

test('backfill selects the latest successful run of each platform for both branches', async () => {
  const selected = [];
  await publishBuilds(repository, async path => {
    if (path.includes('/releases?')) return [];
    if (path.includes('/tags/')) return notFound();
    if (path.includes('/artifacts?')) { selected.push(Number(/runs\/(\d+)/.exec(path)[1])); return { artifacts: [] }; }
    const branch = path.includes('branch=master') ? 'master' : 'dev';
    const id = branch === 'master' ? 200 : 300;
    if (path.includes('event=workflow_dispatch')) return { workflow_runs: [
      run({ id: id + 1, event: 'workflow_dispatch', head_branch: branch, run_started_at: '2026-10-27T12:00:00Z' }),
    ] };
    return { workflow_runs: [run({ id, head_branch: branch }),
      run({ id: id - 1, head_branch: branch, run_started_at: '2026-10-25T12:00:00Z' })] };
  }, () => assert.fail('No artifacts'));
  assert.deepEqual(selected, [201, 301]);
});

test('promotion reuses a complete candidate only when its tree is on the merged branch', async () => {
  const sha = 'a'.repeat(40);
  const mergedSha = 'b'.repeat(40);
  const tree = 'c'.repeat(40);
  const tip = 'd'.repeat(40);
  const candidateBranch = `ci-candidate/pr-23-${sha}`;
  const repo = { full_name: repository };
  const mergedAt = '2026-10-28T12:00:00Z';
  const paths = ['android', 'c-cpp', 'switch', 'uwp', 'linux-arm64', 'bsd', 'text-shaping', 'vmu-regressions'];
  const archiveNames = [
    ['Hollycast-release.apk'],
    ['Hollycast-x86_64-w64-mingw32', 'Hollycast-x86_64-pc-linux-gnu', 'Hollycast-apple-darwin'],
    ['Hollycast-switch-standalone'],
    ['flycast-uwp-x64'],
    ['Hollycast-libretro-aarch64-pc-linux-gnu'],
  ];
  const runs = paths.map((path, index) => run({
    id: 123 + index, workflow_id: 10 + index, path: `.github/workflows/${path}.yml`,
    event: 'workflow_dispatch', head_sha: sha, head_branch: candidateBranch,
  }));

  for (const rejected of [null, 'fork', 'not merged', 'outside branch', 'different tree', 'missing test', 'failed rerun',
    'moved ref', 'expired archive', 'missing digest', 'missing macOS archive']) {
    const releases = [];
    const uploads = [];
    const api = async (path, options) => {
      const endpoint = path.slice(`repos/${repository}/`.length);
      if (endpoint === 'pulls/23') return {
        merged: rejected !== 'not merged', base: { ref: 'dev', repo },
        head: { repo: rejected === 'fork' ? { full_name: 'someone/fork' } : repo },
        merge_commit_sha: mergedSha, merged_at: mergedAt,
      };
      if (endpoint === 'git/matching-refs/heads/ci-candidate/pr-23-') return [{
        ref: `refs/heads/${candidateBranch}`, object: { type: 'commit', sha: rejected === 'moved ref' ? tip : sha },
      }];
      if (endpoint === 'git/ref/heads/dev') return { object: { type: 'commit', sha: tip } };
      if (endpoint === `compare/${mergedSha}...${tip}?per_page=1`) return {
        status: rejected === 'outside branch' ? 'diverged' : 'ahead',
      };
      if (endpoint === `git/commits/${mergedSha}`) return { tree: { sha: tree }, message: 'Merged commit' };
      if (endpoint === `git/commits/${sha}`) return { tree: { sha: rejected === 'different tree' ? tip : tree } };
      if (endpoint.startsWith('actions/runs?')) {
        assert.match(endpoint, /event=workflow_dispatch&branch=ci-candidate%2Fpr-23-/);
        if (rejected === 'missing test') return { workflow_runs: runs.slice(0, -1) };
        if (rejected === 'failed rerun') return {
          workflow_runs: [...runs, { ...runs.at(-1), id: 400, conclusion: 'failure' }],
        };
        return { workflow_runs: runs };
      }
      if (endpoint.includes('/artifacts?')) {
        const id = Number(endpoint.split('/')[2]);
        assert.ok(id >= 123 && id <= 127);
        const names = archiveNames[id - 123];
        return { artifacts: names.filter(name =>
          rejected !== 'missing macOS archive' || !name.endsWith('apple-darwin')).map((name, index) => ({
          ...artifact, id: 100 * id + index, name, expired: rejected === 'expired archive' && id === 127,
          digest: rejected === 'missing digest' && id === 127 ? null : `sha256:${'a'.repeat(64)}`,
        })) };
      }
      if (endpoint === 'releases?per_page=100') return [...releases];
      if (endpoint.startsWith('releases/tags/')) {
        return releases.find(item => item.tag_name === endpoint.split('/').at(-1) && !item.draft) ?? notFound();
      }
      if (endpoint === 'releases' && options?.method === 'POST') {
        assert.equal(rejected, null);
        assert.equal(options.body.target_commitish, mergedSha);
        assert.match(options.body.body, new RegExp(`"date":"${mergedAt}"`));
        assert.match(options.body.body, new RegExp(`"candidate":"${sha}"`));
        const item = release({ ...options.body, id: 99 + releases.length });
        releases.push(item);
        return item;
      }
      if (endpoint.startsWith('releases/') && options?.method === 'PATCH') {
        const item = releases.find(entry => entry.id === Number(endpoint.split('/')[1]));
        Object.assign(item, options.body);
        return item;
      }
      assert.fail(`Unexpected endpoint ${endpoint}`);
    };
    const upload = async (target, item) => {
      assert.equal(target.draft, true);
      assert.equal(target.tag_name, `ci-dev-${Math.floor(item.id / 100)}`);
      uploads.push(item.id);
    };
    assert.equal(await publishMergedPullRequest(repository, 23, api, upload), rejected ? 0 : 5, rejected);
    if (!rejected) {
      assert.equal(await publishMergedPullRequest(repository, 23, api, upload), 5);
      assert.deepEqual(uploads, [12700, 12300, 12400, 12401, 12402, 12500, 12600]);
    } else {
      assert.deepEqual(uploads, [], rejected);
    }
  }
});
