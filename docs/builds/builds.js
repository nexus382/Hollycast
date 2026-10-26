// Portions Copyright 2026 The Hollycast Authors
'use strict';

const list = document.querySelector('#builds');
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

function fileList(files) {
  const links = element('div', '', 'downloads');
  for (const file of files) {
    const name = file.name.toLowerCase();
    let label = file.support ? file.name : file.platform;
    if (!file.support) {
      if (/arm64|aarch64/.test(name)) label += ' ARM64';
      else if (/x86_64|x64|amd64/.test(name)) label += ' x64';
      else if (/i686|x86|win32/.test(name)) label += ' x86';
      else if (/universal/.test(name)) label += ' Universal';
      if (name.includes('libretro')) label += ' Libretro';
      if (name.includes('msvc')) label += ' MSVC';
      else if (name.includes('mingw')) label += ' MinGW';
    }
    const download = link(label, file.url);
    const size = file.size < 1048576 ? `${Math.ceil(file.size / 1024)} KB` : `${(file.size / 1048576).toFixed(1)} MB`;
    download.title = `${file.name} · ${size}`;
    links.append(download);
  }
  return links;
}

function render() {
  if (!catalog) return;
  const fragment = document.createDocumentFragment();
  let count = 0;
  const query = search.value.trim().toLowerCase();
  for (const build of catalog.builds) {
    if (['master', 'dev'].includes(channel.value) ? build.channel !== 'ci' || build.branch !== channel.value :
      channel.value !== 'all' && build.channel !== channel.value) continue;
    const files = build.files.filter(file => platform.value === 'all' || file.platform === platform.value);
    if (!files.length || (query && !`${build.title} ${build.branch || ''} ${build.commit || ''} ${files.map(file => file.name).join(' ')}`.toLowerCase().includes(query))) continue;
    count++;
    const row = element('tr', '', 'build');
    const title = element('th');
    title.scope = 'row';
    const release = link(build.title, build.url);
    release.className = 'build-title';
    title.append(release);
    if (build.commit) title.append(element('span', build.commit.slice(0, 8), 'meta'));
    const branch = element('td');
    branch.append(element('span', build.branch || build.channel, 'badge'));
    const date = new Date(build.date);
    const time = element('time', date.toLocaleDateString());
    time.dateTime = date.toISOString();
    time.title = date.toLocaleString();
    const published = element('td', '', 'published');
    published.append(time);
    const content = element('td');
    content.append(fileList(files.filter(file => !file.support)));
    const supporting = files.filter(file => file.support);
    if (supporting.length) {
      const details = element('details');
      details.append(element('summary', 'Checksums & extras'), fileList(supporting));
      content.append(details);
    }
    const changelog = element('td');
    changelog.append(link('Changes ↗', build.compare || build.url));
    row.append(title, branch, published, content, changelog);
    fragment.append(row);
  }
  list.replaceChildren(fragment);
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
