// Portions Copyright 2026 The Hollycast Authors
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

export function platformFor(name) {
  if (/android|\.(?:apk|aab)(?:\.zip)?$/i.test(name)) return 'Android';
  if (/uwp|\.appx$|\.msix$/i.test(name)) return 'UWP';
  if (/windows|mingw|msvc/i.test(name)) return 'Windows';
  if (/linux|appimage/i.test(name)) return 'Linux';
  if (/macos|darwin/i.test(name)) return 'macOS';
  if (/switch|\.nro$/i.test(name)) return 'Switch';
  return 'Other';
}

export function standaloneDownloads(files) {
  // Match the six standalone downloads in flyinghead/flycast-builds/builds.js.
  // Legacy CI assets may wrap the same package in an additional artifact ZIP.
  const targets = [
    ['Android', /\.apk(?:\.zip)?$/i],
    ['Windows', /(?:windows-x64|x86_64-w64-mingw32)(?:\.zip){1,2}$/i],
    ['Linux', /(?:\.appimage(?:\.zip)?|x86_64-pc-linux-gnu\.zip)$/i],
    ['macOS', /(?:macos-universal|apple-darwin|\.app)(?:\.zip){1,2}$/i],
    ['Switch', /(?:switch(?:-standalone)?\.zip|\.nro(?:\.zip)?)$/i],
    ['UWP', /(?:uwp|xbox)-x64(?:\.appx|\.msix)?(?:\.zip)?$/i],
  ];
  return targets.flatMap(([platform, pattern]) => {
    const file = files.find(candidate => !candidate.support && candidate.platform === platform &&
      !/libretro|msvc/i.test(candidate.name) && pattern.test(candidate.name));
    return file ? [file] : [];
  });
}

export function ciMetadata(release) {
  const tag = /^ci-(master|dev)-(\d+)$/.exec(release.tag_name);
  if (!tag) return null;
  try {
    const marker = /^<!-- hollycast-ci: (.+) -->\r?\n/.exec(release.body || '');
    const data = JSON.parse(marker?.[1] || 'null');
    if (data?.branch !== tag[1] || !Number.isSafeInteger(data.run) || data.run <= 0 || String(data.run) !== tag[2] ||
        !Number.isSafeInteger(data.workflow) || data.workflow <= 0 ||
        !/^[a-f0-9]{40}$/.test(data.commit) || !Number.isFinite(Date.parse(data.date)) ||
        (data.previous && !/^[a-f0-9]{40}$/.test(data.previous))) return null;
    return data;
  } catch {
    return null;
  }
}

export function releaseBuild(release, repository = 'nexus382/Hollycast') {
  const ci = ciMetadata(release);
  // Reserved automatic tags must have valid provenance, not become ordinary releases.
  if (/^ci-(master|dev)-/.test(release.tag_name) && !ci) return null;
  return {
    title: ci ? `${ci.branch} · ${ci.commit.slice(0, 8)}` : release.name || release.tag_name,
    // Some historical .dev releases were published without GitHub's prerelease flag.
    channel: ci ? 'ci' : release.prerelease || /(?:^|[.-])(?:dev|alpha|beta|rc)(?:[.-]|\d|$)/i.test(release.tag_name) ? 'development' : 'stable',
    date: ci?.date || release.published_at,
    url: release.html_url,
    notes: ci ? release.body.replace(/^<!-- hollycast-ci: .+ -->\r?\n/, '').trim() : release.body || '',
    ...(ci ? {
      branch: ci.branch,
      commit: ci.commit,
      compare: `https://github.com/${repository}/${ci.previous ? `compare/${ci.previous}...${ci.commit}` : `commit/${ci.commit}`}`,
    } : {}),
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
  const builds = [];
  const commits = new Map();
  // Scan beyond automatic releases so ordinary releases stay visible as CI history grows.
  for (let page = 1; page <= 10; page++) {
    const releases = await api(`${base}/releases?per_page=100&page=${page}`);
    for (const release of releases) {
      if (release.draft) continue;
      const build = releaseBuild(release, repository);
      if (!build?.files.length) continue;
      if (build.channel !== 'ci') {
        builds.push(build);
        continue;
      }
      const key = `${build.branch}:${build.commit}`;
      const existing = commits.get(key);
      if (existing) {
        const urls = new Set(existing.files.map(file => file.url));
        existing.files.push(...build.files.filter(file => !urls.has(file.url)));
      } else {
        commits.set(key, build);
        builds.push(build);
      }
    }
    if (releases.length < 100) break;
  }
  const complete = builds.filter(build => {
    const downloads = standaloneDownloads(build.files);
    // Wait for all six platforms after grouping runs from the same branch and commit.
    if (downloads.length !== 6) return false;
    build.files = [...downloads, ...build.files.filter(file => file.support)];
    return true;
  });
  complete.sort((a, b) => Date.parse(b.date) - Date.parse(a.date));
  const counts = new Map();
  const recent = complete.filter(build => {
    const key = build.channel === 'ci' ? `ci:${build.branch}` : build.channel;
    const count = (counts.get(key) || 0) + 1;
    counts.set(key, count);
    return count <= 20;
  });
  return { updated: now.toISOString(), repository, builds: recent };
}

export function githubApi(token = process.env.GH_TOKEN) {
  return async (path, options = {}) => {
    const response = await fetch(`https://api.github.com/${path}`, {
      method: options.method || 'GET',
      headers: {
        Accept: 'application/vnd.github+json',
        'X-GitHub-Api-Version': '2022-11-28',
        ...(token ? { Authorization: `Bearer ${token}` } : {}),
        ...(options.body ? { 'Content-Type': 'application/json' } : {}),
      },
      ...(options.body ? { body: JSON.stringify(options.body) } : {}),
      signal: AbortSignal.timeout(30000),
    });
    if (!response.ok) throw Object.assign(new Error(`GitHub API ${response.status}: ${path}`), { status: response.status });
    if (response.status === 204) return null;
    return response.json();
  };
}

async function main() {
  const repository = process.env.GITHUB_REPOSITORY || 'nexus382/Hollycast';
  if (!/^[\w.-]+\/[\w.-]+$/.test(repository)) throw new Error('Invalid repository');
  const catalog = await buildCatalog(repository, githubApi());
  const directory = resolve(process.argv[2] || 'docs/builds');
  await mkdir(directory, { recursive: true });
  await writeFile(resolve(directory, 'builds.json'), `${JSON.stringify(catalog)}\n`);
  console.log(`Catalog: ${catalog.builds.length} builds`);
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  main().catch(error => { console.error(error.message); process.exitCode = 1; });
}
