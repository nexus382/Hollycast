// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

const source=readFileSync(new URL('../../core/network/phone_vmu_page.h',import.meta.url),'utf8');
const script=source.match(/<script>([\s\S]*?)<\/script>/)[1];
const media=readFileSync(new URL('../../core/deps/nosleep/media.h',import.meta.url),'utf8').split('R"WAKEJS(')[1].split(')WAKEJS"')[0];
const flush=()=>new Promise(resolve=>setImmediate(resolve));
function element(){
 return {callbacks:{},classList:{add(){},remove(){},toggle(){}},dataset:{},
  addEventListener(name,cb){this.callbacks[name]=cb;},setAttribute(name,value){this[name]=value;},setPointerCapture(){}};
}
function page({native=true,blockVideo=false}={}){
 const elements=Object.fromEntries([...source.matchAll(/id="([^"]+)"/g)].map(match=>[match[1],element()]));
 const hardware=Array.from({length:8},(_,key)=>Object.assign(element(),{dataset:{key:String(key)}}));
 const requests=[],videos=[];
 elements.lcd.getContext=()=>({fillRect(){}});
 elements.slot.value='0';elements.slot.selectedOptions=[{text:'A1'}];
 const document={hidden:false,documentElement:{},body:element(),callbacks:{},
  addEventListener(name,cb){this.callbacks[name]=cb;},getElementById:id=>elements[id],querySelectorAll:()=>hardware,
  createElement(tag){
   if(tag!=='video'){assert.equal(tag,'source');return element();}
   const video=Object.assign(element(),{paused:true,children:[],plays:0,pauses:0,duration:1,
    appendChild(child){this.children.push(child);},
    play(){++this.plays;if(blockVideo)return Promise.reject(new Error('Playback blocked'));this.paused=false;return Promise.resolve();},
    pause(){++this.pauses;this.paused=true;this.callbacks.pause?.();}});
   videos.push(video);return video;
  }};
 const window={callbacks:{},addEventListener(name,cb){this.callbacks[name]=cb;}};
 const navigator=native?{wakeLock:{request(type){assert.equal(type,'screen');return new Promise((resolve,reject)=>requests.push({resolve,reject}));}}}:{};
 class Player{
  constructor(){this.active=false;this.sleeping=false;}
  releaseAll(){} key(){} tick(){} stop(){this.active=false;this.sleeping=false;}
  load(){this.active=true;this.sleeping=false;} reset(){this.sleeping=false;} sleep(){this.sleeping=!this.sleeping;}
 }
 const context=vm.createContext({document,window,navigator,PhoneVmuPlayer:Player,VMS:{},Uint8Array,AbortController,
  requestAnimationFrame(){},setTimeout(){},clearTimeout(){},fetch:()=>new Promise(()=>{})});
 vm.runInContext(media,context);vm.runInContext(script,context);
 return {elements,hardware,document,window,requests,videos,allowVideo(){blockVideo=false;},
  tap(){document.callbacks.pointerdown();},
  visible(visible){document.hidden=!visible;document.callbacks.visibilitychange();},
  status(){return elements['awake-status'].textContent;}};
}
function lock(){
 return {callbacks:{},releases:0,addEventListener(name,cb){this.callbacks[name]=cb;},
  release(){++this.releases;this.callbacks.release?.();return Promise.resolve();}};
}

const native=page(),first=lock();
assert.equal(native.requests.length,1,'Native wake lock should be requested on opening the page');
native.tap();assert.equal(native.requests.length,1,'Touches must not duplicate a pending request');
native.requests[0].resolve(first);await flush();
assert.match(native.status(),/Keeping this screen awake/);
native.tap();assert.equal(native.requests.length,1,'Touches must not duplicate an active lock');
native.visible(false);assert.equal(first.releases,1,'Backgrounding must release the lock');
native.visible(true);assert.equal(native.requests.length,2,'Returning should reacquire the lock');
const second=lock();native.requests[1].resolve(second);await flush();
native.elements.awake.callbacks.click();assert.equal(second.releases,1,'Opting out must release the lock');
assert.equal(native.elements.awake['aria-pressed'],'false');
native.visible(false);native.visible(true);native.tap();
assert.equal(native.requests.length,2,'Opt-out must survive visibility changes and touches');
native.elements.awake.callbacks.click();
native.requests[2].reject(new Error('Low battery'));await flush();
assert.match(native.status(),/unavailable/,'Denied native requests must show a useful status');
assert.equal(native.videos.length,0,'Do not override a native refusal with the video workaround');
native.tap();const third=lock();native.requests[3].resolve(third);await flush();
third.callbacks.release();assert.match(native.status(),/released/);
native.tap();const fourth=lock();native.requests[4].resolve(fourth);await flush();
native.window.callbacks.pagehide();assert.equal(fourth.releases,1,'Navigation must release even before visibility changes');
native.window.callbacks.pageshow();assert.equal(native.requests.length,6,'History restoration must reacquire');
native.requests[5].resolve(lock());await flush();

const race=page(),stale=lock();
race.visible(false);race.visible(true);race.requests[0].resolve(stale);await flush();
assert.equal(stale.releases,1,'A request completed after hiding must not retain a stale lock');
assert.equal(race.requests.length,2,'Pending acquisition must recover when the page returns');
race.requests[1].resolve(lock());await flush();assert.match(race.status(),/Keeping/);
const disabled=page(),disabledLock=lock();
disabled.elements.awake.callbacks.click();disabled.requests[0].resolve(disabledLock);await flush();
assert.equal(disabledLock.releases,1,'Opting out during acquisition must release its eventual lock');
assert.equal(disabled.requests.length,1);

const fallback=page({native:false});
assert.equal(fallback.videos.length,0,'HTTP fallback must wait for a user gesture');
fallback.tap();await flush();const video=fallback.videos[0];
assert.match(fallback.status(),/Keeping/);assert.equal(video.playsinline,'');
assert.equal(video.children.length,2);assert.match(video.children[0].src,/^data:video\/webm;base64,/);
assert.match(video.children[1].src,/^data:video\/mp4;base64,/);
video.callbacks.loadedmetadata();assert.equal(video.loop,true);
fallback.tap();assert.equal(video.plays,1,'Do not replay an already playing video on every touch');
fallback.visible(false);assert.equal(video.paused,true);
fallback.visible(true);await flush();assert.equal(video.plays,2);
const sleep=fallback.hardware[7],event={pointerId:1,preventDefault(){}};
sleep.callbacks.pointerdown(event);sleep.callbacks.pointerup(event);
assert.equal(video.paused,true,'VMU Sleep should allow the phone to sleep too');
sleep.callbacks.pointerdown(event);sleep.callbacks.pointerup(event);await flush();
assert.equal(video.paused,false,'Waking the VMU should restore keep-awake');
fallback.elements['game-file'].files=[{name:'TEST.VMS',size:1,arrayBuffer:async()=>new ArrayBuffer(1)}];
await fallback.elements['game-file'].callbacks.change({target:fallback.elements['game-file']});
sleep.callbacks.pointerdown(event);sleep.callbacks.pointerup(event);assert.equal(video.paused,true);
fallback.elements.mirror.callbacks.click();await flush();
assert.equal(video.paused,false,'Returning from a sleeping mini-game should restore live keep-awake');
fallback.elements.awake.callbacks.click();assert.equal(video.paused,true);
fallback.visible(false);fallback.visible(true);fallback.tap();await flush();assert.equal(video.paused,true);
fallback.elements.awake.callbacks.click();await flush();assert.equal(video.paused,false);
fallback.window.callbacks.pagehide();assert.equal(video.paused,true);

const blocked=page({native:false,blockVideo:true});blocked.tap();await flush();
assert.match(blocked.status(),/unavailable/);
blocked.allowVideo();blocked.tap();await flush();assert.match(blocked.status(),/Keeping/);
assert.equal(blocked.videos.length,1,'Retry should reuse the same video');
console.log('Phone wake-lock acquisition, opt-out, visibility, navigation, races, VMU sleep, and HTTP fallback checks passed');
