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

function fileList(files, ci) {
  const ul = element('ul', '', 'downloads');
  for (const file of files) {
    const item = element('li');
    item.append(link(file.name, file.url));
    const size = file.size < 1048576 ? `${Math.ceil(file.size / 1024)} KB` : `${(file.size / 1048576).toFixed(1)} MB`;
    item.append(element('span', `${size}${ci ? ' · ZIP · GitHub sign-in required' : ''}`, 'file-size'));
    ul.append(item);
  }
  return ul;
}

function render() {
  if (!catalog) return;
  const fragment = document.createDocumentFragment();
  let count = 0;
  const query = search.value.trim().toLowerCase();
  for (const build of catalog.builds) {
    if (channel.value === 'all' ? build.channel === 'ci' : build.channel !== channel.value) continue;
    const files = build.files.filter(file => (!file.expires || Date.parse(file.expires) > Date.now()) &&
      (platform.value === 'all' || file.platform === platform.value));
    if (!files.length || (query && !`${build.title} ${build.commit || ''} ${files.map(file => file.name).join(' ')}`.toLowerCase().includes(query))) continue;
    count++;
    const article = element('article');
    const heading = element('div', '', 'build-heading');
    const title = element('h3');
    title.append(link(build.title, build.url));
    heading.append(title, element('span', build.channel === 'ci' ? `CI · ${build.branch}` : build.channel, 'badge'));
    const date = new Date(build.date);
    const time = element('time', date.toLocaleString());
    time.dateTime = date.toISOString();
    const meta = element('p', '', 'meta');
    meta.append(time);
    if (build.commit) meta.append(` · ${build.commit.slice(0, 8)}`);
    article.append(heading, meta);
    article.append(fileList(files.filter(file => !file.support), build.channel === 'ci'));
    const supporting = files.filter(file => file.support);
    if (supporting.length) {
      const details = element('details');
      details.append(element('summary', 'Checksums, certificates & installation files'), fileList(supporting, false));
      article.append(details);
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
    changelog.append(notes);
    const source = element('p');
    source.append(link(build.compare ? 'View changes on GitHub ↗' : 'View full details on GitHub ↗', build.compare || build.url));
    changelog.append(source);
    article.append(changelog);
    fragment.append(article);
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
