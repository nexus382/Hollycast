// Portions Copyright 2026 The Hollycast Authors
import { createHash } from 'node:crypto';
import { completeBuilds, listReleases } from './catalog.mjs';

const filenames = {
  Android: 'Hollycast-android.apk.zip',
  Windows: 'Hollycast-windows-x64.zip',
  Linux: 'Hollycast-linux-x64.zip',
  macOS: 'Hollycast-macos-universal.zip',
  Switch: 'Hollycast-switch.zip',
  UWP: 'Hollycast-uwp-x64.zip',
};
const archiveTag = commit => `latest-dev-${commit}`;
const digest = text => `sha256:${createHash('sha256').update(text).digest('hex')}`;

function metadata(release) {
  if (!release || !/^latest-dev(?:-[a-f0-9]{40})?$/.test(release.tag_name)) return null;
  try {
    const marker = /^<!-- hollycast-latest-dev: (.+) -->\r?\n/.exec(release.body || '');
    const data = JSON.parse(marker?.[1] || 'null');
    if (!/^[a-f0-9]{40}$/.test(data?.commit) || !Number.isFinite(Date.parse(data.date)) ||
        (release.tag_name !== 'latest-dev' && release.tag_name !== archiveTag(data.commit))) return null;
    return data;
  } catch {
    return null;
  }
}

async function optional(api, path) {
  try { return await api(path); }
  catch (error) { if (error.status !== 404) throw error; return null; }
}

export async function publishLatestDev(repository, api, upload) {
  const base = `repos/${repository}`;
  const releases = await listReleases(repository, api);
  const build = completeBuilds(releases, repository).find(build => build.channel === 'ci' && build.branch === 'dev');
  if (!build) return null;
  const current = await optional(api, `${base}/releases/tags/latest-dev`);
  const previous = metadata(current);
  if (current && !previous) throw new Error('Refusing to change unrelated release latest-dev');
  if (previous && (previous.commit === build.commit || Date.parse(previous.date) >= Date.parse(build.date))) return current;

  // Only uploaded assets from the selected, complete dev commit may enter the draft.
  const sources = new Map(releases.filter(release => !release.draft).flatMap(release =>
    release.assets.map(asset => [asset.browser_download_url, asset])));
  const downloads = build.files.filter(file => !file.support).map(file => {
    const asset = sources.get(file.url);
    if (!asset || !/^sha256:[a-f0-9]{64}$/.test(asset.digest) || !Number.isSafeInteger(asset.id)) {
      throw new Error(`Missing source asset or SHA256 digest: ${file.name}`);
    }
    return { ...asset, name: filenames[file.platform] };
  });
  const checksums = downloads.map(asset => `${asset.digest.slice(7)}  ${asset.name}\n`).join('');
  const checksumAsset = { name: 'SHA256SUMS.txt', text: checksums, size: Buffer.byteLength(checksums), digest: digest(checksums) };
  const expected = [...downloads, checksumAsset];
  const data = { commit: build.commit, date: build.date };
  const source = `https://github.com/${repository}`;
  const body = `<!-- hollycast-latest-dev: ${JSON.stringify(data)} -->\n\n` +
    `Development build from the **dev** branch. All six platforms were built from the same commit.\n\n` +
    `[Source commit](${source}/commit/${build.commit}) · Build date: ${build.date}\n\n` +
    'These builds may be unstable. Extract ZIP downloads before use; some contain another ZIP archive. ' +
    'SHA256SUMS.txt checks the downloaded ZIP files.\n\n' + build.notes;
  const tag = archiveTag(build.commit);
  let draft = releases.find(release => release.tag_name === tag) || await optional(api, `${base}/releases/tags/${tag}`);
  if (draft && !metadata(draft)) throw new Error(`Refusing to change unrelated release ${tag}`);
  if (!draft) {
    draft = await api(`${base}/releases`, { method: 'POST', body: {
      tag_name: tag, target_commitish: build.commit, name: 'Latest Development Build',
      body, draft: true, prerelease: true, make_latest: 'false',
    } });
  }
  for (const asset of expected) {
    const existing = draft.assets.find(item => item.name === asset.name);
    if (existing?.state === 'uploaded') {
      if (existing.digest !== asset.digest || existing.size !== asset.size) throw new Error(`Staged asset mismatch: ${asset.name}`);
      continue;
    }
    if (!draft.draft) throw new Error(`Archived development build is incomplete: ${tag}`);
    if (existing) await api(`${base}/releases/assets/${existing.id}`, { method: 'DELETE' });
    await upload(draft, asset, asset.name);
  }
  draft = await api(`${base}/releases/${draft.id}`);
  if (draft.assets.length !== expected.length || expected.some(asset => !draft.assets.some(item =>
    item.name === asset.name && item.state === 'uploaded' && item.digest === asset.digest && item.size === asset.size))) {
    throw new Error('Development draft does not contain all six verified downloads and checksums');
  }

  const refPath = `${base}/git/refs/tags/latest-dev`;
  const ref = await optional(api, `${base}/git/ref/tags/latest-dev`);
  if (ref && (ref.object.type !== 'commit' || ![...releases, draft, current].some(release =>
    metadata(release)?.commit === ref.object.sha))) throw new Error('Refusing to move unrelated tag latest-dev');
  try {
    // Retain the old complete release under its commit tag. Uploads never touch it.
    if (current) await api(`${base}/releases/${current.id}`, { method: 'PATCH', body: {
      tag_name: archiveTag(previous.commit), target_commitish: previous.commit,
      name: `Development build · ${previous.commit.slice(0, 8)}`, make_latest: 'false',
    } });
    if (ref) await api(refPath, { method: 'PATCH', body: { sha: build.commit, force: true } });
    else await api(`${base}/git/refs`, { method: 'POST', body: { ref: 'refs/tags/latest-dev', sha: build.commit } });
    return await api(`${base}/releases/${draft.id}`, { method: 'PATCH', body: {
      tag_name: 'latest-dev', target_commitish: build.commit, name: 'Latest Development Build',
      body, draft: false, prerelease: true, make_latest: 'false',
    } });
  } catch (error) {
    // A lost response may follow a successful promotion; do not undo that success.
    const promoted = await optional(api, `${base}/releases/tags/latest-dev`);
    if (promoted?.id === draft.id && !promoted.draft) return promoted;
    try {
      if (ref) await api(refPath, { method: 'PATCH', body: { sha: ref.object.sha, force: true } });
      else if (await optional(api, `${base}/git/ref/tags/latest-dev`)) await api(refPath, { method: 'DELETE' });
      if (current) await api(`${base}/releases/${current.id}`, { method: 'PATCH', body: {
        tag_name: 'latest-dev', name: current.name, make_latest: 'false',
      } });
    } catch (rollbackError) {
      throw new AggregateError([error, rollbackError], 'Development release promotion and rollback failed; retry the publishing workflow');
    }
    throw error;
  }
}
