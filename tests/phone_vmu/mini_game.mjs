// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
import assert from 'node:assert/strict';
import {readFileSync,writeFileSync} from 'node:fs';
import {fileURLToPath} from 'node:url';
import vm from 'node:vm';
import {readAsset} from './embedded_assets.mjs';
export function engine(context=vm.createContext({Uint8Array})) {
  for (const [file,tag] of [['../../core/deps/vmu-softvms/core.h','VMUJS'],['../../core/network/phone_vmu_player.h','VMUPLAYER'],['../../core/network/phone_vmu_link.h','VMULINK']]) {
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
  const {context,core,Player}=engine();
  const game=inputProgram(),original=game.slice();
  const frames=[],messages=[];
  const player=new Player(core,lcd=>frames.push(lcd.slice()),msg=>messages.push(msg),()=>{});
  player.load(game,'input.vms');
  core.run(2); assert.equal(core.flash[0x3000],255,'CPU must read released input');
  player.key(4,true); player.key(5,true);
  player.tick(1); player.tick(31);
  assert.equal(core.flash[0x3000],207,'A+B must reach the VMU together');
  player.releaseAll(); core.run(2); assert.equal(core.flash[0x3000],255);
  // Two VMUs on the same page must keep their inputs, saves, and hooks separate.
  const secondCore=vm.runInContext('createVmu()',context);
  const secondPlayer=new Player(secondCore,()=>{},()=>{},()=>{});
  secondPlayer.load(game,'second-input.vms');
  secondCore.key(4,true);
  secondCore.run(2);
  assert.equal(secondCore.flash[0x3000],239);
  assert.equal(core.flash[0x3000],255);
  assert.notEqual(core.hooks,secondCore.hooks);
  // LC86K starts SIO1, drives SIO0 at SBR's rate, and records the incoming
  // byte, SIO flags and P7 pins in flash. Give each side a different payload.
  function linkProgram(outgoing) {
    const bytes=inputProgram();
    bytes.set([
      0x23,0x08,0x00, // Disable interrupts to test completion by polling.
      0x23,0x05,0x30, 0x23,0x04,0x00,
      0x23,0x44,0x00, 0x23,0x46,0x05, 0x23,0x45,0x05,
      0x23,0x32,0xe0, 0x23,0x31,outgoing, 0x23,0x34,0x08, 0x23,0x30,0x08,
      0x23,0x04,0x00,
      0x03,0x35, 0x51,0x13,0x80, // SBUF1 -> flash[0x3000]
      0x23,0x04,0x01, 0x03,0x34, 0x51,0x13,0x80,
      0x23,0x04,0x02, 0x03,0x30, 0x51,0x13,0x80,
      0x23,0x04,0x03, 0x03,0x5c, 0x51,0x13,0x80,
      0x21,0x02,0x1e,
    ],0x200);
    return bytes;
  }
  for (const overNetwork of [false,true]) {
    const left=vm.runInContext('createVmu()',context),right=vm.runInContext('createVmu()',context);
    new Player(left,()=>{},()=>{},()=>{}).load(linkProgram(0xa6),'link-left.vms');
    new Player(right,()=>{},()=>{},()=>{}).load(linkProgram(0x39),'link-right.vms');
    const messages=[];
    const connect=(from,to)=>from.connectSerial(value=>{
      if (!overNetwork) return to.receiveSerial(value);
      messages.push(()=>from.completeSerial(to.receiveSerialByte(value)));
    },overNetwork);
    connect(left,right);connect(right,left);
    for (let steps=0;steps<12000 && Math.min(left.guestTime,right.guestTime)<2;++steps) {
      (left.guestTime<=right.guestTime?left:right).run(0);
      if (messages.length) messages.shift()();
    }
    assert.deepEqual(left.flash.slice(0x3000,0x3004),Uint8Array.of(0x39,2,2,10));
    assert.deepEqual(right.flash.slice(0x3000,0x3004),Uint8Array.of(0xa6,2,2,10));
  }
  function serialProgram(internalClock) {
    const bytes=inputProgram();
    const code=[
      0x23,0x08,0x00, // Observe the completion flag with interrupts disabled.
      0x23,0x05,0x30,
      0x23,0x44,0x00, 0x23,0x46,0x04, 0x23,0x45,internalClock?0x04:0x00,
      0x23,0x32,0xe0, 0x23,0x31,0x00, 0x23,0x30,0x08,
    ];
    const loop=0x200+code.length;
    code.push(
      0x23,0x04,0x00, 0x03,0x31, 0x51,0x13,0x80, // SBUF0 -> flash[0x3000]
      0x23,0x04,0x01, 0x03,0x30, 0x51,0x13,0x80, // SCON0 -> flash[0x3001]
      0x21,loop>>8,loop&255,
    );
    bytes.set(code,0x200);
    return bytes;
  }
  for (const [scenario,internalClock,edgesBeforeDisconnect,byteMode,waiting,data,flags] of [
    ['solo',true,0,false,false,0xff,2],
    ['external clock',false,0,false,false,0x00,8],
    ['mid-byte',true,2,false,false,0xfc,2],
    ['pending bit',true,3,false,true,0xfc,2],
    ['pending byte',true,1,true,true,0xff,2],
  ]) {
    const serial=vm.runInContext('createVmu()',context);
    new Player(serial,()=>{},()=>{},()=>{}).load(serialProgram(internalClock),'serial.vms');
    if (edgesBeforeDisconnect) {
      let edges=0;
      serial.connectSerial(()=>{
        ++edges;
        return !byteMode && edges<=2?0:undefined;
      },byteMode);
      for (let steps=0;steps<1200 && edges<edgesBeforeDisconnect;++steps)
        serial.run(0);
      assert.equal(edges,edgesBeforeDisconnect,scenario);
      assert.equal(serial.serialWaiting,waiting,scenario);
      serial.connectSerial(null);
      serial.completeSerial(0); // A reply from the closed link must be ignored.
    }
    serial.run(2);
    assert.deepEqual(serial.flash.slice(0x3000,0x3002),Uint8Array.of(data,flags),scenario);
  }
  // Alternate delivery delays and rendering rates. Neither browser can run
  // past the other VMU's next serial byte, even when messages arrive late.
  const first=vm.runInContext('createVmu()',context),second=vm.runInContext('createVmu()',context);
  let redraws=0;
  new Player(first,()=>{++redraws;},()=>{},()=>{}).load(linkProgram(0xa6),'clock-first.vms');
  new Player(second,()=>{},()=>{},()=>{}).load(linkProgram(0x39),'clock-second.vms');
  const LinkClock=vm.runInContext('PhoneVmuLinkClock',context);
  let wallTime=0;
  const packets=[[],[]];
  const send=direction=>message=>{
    const queue=packets[direction];
    const due=Math.max(queue.at(-1)?.due||0,wallTime+1+(message[1]+direction)%13);
    queue.push({message,due});
  };
  const firstClock=new LinkClock(first,()=>{},send(0));
  const secondClock=new LinkClock(second,()=>{},send(1));
  const beforeProbe=first.flash.slice(),beforeRedraws=redraws;
  assert.notEqual(first.probeSerial(firstClock.window).byte,null);
  assert.deepEqual(first.flash,beforeProbe,'A clock proposal must leave the card unchanged');
  assert.equal(first.guestTime,0,'A clock proposal must leave the guest time unchanged');
  assert.equal(redraws,beforeRedraws,'Speculation must not redraw the LCD');
  for (;wallTime<500;++wallTime) {
    if (wallTime%17===0) firstClock.tick(wallTime);
    if (wallTime%53===0) secondClock.tick(wallTime);
    for (const [queue,peer] of [[packets[0],secondClock],[packets[1],firstClock]]) {
      while (queue.length&&queue[0].due<=wallTime) peer.receive(queue.shift().message);
    }
  }
  assert.deepEqual(first.flash.slice(0x3000,0x3004),Uint8Array.of(0x39,2,2,10));
  assert.deepEqual(second.flash.slice(0x3000,0x3004),Uint8Array.of(0xa6,2,2,10));
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
  // Count guest time without depending on a game's animation or frame rate.
  let slicesRun=0;
  const clockCore={hooks:{},lcd:new Uint8Array(48*40),key(){},run(slices){slicesRun+=slices;return slices;}};
  const clockPlayer=new Player(clockCore,()=>{},()=>{},()=>{});
  clockPlayer.active=true;
  assert.equal(clockPlayer.speed,1,'Mini-games default to normal speed');
  for(let speed=1;speed<=10;++speed){
    clockPlayer.setSpeed(speed);slicesRun=0;
    for(const time of [1000,1016,1033,1050,1066,1083,1100])clockPlayer.tick(time);
    assert.equal(slicesRun,10*speed,`${speed}x must advance the requested amount of guest time`);
  }
  for(const speed of [0.25,0.5]){
    clockPlayer.setSpeed(speed);slicesRun=0;
    for(let time=1000;time<=1400;time+=10)clockPlayer.tick(time);
    assert.equal(slicesRun,40*speed,`${speed}x must preserve fractional guest time across frames`);
  }
  clockPlayer.setSpeed(10);
  clockPlayer.tick(2000,true);slicesRun=0;
  clockPlayer.tick(3000);clockPlayer.tick(3010);
  assert.equal(slicesRun,10,'Resuming a hidden page must not catch up hidden time');
  clockPlayer.setSpeed(1);slicesRun=0;
  clockPlayer.tick(4000);clockPlayer.tick(4010);
  assert.equal(slicesRun,1,'Returning to normal speed must discard accelerated timing');
  for(const speed of [0,0.1,0.75,11,1.5,NaN,Infinity])assert.throws(()=>clockPlayer.setSpeed(speed),/Choose 0.25, 0.5/);
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
