// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
namespace phone_vmu {
inline constexpr char standalonePlayer[] = R"VMUPLAYER(
'use strict';
// CPU and input run in the phone. PhoneVmuSync checkpoints paired cards to the host.
class PhoneVmuPlayer {
  constructor(core, redraw, describe, tone) {
    this.core = core; this.redraw = redraw; this.describe = describe; this.tone = tone;
    this.active = false; this.sleeping = false; this.queue = []; this.last = 0; this.acc = 0;
    this.speed = 1;
    core.hooks.redraw = () => redraw(core.lcd);
    core.hooks.sound = tone;
    core.hooks.error = message => describe(message);
  }
  static hasGame(bytes) {
    if (bytes.length !== 131072) return bytes.length >= 0x480 && bytes.length <= 200 * 512;
    const word = at => bytes[at] | bytes[at + 1] << 8;
    const root = 255 * 512;
    if (!bytes.subarray(root, root + 16).every(b => b === 0x55)) return false;
    const fat = word(root + 0x46), count = word(root + 0x4c);
    let block = word(root + 0x4a);
    if (fat >= 256 || count < 1 || count > 13) return false;
    const visited = new Set();
    for (let n = 0; n < count && block < 256 && !visited.has(block); ++n) {
      visited.add(block);
      for (let i = 0; i < 16; ++i) {
        const entry = block * 512 + i * 32;
        if (bytes[entry] !== 0xcc || word(entry + 2) !== 0) continue;
        const size = word(entry + 0x18);
        if (size < 2 || size > 200) return false;
        for (let b = 0; b < size; ++b)
          if (word(fat * 512 + b * 2) !== (b === size - 1 ? 0xfffa : b + 1)) return false;
        return true;
      }
      block = word(fat * 512 + block * 2);
    }
    return false;
  }
  // VMU filesystem helpers for the card manager. Layout mirrors hasGame():
  // root block 255, FAT and directory chains, 32-byte directory entries.
  static cardLayout(flash) {
    if (!flash || flash.length !== 131072) return null;
    const word = at => flash[at] | flash[at + 1] << 8;
    const root = 255 * 512;
    if (!flash.subarray(root, root + 16).every(b => b === 0x55)) return null;
    const fat = word(root + 0x46), dir = word(root + 0x4a), dirBlocks = word(root + 0x4c);
    if (fat < 200 || fat >= 255 || dir < 200 || dir >= 255 || fat === dir || dirBlocks < 1 || dirBlocks > 13) return null;
    return { fat, dir, dirBlocks };
  }
  static fatAt(flash, fat, block) { const at = fat * 512 + block * 2; return flash[at] | flash[at + 1] << 8; }
  static setFat(flash, fat, block, value) { const at = fat * 512 + block * 2; flash[at] = value & 0xff; flash[at + 1] = value >> 8; }
  static blockChain(flash, fat, first, system = false) {
    const min = system ? 200 : 0, max = system ? 255 : 200;
    if (!Number.isInteger(first) || first < min || first >= max || first === fat) return null;
    const blocks = [], seen = new Set();
    let block = first;
    while (true) {
      if (block < min || block >= max || block === fat || seen.has(block)) return null;
      seen.add(block); blocks.push(block);
      const next = PhoneVmuPlayer.fatAt(flash, fat, block);
      if (next === 0xfffa) return blocks;
      if (next < min || next >= max || next === fat) return null;
      block = next;
    }
  }
  static allocationMap(flash, fat) {
    const incoming = new Uint8Array(200), visited = new Uint8Array(200);
    const fatValues = new Uint16Array(200);
    for (let block = 0; block < 200; ++block) {
      const next = PhoneVmuPlayer.fatAt(flash, fat, block);
      fatValues[block] = next;
      if (next === 0xfffc || next === 0xfffb || next === 0xfffa) continue;
      if (next >= 200 || ++incoming[next] > 1) return null;
    }
    for (let first = 0; first < 200; ++first) {
      const value = fatValues[first];
      if (value === 0xfffc || value === 0xfffb || incoming[first] !== 0) continue;
      let block = first;
      while (true) {
        if (visited[block]) return null;
        visited[block] = 1;
        const next = fatValues[block];
        if (next === 0xfffa) break;
        if (next >= 200) return null;
        block = next;
      }
    }
    for (let block = 0; block < 200; ++block)
      if (fatValues[block] !== 0xfffc && fatValues[block] !== 0xfffb && !visited[block]) return null;
    return { incoming, fatValues };
  }
  static inspectCard(flash) {
    const layout = PhoneVmuPlayer.cardLayout(flash);
    if (!layout) return null;
    const dirBlocks = PhoneVmuPlayer.blockChain(flash, layout.fat, layout.dir, true);
    if (!dirBlocks || dirBlocks.length !== layout.dirBlocks || dirBlocks.includes(layout.fat)) return null;
    const allocation = PhoneVmuPlayer.allocationMap(flash, layout.fat);
    if (!allocation) return null;
    const word = at => flash[at] | flash[at + 1] << 8;
    const files = [], owned = new Uint8Array(200);
    let freeBlocks = 0, gameCount = 0;
    for (let block = 0; block < 200; ++block)
      if (allocation.fatValues[block] === 0xfffc) ++freeBlocks;
    for (let dirIndex = 0; dirIndex < dirBlocks.length * 16; ++dirIndex) {
      const entry = dirBlocks[Math.floor(dirIndex / 16)] * 512 + (dirIndex % 16) * 32;
      const type = flash[entry];
      if (type === 0) continue;
      const first = word(entry + 2), blocks = word(entry + 0x18);
      if (blocks === 0) {
        if (type === 0x33 || type === 0xcc) return null;
        continue;
      }
      if (blocks > 200 || first >= 200 || allocation.incoming[first] !== 0) return null;
      const chain = PhoneVmuPlayer.blockChain(flash, layout.fat, first);
      if (!chain || chain.length !== blocks) return null;
      for (const dataBlock of chain) {
        if (owned[dataBlock]) return null;
        owned[dataBlock] = 1;
      }
      const game = type === 0xcc;
      if ((type === 0x33 || game) && word(entry + 0x1a) !== (game ? 1 : 0)) return null;
      if (game) {
        if (++gameCount > 1 || blocks < 2 || first !== 0 || chain.some((dataBlock, i) => dataBlock !== i)) return null;
      }
      if (type !== 0x33 && !game) continue;
      const name = String.fromCharCode(...flash.subarray(entry + 4, entry + 16)).replace(/[\0 ]+$/, '') || 'UNNAMED';
      files.push({ index: dirIndex, name, game, first, blocks, chain });
    }
    return { layout, dirBlocks, files, freeBlocks, allocation };
  }
  static findFile(card, file) {
    if (!card || !file || !Number.isInteger(file.index) || typeof file.name !== 'string') return null;
    return card.files.find(current => current.index === file.index && current.name === file.name
      && current.game === file.game && current.first === file.first && current.blocks === file.blocks) || null;
  }
  static listFiles(flash) {
    const card = PhoneVmuPlayer.inspectCard(flash);
    return card ? { files: card.files, freeBlocks: card.freeBlocks } : null;
  }
  static extractFile(flash, file) {
    const card = PhoneVmuPlayer.inspectCard(flash), current = PhoneVmuPlayer.findFile(card, file);
    if (!current) return null;
    const out = new Uint8Array(current.blocks * 512);
    current.chain.forEach((block, i) => out.set(flash.subarray(block * 512, block * 512 + 512), i * 512));
    return out;
  }
  static deleteFile(flash, file) {
    const card = PhoneVmuPlayer.inspectCard(flash), current = PhoneVmuPlayer.findFile(card, file);
    if (!current) return false;
    const entry = card.dirBlocks[Math.floor(current.index / 16)] * 512 + (current.index % 16) * 32;
    for (const block of current.chain) PhoneVmuPlayer.setFat(flash, card.layout.fat, block, 0xfffc);
    flash[entry] = 0;
    return true;
  }
  static importFile(flash, name, data, game) {
    const card = PhoneVmuPlayer.inspectCard(flash);
    if (!card || !(data instanceof Uint8Array) || data.length === 0 || data.length > 200 * 512
        || typeof name !== 'string' || typeof game !== 'boolean') return false;
    const clean = name.replace(/[^\x20-\x7e]/g, ' ').trim().slice(0, 12);
    if (!clean || card.files.some(file => file.name.toUpperCase() === clean.toUpperCase())) return false;
    if (game && (data.length <= 512 || card.files.some(file => file.game))) return false;
    const input = new Uint8Array(data), needed = Math.ceil(input.length / 512), free = [];
    if (game) {
      for (let block = 0; block < needed; ++block) {
        if (card.allocation.fatValues[block] !== 0xfffc) return false;
        free.push(block);
      }
    } else {
      for (let block = 0; block < 200 && free.length < needed; ++block)
        if (card.allocation.fatValues[block] === 0xfffc) free.push(block);
    }
    if (free.length !== needed) return false;
    let entry = -1;
    for (let i = 0; i < card.dirBlocks.length * 16; ++i) {
      const offset = card.dirBlocks[Math.floor(i / 16)] * 512 + (i % 16) * 32;
      if (flash[offset] === 0) { entry = offset; break; }
    }
    if (entry < 0) return false;
    for (let i = 0; i < free.length; ++i) {
      const block = free[i], start = i * 512;
      flash.fill(0, block * 512, (block + 1) * 512);
      flash.set(input.subarray(start, Math.min(start + 512, input.length)), block * 512);
      PhoneVmuPlayer.setFat(flash, card.layout.fat, block, i === free.length - 1 ? 0xfffa : free[i + 1]);
    }
    flash.fill(0, entry, entry + 32);
    flash[entry] = game ? 0xcc : 0x33;
    flash[entry + 2] = free[0] & 0xff; flash[entry + 3] = free[0] >> 8;
    for (let i = 0; i < clean.length; ++i) flash[entry + 4 + i] = clean.charCodeAt(i);
    flash[entry + 0x18] = needed & 0xff; flash[entry + 0x19] = needed >> 8;
    flash[entry + 0x1a] = game ? 1 : 0;
    return true;
  }
  load(bytes, name, bios = null) {
    if (!(bytes instanceof Uint8Array) || bytes.length < 0x480 || bytes.length > 131072 || (!bios && !PhoneVmuPlayer.hasGame(bytes)))
      throw new Error('No runnable VMU mini-game was found. Open a game .vms file or a VMU card containing a mini-game.');
    if (bios && bios.length !== 65536) throw new Error('A VMU BIOS must be exactly 64 KiB.');
    this.core.setBios(bios);
    if (!this.core.load(bytes, name)) throw new Error('This VMU game file is too short.');
    this.name = name; this.active = true;
    this.reset();
  }
  reset() {
    this.releaseAll(); this.sleeping = false; this.last = 0; this.acc = 0;
    this.core.reset(this.core.dateNow()); this.redraw(this.core.lcd);
    this.describe('Mini-game ready. Some games start with A and B together.');
  }
  key(key, pressed) { if (this.active && !this.sleeping && !this.core.halted) this.queue.push([key, pressed]); }
  releaseAll() {
    this.queue = [];
    // The link scheduler applies input between agreed clock rounds.
    if (this.core.linked) this.queue.push(null);
    else this.releaseKeys();
  }
  releaseKeys() {
    for (let key = 0; key < 8; ++key) this.core.key(key, false);
  }
  applyInput() {
    const event = this.queue.shift();
    if (event === null) this.releaseKeys();
    else if (event) this.core.key(...event);
  }
  sleep() {
    this.releaseAll(); this.sleeping = !this.sleeping; this.last = 0; this.acc = 0;
    this.tone(-1);
    this.redraw(this.sleeping ? new Uint8Array(48 * 40) : this.core.lcd);
    this.describe(this.sleeping ? 'Sleeping. Press Sleep to wake the VMU.' : 'Mini-game resumed.');
  }
  stop() { this.releaseAll(); this.active = false; this.sleeping = false; this.tone(-1); }
  setSpeed(speed) {
    if (speed !== 0.25 && speed !== 0.5 && (!Number.isInteger(speed) || speed < 1 || speed > 10))
      throw new RangeError('Choose 0.25, 0.5, or a whole-number VMU speed from 1 to 10.');
    this.speed = speed; this.last = 0; this.acc = 0;
  }
  tick(time, hidden = false) {
    if (!this.active || this.sleeping || hidden || this.core.halted) { this.last = 0; return; }
    if (!this.last) this.last = time;
    // Accelerate guest time only; network checkpoints continue on their real-time schedule.
    this.acc = Math.min(this.acc + Math.min(Math.max(time - this.last, 0), 100) * this.speed, 100 * this.speed);
    this.last = time;
    const slices = Math.floor(this.acc / 10);
    if (!slices || this.core.serialWaiting) return;
    const completed = this.core.run(slices, () => this.applyInput());
    this.acc -= completed * 10;
    this.finishIfHalted();
  }
  finishIfHalted() {
    if (this.core.halted) {
      this.releaseAll(); this.tone(-1);
      this.describe(this.core.halted === 'exit'
        ? 'The game has exited. Choose Restart or return to the Dreamcast display.'
        : 'This game needs firmware support that is unavailable. Try loading your VMU BIOS.');
    }
  }
}
)VMUPLAYER";
}
