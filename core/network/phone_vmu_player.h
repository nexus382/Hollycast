// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
namespace phone_vmu {
inline constexpr char standalonePlayer[] = R"VMUPLAYER(
'use strict';
// Runs entirely in the phone. Neither input nor modified flash is sent to the host.
class PhoneVmuPlayer {
  constructor(core, redraw, describe, tone) {
    this.core = core; this.redraw = redraw; this.describe = describe; this.tone = tone;
    this.active = false; this.sleeping = false; this.queue = []; this.last = 0; this.acc = 0;
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
    for (let key = 0; key < 8; ++key) this.core.key(key, false);
  }
  sleep() {
    this.releaseAll(); this.sleeping = !this.sleeping; this.last = 0; this.acc = 0;
    this.tone(-1);
    this.redraw(this.sleeping ? new Uint8Array(48 * 40) : this.core.lcd);
    this.describe(this.sleeping ? 'Sleeping. Press Sleep to wake the VMU.' : 'Mini-game resumed.');
  }
  stop() { this.releaseAll(); this.active = false; this.sleeping = false; this.tone(-1); }
  tick(time, hidden = false) {
    if (!this.active || this.sleeping || hidden || this.core.halted) { this.last = 0; return; }
    if (!this.last) this.last = time;
    this.acc += Math.min(Math.max(time - this.last, 0), 100); this.last = time;
    const slices = Math.floor(this.acc / 10);
    if (!slices) return;
    this.acc -= slices * 10;
    this.core.run(slices, () => { const event = this.queue.shift(); if (event) this.core.key(...event); });
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
