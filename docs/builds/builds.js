// Portions Copyright 2026 The Hollycast Authors
'use strict';

const status = document.querySelector('#status');
const channel = document.querySelector('#channel');
const platform = document.querySelector('#platform');
const search = document.querySelector('#search');
let catalog;

function element(tag, text, className) {
  const node = document.createElement(tag);
  if (text) node.textContent = text;
  if (className) node.className = className;
  return node;
}

function link(text, url) {
  const node = element('a', text);
  // Treat release notes and API metadata as content, never HTML or executable URLs.
  const parsed = new URL(url);
  if (parsed.protocol !== 'https:' || parsed.hostname !== 'github.com') throw new Error('Unexpected download URL');
  node.href = parsed.href;
  return node;
}

function fileList(files, showPlatform = true) {
  const links = element('div', '', 'downloads');
  for (const file of files) {
    const name = file.name.toLowerCase();
    let label = '';
    if (file.support) label = file.name;
    else if (showPlatform) label = file.platform === 'UWP' ? 'Xbox / UWP' : file.platform;
    if (!file.support) {
      if (/arm64|aarch64/.test(name)) label += ' ARM64';
      else if (/x86_64|x64|amd64/.test(name)) label += ' x64';
      else if (/i686|x86|win32/.test(name)) label += ' x86';
      else if (/universal/.test(name)) label += ' Universal';
      if (name.includes('.apk')) label += ' APK';
      else if (name.includes('.aab')) label += ' AAB';
      else if (name.includes('.appimage')) label += ' AppImage';
      if (name.includes('libretro')) label += ' Libretro';
      if (name.includes('msvc')) label += ' MSVC';
      else if (name.includes('mingw')) label += ' MinGW';
    }
    const download = link(label.trim() || 'Download', file.url);
    const size = file.size < 1048576 ? `${Math.ceil(file.size / 1024)} KB` : `${(file.size / 1048576).toFixed(1)} MB`;
    download.title = `${file.name} · ${size}`;
    links.append(download);
  }
  return links;
}

function render() {
  if (!catalog) return;
  const fragments = { master: document.createDocumentFragment(), dev: document.createDocumentFragment(), releases: document.createDocumentFragment() };
  const counts = { master: 0, dev: 0, releases: 0 };
  let count = 0;
  const query = search.value.trim().toLowerCase();
  for (const build of catalog.builds) {
    if (['master', 'dev'].includes(channel.value) ? build.channel !== 'ci' || build.branch !== channel.value :
      channel.value !== 'all' && build.channel !== channel.value) continue;
    const files = build.files.filter(file => platform.value === 'all' || file.platform === platform.value);
    if (!files.length || (query && !`${build.title} ${build.branch || ''} ${build.commit || ''} ${files.map(file => file.name).join(' ')}`.toLowerCase().includes(query))) continue;
    count++;
    const group = build.channel === 'ci' ? build.branch : 'releases';
    counts[group]++;
    const row = element('details', '', 'build');
    const heading = element('summary', '', 'build-heading');
    const id = build.commit ? build.commit.slice(0, 8) : new URL(build.url).pathname.split('/').at(-1);
    heading.append(element('code', id, 'build-id'));
    const date = new Date(build.date);
    const time = element('time', date.toLocaleDateString());
    time.dateTime = date.toISOString();
    time.title = date.toLocaleString();
    time.className = 'meta';
    const platforms = [...new Set(files.filter(file => !file.support).map(file => file.platform === 'UWP' ? 'Xbox / UWP' : file.platform))].sort();
    heading.append(time, element('span', platforms.join(' · '), 'platforms'), element('span', 'Downloads & changes', 'row-action'));
    const content = element('div', '', 'build-content');
    if (build.channel === 'ci') content.append(element('p', 'Public ZIP archives. Extract before use.', 'meta'));
    const downloads = element('div', '', 'download-columns');
    for (const device of ['Windows', 'Android', 'UWP', 'Linux', 'macOS', 'Switch', 'Other']) {
      const deviceFiles = files.filter(file => !file.support && file.platform === device);
      if (!deviceFiles.length) continue;
      const standalone = deviceFiles.filter(file => !file.name.toLowerCase().includes('libretro'));
      const primary = standalone.find(file => /x64|x86_64|amd64/i.test(file.name) && /msvc/i.test(file.name)) ||
        standalone.find(file => /x64|x86_64|amd64/i.test(file.name)) || standalone[0];
      const column = element('section', '', 'download-platform');
      column.append(element('h4', device === 'UWP' ? 'Xbox / UWP' : device));
      if (device === 'UWP') column.append(element('p', 'Xbox uses x64.', 'meta'));
      column.append(fileList(primary ? [primary, ...deviceFiles.filter(file => file !== primary)] : deviceFiles, false));
      downloads.append(column);
    }
    content.append(downloads);
    const supporting = files.filter(file => file.support);
    if (supporting.length) {
      const details = element('details');
      details.append(element('summary', 'Checksums & extras'), fileList(supporting));
      content.append(details);
    }
    const changelog = element('details');
    changelog.append(element('summary', 'Changelog'));
    const notes = element('div', '', 'notes');
    const text = build.notes || 'No release notes were provided. See the source details on GitHub.';
    let offset = 0;
    // Link GitHub references without interpreting release-supplied HTML or scripts.
    for (const match of text.matchAll(/\[([^\]\n]+)\]\((https:\/\/github\.com\/[^\s)]+)\)/g)) {
      notes.append(text.slice(offset, match.index), link(match[1], match[2]));
      offset = match.index + match[0].length;
    }
    notes.append(text.slice(offset));
    const source = element('p');
    source.append(link('View changes on GitHub ↗', build.compare || build.url));
    changelog.append(notes, source);
    content.append(changelog);
    row.append(heading, content);
    fragments[group].append(row);
  }
  for (const group of ['master', 'dev', 'releases']) {
    const visible = group === 'releases' ? ['all', 'stable', 'development'].includes(channel.value) :
      ['ci', 'all', group].includes(channel.value);
    document.querySelector(`#${group}-column`).hidden = !visible;
    if (!counts[group]) fragments[group].append(element('p', 'No matching builds available.', 'meta'));
    document.querySelector(group === 'releases' ? '#release-builds' : `#${group}-builds`).replaceChildren(fragments[group]);
  }
  status.textContent = count ? `${count} ${count === 1 ? 'build' : 'builds'}` : 'No builds match these filters.';
}

async function refresh() {
  try {
    const response = await fetch('builds.json', { cache: 'no-store', signal: AbortSignal.timeout(20000) });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const next = await response.json();
    if (!Array.isArray(next.builds)) throw new Error('Invalid build catalog');
    catalog = next;
    render();
    document.querySelector('#updated').textContent = `Catalog updated ${new Date(catalog.updated).toLocaleString()}`;
  } catch (error) {
    status.replaceChildren(element('span', catalog ? 'Refresh failed; showing the last loaded builds. ' : 'Builds could not be loaded. '), link('Browse downloads on GitHub', 'https://github.com/nexus382/Hollycast/releases'));
    console.error('Build catalog could not be loaded:', error);
  }
}

channel.addEventListener('change', render);
platform.addEventListener('change', render);
search.addEventListener('input', render);
refresh();
setInterval(refresh, 5 * 60 * 1000);
