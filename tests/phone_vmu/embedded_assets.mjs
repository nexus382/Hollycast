// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {fileURLToPath} from 'node:url';

const assets=[
 ['../../core/network/phone_vmu_page.h','HTML'],
 ['../../core/deps/vmu-softvms/core.h','VMUJS'],
 ['../../core/deps/vmu-softvms/core.h','VMULICENSE'],
 ['../../core/network/phone_vmu_player.h','VMUPLAYER'],
 ['../../core/network/phone_vmu_sync.h','VMUSYNC'],
 ['../../core/deps/nosleep/media.h','WAKEJS'],
];
export function assetChunks(file,tag){
 const text=readFileSync(new URL(file,import.meta.url),'utf8').replace(/\r\n/g,'\n');
 const chunks=[...text.matchAll(new RegExp('R"'+tag+'\\(([\\s\\S]*?)\\)'+tag+'"','g'))].map(match=>match[1]);
 assert.ok(chunks.length,`Missing embedded asset ${tag} in ${file}`);
 return chunks;
}
export function readAsset(file,tag){return assetChunks(file,tag).join('');}

if(process.argv[1]===fileURLToPath(import.meta.url)){
 for(const [file,tag] of assets){
  const chunks=assetChunks(file,tag);
  for(const [index,chunk] of chunks.entries())
   assert.ok(Buffer.byteLength(chunk)<=16380,`${file}: ${tag} literal ${index+1} exceeds MSVC's 16380-byte limit (${Buffer.byteLength(chunk)} bytes)`);
  assert.ok(Buffer.byteLength(chunks.join(''))<65535,`${file}: ${tag} exceeds MSVC's concatenated string limit`);
 }
 const license=readFileSync(new URL('../../core/deps/vmu-softvms/COPYING',import.meta.url),'utf8').replace(/\r\n/g,'\n');
 assert.equal(readAsset('../../core/deps/vmu-softvms/core.h','VMULICENSE'),license,'Served VMU license must preserve its complete text');
 console.log('Embedded phone VMU assets fit MSVC string limits and preserve the complete VMU license');
}
