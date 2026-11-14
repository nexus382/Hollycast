// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
import assert from 'node:assert/strict';
import vm from 'node:vm';
import {webcrypto} from 'node:crypto';
import {engine,inputProgram} from './mini_game.mjs';
import {readAsset} from './embedded_assets.mjs';
const source=readAsset('../../core/network/phone_vmu_page.h','HTML');
const script=source.match(/<script>([\s\S]*?)<\/script>/)[1];
function classList() {
 const values=new Set();
 return {values,add:name=>values.add(name),remove:name=>values.delete(name),toggle(name,on){on=on??!values.has(name);if(on)values.add(name);else values.delete(name);return on;}};
}
function element() {return {callbacks:{},classList:classList(),addEventListener(event,cb){this.callbacks[event]=cb;},setAttribute(name,value){this[name]=value;},setPointerCapture(){}};}
const ids=Array.from(source.matchAll(/id="([^"]+)"/g),match=>match[1]);
const elements=Object.fromEntries(ids.map(id=>[id,element()]));
const hardware=Array.from({length:8},(_,index)=>Object.assign(element(),{dataset:{key:String(index)}}));
let drawn,blanks=0,nextPoll,hideOverlay,pending,requests=0,fail=false,animate,syncMode=false,remoteFlash,checkoutPending=false,settleCheckout;
const ctx={createImageData:(w,h)=>({data:new Uint8ClampedArray(w*h*4)}),fillRect(){++blanks;},putImageData(image){drawn=[...image.data];}};
elements.lcd.getContext=()=>ctx;
elements.slot.value='0'; elements.slot.selectedOptions=[{text:'A1'}];
const document={hidden:false,documentElement:{},callbacks:{},addEventListener(event,cb){this.callbacks[event]=cb;},body:{classList:classList()},getElementById:id=>elements[id],querySelectorAll:()=>hardware};
const window={callbacks:{},addEventListener(event,cb){this.callbacks[event]=cb;}};
const context=vm.createContext({document,window,navigator:{},crypto:webcrypto,btoa,atob,Uint8Array,AbortController,requestAnimationFrame:cb=>{animate=cb;},
 setTimeout(cb,delay){if(delay===100)queueMicrotask(cb);else if(delay===4000)hideOverlay=cb;else if(delay!==2500&&delay!==5000)nextPoll=cb;return 1;},clearTimeout(){},
 fetch(path,options={}){
  ++requests;
  if(!path.startsWith('frame/')){
   assert.ok(syncMode,'Local games must not send card requests');
   if(path.startsWith('card/'))return Promise.resolve({ok:true,headers:{get:()=> '"1"'},arrayBuffer:async()=>new Uint8Array(core.flash).buffer});
   assert.ok(options.headers['X-VMU-Lease']);
   if(path.startsWith('checkout/')&&checkoutPending)return new Promise((resolve,reject)=>{settleCheckout={resolve,reject};});
   if(path.startsWith('sync/')&&options.method==='POST')remoteFlash=new Uint8Array(options.body);
   return Promise.resolve({ok:true,status:path.startsWith('sync/')&&options.method==='POST'?202:200});
  }
  assert.equal(path,'frame/'+elements.slot.value);if(fail)return Promise.reject(new Error('offline'));return new Promise(resolve=>{pending=bytes=>resolve({ok:true,arrayBuffer:async()=>Uint8Array.from(bytes).buffer});});
 }});
const {core}=engine(context);
vm.runInContext(readAsset('../../core/network/phone_vmu_sync.h','VMUSYNC'),context);
vm.runInContext(script,context);
const flush=()=>new Promise(resolve=>setImmediate(resolve));
const frame=new Array(193).fill(0);frame[0]=1;frame[1]=1;frame[192]=128;
pending(frame);await flush();
assert.equal(elements.status.textContent,'VMU A1 connected');
assert.deepEqual(drawn.slice(0,8),[35,57,46,255,158,179,165,255]);
hideOverlay();assert.ok(elements.controls.hidden);elements.lcd.callbacks.pointerdown();assert.ok(!elements.controls.hidden);
elements.layout.callbacks.click();assert.ok(document.body.classList.values.has('lcd-only'));
elements.hide.callbacks.click();assert.ok(elements.controls.hidden);
elements.rotate.callbacks.click();assert.equal(elements.rotate['aria-pressed'],'true');
nextPoll();elements.slot.value='7';elements.slot.selectedOptions=[{text:'D2'}];elements.slot.callbacks.change();
const before=elements.status.textContent;pending(frame);await flush();assert.equal(elements.status.textContent,before);
nextPoll();pending(new Array(193).fill(0));await flush();assert.match(elements.status.textContent,/Waiting for VMU D2/);
fail=true;nextPoll();await flush();assert.match(elements.status.textContent,/Connection lost/);
fail=false;nextPoll();pending(frame);await flush();assert.equal(elements.status.textContent,'VMU D2 connected');
const count=requests;document.hidden=true;nextPoll();await flush();assert.equal(requests,count);document.hidden=false;
// Load via the actual file input handler, then drive real on-screen button handlers.
const program=inputProgram();
elements['game-file'].files=[{name:'INPUT.VMS',size:program.length,arrayBuffer:async()=>program.buffer}];
await elements['game-file'].callbacks.change({target:elements['game-file']});
assert.equal(elements.slot.disabled,true);assert.equal(elements.download.hidden,false);
const event=id=>({pointerId:id,preventDefault(){}});
hardware[4].callbacks.pointerdown(event(10));hardware[5].callbacks.pointerdown(event(11));
animate(1);animate(31);assert.equal(core.flash[0x3000],207,'Two fingers must hold A+B');
hardware[4].callbacks.pointercancel(event(10));hardware[5].callbacks.lostpointercapture(event(11));
animate(61);assert.equal(core.flash[0x3000],255,'Cancelled/lost touches must release input');
const keyboard={key:'a',code:'KeyA',target:{closest:()=>null},preventDefault(){}};
document.callbacks.keydown(keyboard);hardware[4].callbacks.pointerdown(event(12));hardware[4].callbacks.pointerup(event(12));
animate(91);assert.equal(core.flash[0x3000],239,'Releasing a touch must preserve a held keyboard key');
window.callbacks.blur();animate(121);assert.equal(core.flash[0x3000],255,'Lost focus must release every input');
hardware[7].callbacks.pointerdown(event(13));hardware[7].callbacks.pointerup(event(13));
assert.match(elements.status.textContent,/Sleeping/);
hardware[7].callbacks.pointerdown(event(14));hardware[7].callbacks.pointerup(event(14));
assert.match(elements.status.textContent,/resumed/);
await elements.mirror.callbacks.click();assert.equal(elements.slot.disabled,false);assert.equal(elements.download.hidden,true);
syncMode=true;core.flash[0x3000]=255;checkoutPending=true;settleCheckout=null;
const opening=elements.play.callbacks.click();
await flush();assert.ok(settleCheckout,'Card checkout must remain pending while the mini-game is loaded');
assert.equal(elements.mirror.disabled,true,'Return must wait for checkout to finish');
const pendingRequests=requests;await elements.mirror.callbacks.click();
assert.equal(requests,pendingRequests,'Return during checkout must not start a second claim');
hardware[4].callbacks.pointerdown(event(16));hardware[4].callbacks.pointerup(event(16));
animate(200);animate(230);assert.equal(core.flash[0x3000],255,'VMU CPU and input must wait for checkout');
checkoutPending=false;settleCheckout.resolve({ok:true,status:200});await opening;
assert.equal(elements.mirror.disabled,false);
animate(260);animate(290);assert.equal(core.flash[0x3000],255,'Input attempted during checkout must not play later');
assert.equal(elements['game-file'].disabled,true,'An owned card must not be replaced by a local file');
document.callbacks.keydown(keyboard);animate(320);animate(350);
hardware[7].callbacks.pointerdown(event(15));hardware[7].callbacks.pointerup(event(15));
await flush();await flush();
assert.equal(remoteFlash[0x3000],239,'Sleeping must checkpoint progress made by the real VMU CPU');
assert.equal(elements.slot.disabled,true,'Autosave must leave the VMU owned by the phone');
await elements.mirror.callbacks.click();
assert.equal(elements.slot.disabled,false);assert.equal(elements['game-file'].disabled,false);
assert.match(elements['sync-status'].textContent,/saved.*reconnected/);
checkoutPending=true;settleCheckout=null;const failedOpen=elements.play.callbacks.click();
await flush();assert.ok(settleCheckout);
checkoutPending=false;settleCheckout.resolve({ok:false,status:409});await failedOpen;
assert.equal(vm.runInContext('player.active',context),false,'A failed checkout must close the unowned mini-game');
assert.equal(vm.runInContext('cardSync.owner',context),null);
assert.equal(elements.mirror.hidden,true);
assert.match(elements['sync-status'].textContent,/did not start/);
assert.match(elements['sync-status'].textContent,/Connect Phone off and on/,'A lost checkout session must explain the safe desktop reset');
const afterConflictRequests=requests;await elements.mirror.callbacks.click();
assert.equal(requests,afterConflictRequests,'Return after failed checkout must not claim a save');
assert.doesNotMatch(elements['sync-status'].textContent,/saved.*reconnected/);
checkoutPending=true;settleCheckout=null;core.flash[0x3000]=255;
const retryOpen=elements.play.callbacks.click();await flush();assert.ok(settleCheckout);
checkoutPending=false;settleCheckout.reject(new Error('offline'));await retryOpen;
assert.equal(vm.runInContext('cardSync.owner.ready',context),false,'Transient checkout failures retain ownership for retry');
hardware[4].callbacks.pointerdown(event(17));hardware[4].callbacks.pointerup(event(17));
animate(380);animate(410);assert.equal(core.flash[0x3000],255,'A pending retry must keep the VMU paused');
nextPoll();pending(frame);await flush();await flush();
assert.equal(vm.runInContext('cardSync.owner.ready',context),true,'Periodic sync should retry checkout');
assert.equal(elements.mirror.disabled,false,'Return should become available after ownership recovers');
animate(440);animate(470);assert.equal(core.flash[0x3000],255,'Blocked input must not replay after retry');
await elements.mirror.callbacks.click();assert.equal(vm.runInContext('player.active',context),false);
assert.ok(blanks>=3);
console.log('Phone browser rendering, overlay, multi-touch, cancellation, and mini-game checks passed');
