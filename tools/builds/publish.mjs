// Portions Copyright 2026 The Hollycast Authors
import { createReadStream, createWriteStream, openAsBlob } from 'node:fs';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { pipeline } from 'node:stream/promises';
import { pathToFileURL } from 'node:url';
import { ciMetadata, githubApi, platformFor, standaloneDownloads } from './catalog.mjs';
import { publishLatestDev } from './latest-dev.mjs';

const workflows = new Set(['android.yml', 'c-cpp.yml', 'switch.yml', 'uwp.yml', 'bsd.yml', 'linux-arm64.yml']
  .map(name => `.github/workflows/${name}`));
const candidateWorkflows = new Set([...workflows, '.github/workflows/text-shaping.yml',
  '.github/workflows/vmu-regressions.yml']);
// BSD is a required check, but its workflow does not upload a build archive.
const candidateBuilds = ['linux-arm64', 'android', 'c-cpp', 'switch', 'uwp']
  .map(name => `.github/workflows/${name}.yml`);
const publicBranches = ['master', 'dev'];
const commitSha = /^[a-f0-9]{40}$/;
const artifactDigest = /^sha256:[a-f0-9]{64}$/;
const sameRepository = (repo, repository) => repo?.full_name?.toLowerCase() === repository.toLowerCase();
const passed = run => run.status === 'completed' && run.conclusion === 'success';

export function eligibleRun(run, repository) {
  return ['push', 'workflow_dispatch'].includes(run.event) && passed(run) &&
    sameRepository(run.repository, repository) && sameRepository(run.head_repository, repository) &&
    publicBranches.includes(run.head_branch) && workflows.has(run.path) &&
    commitSha.test(run.head_sha) && Number.isFinite(Date.parse(run.run_started_at));
}

export function archiveName(artifact) {
  return `${artifact.id}-${artifact.name.replace(/[^a-z0-9._-]/gi, '-')}.zip`;
}

export async function publishRun(repository, run, api, upload, releases) {
  if (!eligibleRun(run, repository)) return null;
  return publishRelease(repository, run, api, upload, releases);
}

async function listArtifacts(base, runId, api) {
  const artifacts = [];
  for (let page = 1; ; page++) {
    const result = await api(`${base}/actions/runs/${runId}/artifacts?per_page=100&page=${page}`);
    artifacts.push(...result.artifacts.filter(artifact => !artifact.expired &&
      Date.parse(artifact.expires_at) > Date.now() && /^(?:hollycast|flycast)[-_.]/i.test(artifact.name)));
    if (result.artifacts.length < 100) return artifacts;
  }
}

async function publishRelease(repository, run, api, upload, releases, artifacts) {
  const base = `repos/${repository}`;
  const tag = `ci-${run.head_branch}-${run.id}`;
  let release;
  try {
    release = await api(`${base}/releases/tags/${tag}`);
  } catch (error) {
    if (error.status !== 404) throw error;
    // The tag endpoint only returns published releases; drafts come from the list API.
    release = releases.find(candidate => candidate.tag_name === tag);
  }
  if (release) {
    const provenance = ciMetadata(release);
    if (!provenance || provenance.commit !== run.head_sha || provenance.workflow !== run.workflow_id) {
      throw new Error(`Refusing to change unrelated release ${tag}`);
    }
    if (!release.draft) return release;
  }
  artifacts ??= await listArtifacts(base, run.id, api);
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
    commit: run.head_sha, date: run.run_started_at, ...(previousCommit ? { previous: previousCommit } : {}),
    ...(run.candidate ? { candidate: run.candidate } : {}) };
  const source = `https://github.com/${repository}`;
  const revision = run.candidate ? `[Merged commit](${source}/commit/${run.head_sha}) · [Tested commit](${source}/commit/${run.candidate})` :
    `[Source commit](${source}/commit/${run.head_sha})`;
  const body = `<!-- hollycast-ci: ${JSON.stringify(metadata)} -->\n\n${notes}\n\n` +
    `${revision} · [Build details](${source}/actions/runs/${run.id})\n\n` +
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
    for (const branch of publicBranches) {
      const results = await Promise.all(['push', 'workflow_dispatch'].map(event =>
        api(`${base}/actions/runs?event=${event}&status=success&branch=${branch}&per_page=100`)));
      const latest = new Map();
      for (const run of results.flatMap(result => result.workflow_runs)) {
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

async function passedCandidateRuns(base, branch, sha, repository, api) {
  const latest = new Map();
  for (let page = 1; ; page++) {
    const result = await api(`${base}/actions/runs?event=workflow_dispatch&branch=${encodeURIComponent(branch)}&head_sha=${sha}&per_page=100&page=${page}`);
    for (const run of result.workflow_runs) {
      if (run.event !== 'workflow_dispatch' || run.head_branch !== branch || run.head_sha !== sha ||
          !sameRepository(run.repository, repository) || !sameRepository(run.head_repository, repository) ||
          !candidateWorkflows.has(run.path)) continue;
      const existing = latest.get(run.path);
      // Compare dispatches, including failures and queued reruns, before requiring success.
      if (!existing || run.id > existing.id) latest.set(run.path, run);
    }
    if (result.workflow_runs.length < 100) break;
  }
  if (latest.size !== candidateWorkflows.size || ![...latest.values()].every(passed)) return [];
  return candidateBuilds.map(path => latest.get(path));
}

export async function publishMergedPullRequest(repository, number, api, upload) {
  if (!Number.isSafeInteger(number) || number <= 0) throw new Error('Invalid pull request number');
  const base = `repos/${repository}`;
  const pr = await api(`${base}/pulls/${number}`);
  const branch = pr.base?.ref;
  const mergedSha = pr.merge_commit_sha;
  if (!pr.merged || !publicBranches.includes(branch) ||
      !sameRepository(pr.base?.repo, repository) || !sameRepository(pr.head?.repo, repository) ||
      !commitSha.test(mergedSha) || !Number.isFinite(Date.parse(pr.merged_at))) return 0;

  const prefix = `ci-candidate/pr-${number}-`;
  const refs = await api(`${base}/git/matching-refs/heads/${prefix}`);
  if (!refs.length) return 0;
  const tip = (await api(`${base}/git/ref/heads/${branch}`)).object;
  if (tip.type !== 'commit' || !commitSha.test(tip.sha)) return 0;
  const ancestry = await api(`${base}/compare/${mergedSha}...${tip.sha}?per_page=1`);
  if (!['identical', 'ahead'].includes(ancestry.status)) return 0;
  const merged = await api(`${base}/git/commits/${mergedSha}`);

  for (const ref of refs) {
    const sha = ref.object.sha;
    const candidateBranch = `${prefix}${sha}`;
    if (ref.object.type !== 'commit' || !commitSha.test(sha) || ref.ref !== `refs/heads/${candidateBranch}`) continue;
    const candidate = await api(`${base}/git/commits/${sha}`);
    if (!commitSha.test(merged.tree?.sha) || candidate.tree?.sha !== merged.tree.sha) continue;
    const runs = await passedCandidateRuns(base, candidateBranch, sha, repository, api);
    if (!runs.length) continue;
    const archives = await Promise.all(runs.map(run => listArtifacts(base, run.id, api)));
    if (archives.some(items => !items.length || items.some(item => !artifactDigest.test(item.digest)))) continue;
    const files = archives.flat().map(artifact => {
      const name = archiveName(artifact);
      return { name, platform: platformFor(name) };
    });
    if (standaloneDownloads(files).length !== 6) continue;
    const releases = await api(`${base}/releases?per_page=100`);

    // A PR run supplies the artifacts and run ID, while release history tracks the merged commit.
    for (const [index, run] of runs.entries()) {
      const published = await publishRelease(repository, {
        ...run, candidate: sha, head_branch: branch, head_sha: mergedSha,
        head_commit: merged, run_started_at: pr.merged_at,
      }, api, upload, releases, archives[index]);
      releases.push(published);
    }
    return runs.length;
  }
  return 0;
}

async function uploadFile(repository, token, release, name, body, contentType, fetchImpl) {
  const url = new URL(release.upload_url.split('{')[0]);
  if (url.origin !== 'https://uploads.github.com' || !url.pathname.startsWith(`/repos/${repository}/releases/`)) throw new Error('Unexpected upload URL');
  url.searchParams.set('name', name);
  const uploaded = await fetchImpl(url, {
    method: 'POST',
    headers: { Authorization: `Bearer ${token}`, 'Content-Type': contentType },
    body,
    signal: AbortSignal.timeout(10 * 60 * 1000),
  });
  if (!uploaded.ok) throw new Error(`Upload failed (${uploaded.status}): ${name}`);
  await uploaded.body?.cancel();
}

async function uploadArchive(repository, token, release, source, name, fetchImpl) {
  if (source.size > 2 * 1024 ** 3) throw new Error(`Artifact exceeds GitHub's release asset limit: ${name}`);
  const directory = await mkdtemp(join(tmpdir(), 'hollycast-publish-'));
  try {
    const file = join(directory, 'artifact.zip');
    const response = await fetchImpl(source.url, {
      headers: { Authorization: `Bearer ${token}`, Accept: source.accept },
      signal: AbortSignal.timeout(10 * 60 * 1000),
    });
    if (!response.ok) throw new Error(`Download failed (${response.status}): ${name}`);
    // Archives remain opaque data: never extract or execute build-provided content.
    await pipeline(response.body, createWriteStream(file));
    if (source.digest) {
      const hash = createHash('sha256');
      for await (const chunk of createReadStream(file)) hash.update(chunk);
      if (`sha256:${hash.digest('hex')}` !== source.digest) throw new Error(`Artifact digest mismatch: ${name}`);
    }
    await uploadFile(repository, token, release, name, await openAsBlob(file), 'application/zip', fetchImpl);
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
}

export function uploadArtifact(repository, token, release, artifact, name, fetchImpl = fetch) {
  return uploadArchive(repository, token, release, {
    url: `https://api.github.com/repos/${repository}/actions/artifacts/${artifact.id}/zip`,
    size: artifact.size_in_bytes, digest: artifact.digest, accept: 'application/vnd.github+json',
  }, name, fetchImpl);
}

export function uploadReleaseAsset(repository, token, release, asset, name, fetchImpl = fetch) {
  if (asset.text !== undefined) return uploadFile(repository, token, release, name, asset.text, 'text/plain', fetchImpl);
  return uploadArchive(repository, token, release, {
    url: `https://api.github.com/repos/${repository}/releases/assets/${asset.id}`,
    size: asset.size, digest: asset.digest, accept: 'application/octet-stream',
  }, name, fetchImpl);
}

async function main() {
  const repository = process.env.GITHUB_REPOSITORY;
  const token = process.env.GH_TOKEN;
  if (repository !== 'nexus382/Hollycast' || !token) throw new Error('Publishing requires the Hollycast repository token');
  const eventName = process.env.GITHUB_EVENT_NAME;
  let runId;
  let pullRequest;
  if (eventName === 'workflow_run') {
    const event = JSON.parse(await readFile(process.env.GITHUB_EVENT_PATH, 'utf8'));
    runId = event.workflow_run.id;
  } else if (eventName === 'workflow_dispatch' && process.env.PROMOTE_PR) {
    pullRequest = Number(process.env.PROMOTE_PR);
  }
  const api = githubApi(token);
  const upload = (release, artifact, name) => uploadArtifact(repository, token, release, artifact, name);
  const count = pullRequest === undefined ? await publishBuilds(repository, api, upload, runId) :
    await publishMergedPullRequest(repository, pullRequest, api, upload);
  if (pullRequest !== undefined && !count) {
    throw new Error(`PR #${pullRequest} has no matching merged tree with eight successful runs and complete artifacts.`);
  }
  console.log(`Public build releases available: ${count}`);
  const latest = await publishLatestDev(repository, api,
    (release, asset, name) => uploadReleaseAsset(repository, token, release, asset, name));
  console.log(`Latest dev release: ${latest?.html_url || 'waiting for a complete dev build'}`);
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  main().catch(error => { console.error(error.message); process.exitCode = 1; });
}
