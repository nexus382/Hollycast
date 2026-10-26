// Portions Copyright 2026 The Hollycast Authors
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

export function platformFor(name) {
  if (/android|\.apk$|\.aab$/i.test(name)) return 'Android';
  if (/uwp|\.appx$|\.msix$/i.test(name)) return 'UWP';
  if (/windows|mingw|msvc/i.test(name)) return 'Windows';
  if (/linux|appimage/i.test(name)) return 'Linux';
  if (/macos|darwin/i.test(name)) return 'macOS';
  if (/switch|\.nro$/i.test(name)) return 'Switch';
  return 'Other';
}

export function releaseBuild(release) {
  return {
    title: release.name || release.tag_name,
    // Some historical .dev releases were published without GitHub's prerelease flag.
    channel: release.prerelease || /(?:^|[.-])(?:dev|alpha|beta|rc)(?:[.-]|\d|$)/i.test(release.tag_name) ? 'development' : 'stable',
    date: release.published_at,
    url: release.html_url,
    notes: release.body || '',
    files: release.assets.filter(asset => asset.state === 'uploaded').map(asset => ({
      name: asset.name,
      url: asset.browser_download_url,
      size: asset.size,
      platform: platformFor(asset.name),
      support: /\.(?:json|txt|cer|sha256)$/i.test(asset.name),
    })),
  };
}

export async function buildCatalog(repository, api, now = new Date()) {
  const base = `repos/${repository}`;
  // Bound history to the latest 100 releases and 300 artifacts; never download binaries.
  const releases = await api(`${base}/releases?per_page=100`);
  const builds = releases.filter(release => !release.draft).map(releaseBuild).filter(build => build.files.length);
  const grouped = new Map();
  for (let page = 1; page <= 3; page++) {
    const { artifacts } = await api(`${base}/actions/artifacts?per_page=100&page=${page}`);
    for (const artifact of artifacts) {
      const run = artifact.workflow_run;
      if (artifact.expired || Date.parse(artifact.expires_at) <= now.getTime() ||
          !run || run.head_repository_id !== run.repository_id ||
          !['master', 'dev'].includes(run.head_branch) || !/^(?:hollycast|flycast)[-_.]/i.test(artifact.name)) continue;
      if (!grouped.has(run.id)) grouped.set(run.id, []);
      grouped.get(run.id).push(artifact);
    }
    if (artifacts.length < 100) break;
  }
  for (const [id, artifacts] of grouped) {
    const run = await api(`${base}/actions/runs/${id}`);
    // workflow_run is privileged: publish metadata only for this repo's successful pushes.
    if (run.event !== 'push' || run.status !== 'completed' || run.conclusion !== 'success' ||
        run.repository.full_name.toLowerCase() !== repository.toLowerCase() ||
        run.head_repository?.full_name.toLowerCase() !== repository.toLowerCase() ||
        !['master', 'dev'].includes(run.head_branch)) continue;
    builds.push({
      title: `${run.name} #${run.run_number}`,
      channel: 'ci',
      branch: run.head_branch,
      workflow: run.workflow_id,
      date: run.run_started_at,
      commit: run.head_sha,
      url: run.html_url,
      notes: run.head_commit?.message || 'See the build commit on GitHub.',
      files: artifacts.map(artifact => ({
        name: artifact.name,
        url: `https://github.com/${repository}/actions/runs/${id}/artifacts/${artifact.id}`,
        size: artifact.size_in_bytes,
        platform: platformFor(artifact.name),
        expires: artifact.expires_at,
      })),
    });
  }
  builds.sort((a, b) => Date.parse(b.date) - Date.parse(a.date));
  for (let i = 0; i < builds.length; i++) {
    const build = builds[i];
    if (build.channel !== 'ci') continue;
    const previous = builds.slice(i + 1).find(candidate => candidate.channel === 'ci' &&
      candidate.branch === build.branch && candidate.workflow === build.workflow && candidate.commit !== build.commit);
    if (previous) {
      build.compare = `https://github.com/${repository}/compare/${previous.commit}...${build.commit}`;
      let comparison;
      try {
        comparison = await api(`${base}/compare/${previous.commit}...${build.commit}?per_page=100`);
      } catch (error) {
        if (error.status !== 404) throw error;
        // A force-push can make the previous commit unavailable to the compare API.
        build.compare = `https://github.com/${repository}/commit/${build.commit}`;
        build.notes += '\n\nThe previous build commit is no longer available for comparison.';
        continue;
      }
      if (comparison.status === 'ahead') {
        build.notes = comparison.commits.map(commit => commit.commit.message).join('\n\n');
        if (comparison.total_commits > comparison.commits.length) build.notes += '\n\nMore commits are available in the full comparison on GitHub.';
      } else {
        // Rebased or reset branches do not have a linear changes-since-last-build list.
        build.notes += '\n\nBranch history changed; view the comparison on GitHub for details.';
      }
    } else {
      build.notes = `Build commit:\n\n${build.notes}\n\nNo earlier build is available in the recent catalog for comparison.`;
      build.compare = `https://github.com/${repository}/commit/${build.commit}`;
    }
  }
  return { updated: now.toISOString(), repository, builds };
}

async function main() {
  const repository = process.env.GITHUB_REPOSITORY || 'nexus382/Hollycast';
  if (!/^[\w.-]+\/[\w.-]+$/.test(repository)) throw new Error('Invalid repository');
  const api = async path => {
    const response = await fetch(`https://api.github.com/${path}`, {
      headers: {
        Accept: 'application/vnd.github+json',
        'X-GitHub-Api-Version': '2022-11-28',
        ...(process.env.GH_TOKEN ? { Authorization: `Bearer ${process.env.GH_TOKEN}` } : {}),
      },
      signal: AbortSignal.timeout(30000),
    });
    if (!response.ok) throw Object.assign(new Error(`GitHub API ${response.status}: ${path}`), { status: response.status });
    return response.json();
  };
  const catalog = await buildCatalog(repository, api);
  const directory = resolve(process.argv[2] || 'docs/builds');
  await mkdir(directory, { recursive: true });
  await writeFile(resolve(directory, 'builds.json'), `${JSON.stringify(catalog)}\n`);
  console.log(`Catalog: ${catalog.builds.length} builds`);
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  main().catch(error => { console.error(error.message); process.exitCode = 1; });
}
