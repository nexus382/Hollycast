// Portions Copyright 2026 The Hollycast Authors
import { createReadStream, createWriteStream, openAsBlob } from 'node:fs';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { pipeline } from 'node:stream/promises';
import { pathToFileURL } from 'node:url';
import { ciMetadata, githubApi } from './catalog.mjs';

const workflows = new Set(['android.yml', 'c-cpp.yml', 'switch.yml', 'uwp.yml', 'bsd.yml', 'linux-arm64.yml']
  .map(name => `.github/workflows/${name}`));

export function eligibleRun(run, repository) {
  return run.event === 'push' && run.status === 'completed' && run.conclusion === 'success' &&
    run.repository?.full_name.toLowerCase() === repository.toLowerCase() &&
    run.head_repository?.full_name.toLowerCase() === repository.toLowerCase() &&
    ['master', 'dev'].includes(run.head_branch) && workflows.has(run.path) &&
    /^[a-f0-9]{40}$/.test(run.head_sha) && Number.isFinite(Date.parse(run.run_started_at));
}

export function archiveName(artifact) {
  return `${artifact.id}-${artifact.name.replace(/[^a-z0-9._-]/gi, '-')}.zip`;
}

export async function publishRun(repository, run, api, upload, releases) {
  if (!eligibleRun(run, repository)) return null;
  const base = `repos/${repository}`;
  const tag = `ci-${run.head_branch}-${run.id}`;
  let release;
  try {
    release = await api(`${base}/releases/tags/${tag}`);
  } catch (error) {
    if (error.status !== 404) throw error;
  }
  if (release) {
    const provenance = ciMetadata(release);
    if (!provenance || provenance.commit !== run.head_sha || provenance.workflow !== run.workflow_id) {
      throw new Error(`Refusing to change unrelated release ${tag}`);
    }
    if (!release.draft) return release;
  }
  const artifacts = [];
  for (let page = 1; ; page++) {
    const result = await api(`${base}/actions/runs/${run.id}/artifacts?per_page=100&page=${page}`);
    artifacts.push(...result.artifacts.filter(artifact => !artifact.expired &&
      Date.parse(artifact.expires_at) > Date.now() && /^(?:hollycast|flycast)[-_.]/i.test(artifact.name)));
    if (result.artifacts.length < 100) break;
  }
  if (!artifacts.length) return null;
  const previous = releases.filter(candidate => !candidate.draft).map(ciMetadata).filter(candidate =>
    candidate?.branch === run.head_branch && candidate.commit !== run.head_sha &&
    Date.parse(candidate.date) < Date.parse(run.run_started_at))
    .sort((a, b) => Date.parse(b.date) - Date.parse(a.date))[0];
  let notes = `Build commit:\n\n${run.head_commit?.message || run.head_sha}`;
  let previousCommit;
  if (previous) {
    try {
      const comparison = await api(`${base}/compare/${previous.commit}...${run.head_sha}?per_page=100`);
      if (comparison.status === 'ahead') {
        previousCommit = previous.commit;
        notes = comparison.commits.map(commit => commit.commit.message).join('\n\n');
        if (comparison.total_commits > comparison.commits.length) notes += '\n\nMore commits are available in the full comparison.';
      } else {
        notes += '\n\nBranch history changed; this is the current build commit.';
      }
    } catch (error) {
      if (error.status !== 404) throw error;
      notes += '\n\nThe previous commit is no longer available for comparison.';
    }
  }
  const metadata = { branch: run.head_branch, run: run.id, workflow: run.workflow_id,
    commit: run.head_sha, date: run.run_started_at, ...(previousCommit ? { previous: previousCommit } : {}) };
  const source = `https://github.com/${repository}`;
  const body = `<!-- hollycast-ci: ${JSON.stringify(metadata)} -->\n\n${notes}\n\n` +
    `[Source commit](${source}/commit/${run.head_sha}) · [Build details](${source}/actions/runs/${run.id})\n\n` +
    'Automatic testing build. Downloads are ZIP archives; extract them before use. These builds may be unstable.';
  if (!release) {
    release = await api(`${base}/releases`, { method: 'POST', body: {
      tag_name: tag, target_commitish: run.head_sha,
      name: `${run.head_branch} · ${run.name} #${run.run_number}`,
      body, draft: true, prerelease: true, make_latest: 'false',
    } });
  }
  // Keep incomplete uploads private. A retry resumes this draft without replacing assets.
  for (const artifact of artifacts) {
    const name = archiveName(artifact);
    const existing = release.assets.find(asset => asset.name === name);
    if (existing) {
      if (existing.state === 'uploaded') {
        if (artifact.digest && existing.digest && artifact.digest !== existing.digest) throw new Error(`Asset digest mismatch: ${name}`);
        continue;
      }
      // GitHub can leave an empty "starter" asset after a failed upload. Remove only
      // this publisher's incomplete draft asset so the next attempt can recover.
      await api(`${base}/releases/assets/${existing.id}`, { method: 'DELETE' });
    }
    await upload(release, artifact, name);
  }
  return api(`${base}/releases/${release.id}`, { method: 'PATCH', body: {
    body, draft: false, prerelease: true, make_latest: 'false',
  } });
}

export async function publishBuilds(repository, api, upload, runId) {
  const base = `repos/${repository}`;
  const releases = await api(`${base}/releases?per_page=100`);
  let runs;
  if (runId !== undefined) {
    if (!Number.isSafeInteger(runId) || runId <= 0) throw new Error('Invalid workflow run ID');
    runs = [await api(`${base}/actions/runs/${runId}`)];
  } else {
    runs = [];
    for (const branch of ['master', 'dev']) {
      const result = await api(`${base}/actions/runs?event=push&status=success&branch=${branch}&per_page=100`);
      const latest = new Map();
      for (const run of result.workflow_runs) {
        if (!eligibleRun(run, repository)) continue;
        const existing = latest.get(run.workflow_id);
        if (!existing || Date.parse(run.run_started_at) > Date.parse(existing.run_started_at)) latest.set(run.workflow_id, run);
      }
      runs.push(...latest.values());
    }
  }
  // Oldest first preserves meaningful comparisons while backfilling the latest platform builds.
  runs.sort((a, b) => Date.parse(a.run_started_at) - Date.parse(b.run_started_at));
  let count = 0;
  for (const run of runs) {
    const release = await publishRun(repository, run, api, upload, releases);
    if (release) {
      releases.push(release);
      count++;
    }
  }
  return count;
}

export async function uploadArtifact(repository, token, release, artifact, name, fetchImpl = fetch) {
  if (artifact.size_in_bytes > 2 * 1024 ** 3) throw new Error(`Artifact exceeds GitHub's release asset limit: ${name}`);
  const directory = await mkdtemp(join(tmpdir(), 'hollycast-publish-'));
  try {
    const file = join(directory, 'artifact.zip');
    const response = await fetchImpl(`https://api.github.com/repos/${repository}/actions/artifacts/${artifact.id}/zip`, {
      headers: { Authorization: `Bearer ${token}`, Accept: 'application/vnd.github+json' },
      signal: AbortSignal.timeout(10 * 60 * 1000),
    });
    if (!response.ok) throw new Error(`Download failed (${response.status}): ${name}`);
    // Archives remain opaque data: never extract or execute build-provided content.
    await pipeline(response.body, createWriteStream(file));
    if (artifact.digest) {
      const hash = createHash('sha256');
      for await (const chunk of createReadStream(file)) hash.update(chunk);
      if (`sha256:${hash.digest('hex')}` !== artifact.digest) throw new Error(`Artifact digest mismatch: ${name}`);
    }
    const url = new URL(release.upload_url.split('{')[0]);
    if (url.origin !== 'https://uploads.github.com' || !url.pathname.startsWith(`/repos/${repository}/releases/`)) throw new Error('Unexpected upload URL');
    url.searchParams.set('name', name);
    const uploaded = await fetchImpl(url, {
      method: 'POST',
      headers: { Authorization: `Bearer ${token}`, 'Content-Type': 'application/zip' },
      body: await openAsBlob(file),
      signal: AbortSignal.timeout(10 * 60 * 1000),
    });
    if (!uploaded.ok) throw new Error(`Upload failed (${uploaded.status}): ${name}`);
    await uploaded.body?.cancel();
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
}

async function main() {
  const repository = process.env.GITHUB_REPOSITORY;
  const token = process.env.GH_TOKEN;
  if (repository !== 'nexus382/Hollycast' || !token) throw new Error('Publishing requires the Hollycast repository token');
  let runId;
  if (process.env.GITHUB_EVENT_NAME === 'workflow_run') {
    const event = JSON.parse(await readFile(process.env.GITHUB_EVENT_PATH, 'utf8'));
    runId = event.workflow_run.id;
  }
  const count = await publishBuilds(repository, githubApi(token),
    (release, artifact, name) => uploadArtifact(repository, token, release, artifact, name), runId);
  console.log(`Public build releases available: ${count}`);
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  main().catch(error => { console.error(error.message); process.exitCode = 1; });
}
