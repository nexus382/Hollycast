// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
import assert from 'node:assert/strict';
import {engine} from './mini_game.mjs';

const {Player} = engine();

// Build a minimal VMU file system: root block 255, FAT 253, directory 254,
// one two-block game file at blocks 0-1, everything else free.
function testCard() {
  const flash = new Uint8Array(131072);
  const word = (at, v) => { flash[at] = v & 0xff; flash[at + 1] = v >> 8; };
  const root = 255 * 512;
  flash.fill(0x55, root, root + 16);
  word(root + 0x46, 253); word(root + 0x4a, 254); word(root + 0x4c, 1);
  for (let b = 0; b < 200; ++b) word(253 * 512 + b * 2, 0xfffc);
  word(253 * 512 + 254 * 2, 0xfffa);
  word(253 * 512, 1); word(253 * 512 + 2, 0xfffa);
  flash.fill(0x41, 0, 512); flash.fill(0x42, 512, 1024);
  const entry = 254 * 512;
  flash[entry] = 0xcc; word(entry + 2, 0); word(entry + 0x18, 2); word(entry + 0x1a, 1);
  [...'TESTGAME'].forEach((c, i) => { flash[entry + 4 + i] = c.charCodeAt(0); });
  return flash;
}

const flash = testCard();
let listed = Player.listFiles(flash);
assert.equal(listed.files.length, 1);
assert.equal(listed.files[0].name, 'TESTGAME');
assert.equal(listed.files[0].game, true);
assert.equal(listed.files[0].blocks, 2);
assert.equal(listed.freeBlocks, 198, 'Two file blocks must not count as free');
const extracted = Player.extractFile(flash, listed.files[0]);
assert.equal(extracted.length, 1024);
assert.ok(extracted.subarray(0, 512).every(b => b === 0x41));
assert.ok(extracted.subarray(512).every(b => b === 0x42));

const save = new Uint8Array(513).fill(0x43);
assert.equal(Player.importFile(flash, 'NEWSAVE', save, false), true);
listed = Player.listFiles(flash);
assert.equal(listed.files.length, 2);
assert.equal(listed.freeBlocks, 196);
const imported = listed.files.find(f => f.name === 'NEWSAVE');
assert.equal(imported.game, false);
const extractedSave = Player.extractFile(flash, imported);
assert.equal(extractedSave.length, 1024, 'A partial final block must be padded to the VMU block size');
assert.ok(extractedSave.subarray(0, save.length).every(b => b === 0x43));
assert.ok(extractedSave.subarray(save.length).every(b => b === 0), 'Padding must be zero-filled');
let beforeFailure = flash.slice();
assert.equal(Player.importFile(flash, 'TOOLONGNAME123', new Uint8Array(300 * 512), false), false,
  'Import larger than the card must fail');
assert.deepEqual(flash, beforeFailure, 'A failed import must leave every card byte unchanged');
beforeFailure = flash.slice();
assert.equal(Player.importFile(flash, 'NEWSAVE', new Uint8Array(512), false), false, 'Duplicate names must be rejected');
assert.deepEqual(flash, beforeFailure, 'Duplicate-name rejection must leave every card byte unchanged');
assert.equal(Player.listFiles(flash).files.length, 2);

const staleCard = testCard();
const staleFile = Player.listFiles(staleCard).files[0];
const staleEntry = 254 * 512;
staleCard.fill(0, staleEntry + 4, staleEntry + 16);
[...'RENAMED'].forEach((c, i) => { staleCard[staleEntry + 4 + i] = c.charCodeAt(0); });
beforeFailure = staleCard.slice();
assert.equal(Player.deleteFile(staleCard, staleFile), false, 'A stale descriptor must match the current file name');
assert.deepEqual(staleCard, beforeFailure, 'A stale delete must leave the card unchanged');

const badChain = testCard();
const badFile = Player.listFiles(badChain).files[0];
badChain[253 * 512 + 2] = 0; badChain[253 * 512 + 3] = 0;
beforeFailure = badChain.slice();
assert.equal(Player.listFiles(badChain), null, 'Cyclic data chains must be rejected');
assert.equal(Player.extractFile(badChain, badFile), null, 'Malformed exports must fail without throwing');
assert.equal(Player.deleteFile(badChain, badFile), false, 'Malformed chains must not be freed');
assert.equal(Player.importFile(badChain, 'BROKEN', new Uint8Array(512), false), false, 'Malformed cards must reject imports');
assert.deepEqual(badChain, beforeFailure, 'Malformed-chain operations must leave the card unchanged');

const crossLinked = testCard();
const secondEntry = 254 * 512 + 32;
crossLinked[secondEntry] = 0x33;
crossLinked[secondEntry + 2] = 1;
crossLinked[secondEntry + 0x18] = 1;
assert.equal(Player.listFiles(crossLinked), null, 'Cross-linked file chains must be rejected');
const systemBlock = testCard();
systemBlock[254 * 512 + 2] = 253;
systemBlock[254 * 512 + 3] = 0;
assert.equal(Player.listFiles(systemBlock), null, 'System blocks must not be exposed as file data');
const shortDirectory = testCard();
shortDirectory[255 * 512 + 0x4c] = 2;
shortDirectory[255 * 512 + 0x4d] = 0;
assert.equal(Player.listFiles(shortDirectory), null, 'Directory chain length must match the root metadata');

const preserved = testCard();
preserved[253 * 512 + 5 * 2] = 0xfa; preserved[253 * 512 + 5 * 2 + 1] = 0xff;
const unknownEntry = 254 * 512 + 32;
preserved[unknownEntry] = 0x77;
preserved[unknownEntry + 2] = 6;
preserved[unknownEntry + 0x18] = 1;
preserved[253 * 512 + 6 * 2] = 0xfa; preserved[253 * 512 + 6 * 2 + 1] = 0xff;
const preservedFile = Player.listFiles(preserved).files[0];
assert.equal(Player.importFile(preserved, 'SAFE', new Uint8Array(512), false), true);
const safeFile = Player.listFiles(preserved).files.find(f => f.name === 'SAFE');
assert.equal(safeFile.first, 2, 'Import must allocate only FAT-marked free blocks');
assert.equal(Player.deleteFile(preserved, safeFile), true);
assert.equal(Player.fatAt(preserved, 253, 5), 0xfffa, 'Unreferenced allocated blocks must be preserved');
assert.equal(Player.fatAt(preserved, 253, 6), 0xfffa, 'Unknown file allocations must be preserved');
assert.equal(Player.deleteFile(preserved, preservedFile), true);
assert.equal(Player.fatAt(preserved, 253, 5), 0xfffa);
assert.equal(Player.fatAt(preserved, 253, 6), 0xfffa);

const freshCard = testCard();
assert.equal(Player.deleteFile(freshCard, Player.listFiles(freshCard).files[0]), true);
beforeFailure = freshCard.slice();
assert.equal(Player.importFile(freshCard, 'SHORTGAME', new Uint8Array(512), true), false,
  'A game needs a block beyond its one-block VMU header');
assert.deepEqual(freshCard, beforeFailure, 'A one-block game rejection must leave every card byte unchanged');
assert.equal(Player.importFile(freshCard, 'NEWGAME', new Uint8Array(513).fill(0x55), true), true);
const game = Player.listFiles(freshCard).files[0];
assert.equal(game.first, 0, 'Mini-game allocation must begin at data block zero');
assert.equal(game.blocks, 2);
assert.equal(Player.hasGame(freshCard), true, 'An imported game must satisfy the VMU boot layout');
beforeFailure = freshCard.slice();
assert.equal(Player.importFile(freshCard, 'SECONDGAME', new Uint8Array(512), true), false,
  'A card may contain at most one block-zero game');
assert.deepEqual(freshCard, beforeFailure, 'A rejected second game must leave every card byte unchanged');

assert.equal(Player.deleteFile(flash, imported), true);
listed = Player.listFiles(flash);
assert.equal(listed.files.length, 1);
assert.equal(listed.freeBlocks, 198, 'Delete must return the block to the free pool');
assert.equal(Player.listFiles(new Uint8Array(131072)), null, 'Blank cards have no file system');

console.log('Phone card manager filesystem, import, export, delete, and corruption checks passed');
