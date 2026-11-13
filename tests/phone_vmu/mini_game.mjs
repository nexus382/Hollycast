// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
import assert from 'node:assert/strict';
import {readFileSync,writeFileSync} from 'node:fs';
import {fileURLToPath} from 'node:url';
import vm from 'node:vm';
import {readAsset} from './embedded_assets.mjs';
export function engine(context=vm.createContext({Uint8Array})) {
  for (const [file,tag] of [['../../core/deps/vmu-softvms/core.h','VMUJS'],['../../core/network/phone_vmu_player.h','VMUPLAYER']]) {
    vm.runInContext(readAsset(file,tag),context);
  }
  return {context,core:vm.runInContext('VMS',context),Player:vm.runInContext('PhoneVmuPlayer',context)};
}
// A homebrew LC86K program: copy the button port to flash and LCD repeatedly.
// No Sega game, firmware, or other copyrighted ROM is needed.
export function inputProgram() {
  const bytes=new Uint8Array(0x4000); bytes.set([0x21,0x02,0x00]);
  const code=[0x23,0x08,0x00, 0x23,0x05,0x30, 0x23,0x04,0x00, 0x23,0x54,0x00];
  const loop=0x200+code.length;
  code.push(0x03,0x4c,0x51,0x13,0x80,0x21,loop>>8,loop&255);
  bytes.set(code,0x200); return bytes;
}
if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const {core,Player}=engine();
  const game=inputProgram(),original=game.slice();
  const frames=[],messages=[];
  const player=new Player(core,lcd=>frames.push(lcd.slice()),msg=>messages.push(msg),()=>{});
  player.load(game,'input.vms');
  core.run(2); assert.equal(core.flash[0x3000],255,'CPU must read released input');
  player.key(4,true); player.key(5,true);
  player.tick(1); player.tick(31);
  assert.equal(core.flash[0x3000],207,'A+B must reach the VMU together');
  player.releaseAll(); core.run(2); assert.equal(core.flash[0x3000],255);
  player.sleep(); const before=core.flash.slice(); player.tick(50); player.tick(300);
  assert.deepEqual(core.flash,before,'Sleeping must pause execution');
  player.sleep(); player.key(0,true); player.tick(400); player.tick(420);
  assert.equal(core.flash[0x3000],254,'Wake must resume the VMU');
  assert.deepEqual(game,original,'Playing must not modify the source snapshot');
  assert.ok(Player.hasGame(core.flash));
  assert.ok(!Player.hasGame(new Uint8Array(131072)));
  assert.throws(()=>player.load(new Uint8Array(100),'bad.vms'),/No runnable/);
  assert.throws(()=>player.load(game,'game.vms',new Uint8Array(100)),/64 KiB/);
  player.stop(); assert.equal(player.active,false);
  assert.ok(frames.length>0);
  // Optional local acceptance fixture, downloaded separately from its author.
  if(process.argv[2]) {
    const tetris=new Uint8Array(readFileSync(process.argv[2]));
    player.load(tetris,'TETRIS.VMS'); core.run(100);
    assert.ok(!core.halted,'Public-domain Tetris must execute');
    assert.ok(core.lcd.some(pixel=>pixel),'Tetris must produce a visible LCD');
    if(process.argv[3]) writeFileSync(process.argv[3],core.flash);
  }
  console.log('Standalone VMU CPU, input, sleep, and copy-isolation checks passed');
}
