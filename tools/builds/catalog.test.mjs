// Portions Copyright 2026 The Hollycast Authors
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { buildCatalog, ciMetadata, platformFor, releaseBuild } from './catalog.mjs';

test('historical development tags remain development releases', () => {
  for (const tag of ['v0.1.1-dev.1', 'v0.1.0-beta.1.dev.1', 'v2.0-rc1']) {
    assert.equal(releaseBuild({ tag_name: tag, assets: [] }).channel, 'development');
  }
  assert.equal(releaseBuild({ tag_name: 'v0.1.1', assets: [] }).channel, 'stable');
});

test('platforms preserve libretro host and UWP distinctions', () => {
  for (const [name, expected] of Object.entries({
    'Hollycast-release.apk': 'Android', 'Hollycast-linux-libretro-arm64.zip': 'Linux',
    '123-Hollycast-release.apk.zip': 'Android',
    'Hollycast-x86_64-w64-mingw32': 'Windows', 'Hollycast-apple-darwin': 'macOS',
    'flycast-uwp-ARM64': 'UWP', 'Hollycast-switch.nro': 'Switch',
  })) assert.equal(platformFor(name), expected);
});

function automaticRelease(id, branch = 'dev', commit = 'a'.repeat(40)) {
  const metadata = { branch, run: id, workflow: id, commit, title: 'Improve playback', date: '2026-10-26T12:00:00Z' };
  return {
    tag_name: `ci-${branch}-${id}`, html_url: `https://github.com/nexus382/Hollycast/releases/tag/ci-${branch}-${id}`,
    body: `<!-- hollycast-ci: ${JSON.stringify(metadata)} -->\n\nFixed playback.`,
    assets: [{ state: 'uploaded', name: `${id}-Hollycast-release.apk.zip`, size: 100,
      browser_download_url: `https://github.com/nexus382/Hollycast/releases/download/ci-${branch}-${id}/build.zip` }],
  };
}

test('public CI downloads survive artifact expiry and combine platforms by branch and commit', async () => {
  const api = async path => {
    assert.match(path, /\/releases\?/);
    return [automaticRelease(1), automaticRelease(2), automaticRelease(3, 'master'),
      { ...automaticRelease(4), draft: true }];
  };
  const result = await buildCatalog('nexus382/Hollycast', api, new Date('2028-01-01T00:00:00Z'));
  assert.equal(result.builds.length, 2);
  assert.equal(result.builds[0].files.length, 2);
  assert.equal(result.builds[0].notes, 'Fixed playback.');
  assert.equal(result.builds[0].title, 'Improve playback');
  assert.equal(result.builds[0].branch, 'dev');
  assert.equal(result.builds[1].branch, 'master');
  assert.ok(result.builds.flatMap(build => build.files).every(file => file.url.includes('/releases/download/') && !file.expires));
});

test('malformed reserved releases do not impersonate public builds', () => {
  const release = automaticRelease(1);
  assert.equal(ciMetadata({ ...release, body: 'manual release' }), null);
  assert.equal(releaseBuild({ ...release, body: 'manual release' }), null);
  assert.equal(ciMetadata({ ...release, tag_name: 'ci-master-1' }), null);
});

test('release pagination preserves ordinary releases behind automatic builds', async () => {
  const pages = [];
  const result = await buildCatalog('nexus382/Hollycast', async path => {
    pages.push(path);
    if (path.endsWith('page=1')) return Array.from({ length: 100 }, (_, i) => automaticRelease(i + 1));
    return [{ tag_name: 'v1.0', published_at: '2026-10-01T00:00:00Z', assets: [
      { state: 'uploaded', name: 'Hollycast-android.apk', size: 100, browser_download_url: 'https://github.com/nexus382/Hollycast/releases/download/v1.0/Hollycast-android.apk' },
    ] }];
  });
  assert.equal(pages.length, 2);
  assert.equal(result.builds.at(-1).channel, 'stable');
});

test('API failures stop publication instead of replacing a working catalog', async () => {
  await assert.rejects(buildCatalog('nexus382/Hollycast', async () => { throw new Error('API unavailable'); }), /API unavailable/);
});
