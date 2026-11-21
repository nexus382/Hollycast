// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
import assert from 'node:assert/strict';
import vm from 'node:vm';
import {webcrypto} from 'node:crypto';
import {readAsset} from './embedded_assets.mjs';
import {engine,inputProgram} from './mini_game.mjs';

function setup(){
 const values=new Map(),storage={setItem:(k,v)=>values.set(k,v),getItem:k=>values.get(k),removeItem:k=>values.delete(k)};
 const server={owner:null,lease:null,revision:'"1"',operations:new Map(),requests:0,writes:0,uploads:[],rotations:[],paused:false,failBefore:false,loseReply:false,disk:false,conflict:false,card:new Uint8Array(131072).fill(0x55),checkoutPending:false,settleCheckout:null};
 const fetch=async(path,options={})=>{
  ++server.requests;
  if(server.failBefore){server.failBefore=false;throw new Error('offline');}
  const headers=options.headers||{},lease=headers['X-VMU-Lease'],operation=headers['X-VMU-Operation'];
  let status;
  if(server.conflict)status=409;
  else if(path.startsWith('checkout/')){
   const previousLease=headers['X-VMU-Previous-Lease'];
   if(previousLease){
    status=server.lease===previousLease||server.lease===lease?200:409;
    server.rotations.push({previousLease,lease,status});
    if(status===200){
     server.lease=lease;
     if(server.owner)server.owner=lease;
    }
   }else if(server.owner===lease&&!server.checkoutPending)status=200;
   else if(server.owner&&server.owner!==lease)status=409;
   else if(headers['X-VMU-Force']!=='1'&&headers['If-Match']!==server.revision)status=409;
   else{
    if(server.checkoutPending)return new Promise(resolve=>{server.settleCheckout=()=>{server.owner=server.lease=lease;resolve({status:200,ok:true});};});
    server.owner=server.lease=lease;server.operations.clear();status=200;
   }
  }else if(path.startsWith('release/')){
   status=server.owner&&server.owner!==lease?409:200;
   if(status===200&&server.owner===lease){server.owner=server.lease=null;server.operations.clear();}
  }else if(server.lease!==lease)status=409;
  else{
   assert.equal(path,'sync/0');
   let entry=server.operations.get(operation);
   if(!entry&&options.method==='POST'){
    if(!server.owner)return {status:409,ok:false};
    entry={data:new Uint8Array(options.body),returning:headers['X-VMU-Return']==='1',status:202};server.operations.set(operation,entry);
    server.uploads.push({returning:entry.returning,data:new Uint8Array(entry.data)});
   }
   if(entry?.status===202&&!server.paused){
    entry.status=server.disk?500:200;
    if(entry.status===200){server.card=entry.data;++server.writes;if(entry.returning)server.owner=null;}
   }
   status=entry?.status||404;
  }
  if(server.loseReply){server.loseReply=false;throw new Error('response lost after processing');}
  return {status,ok:status>=200&&status<300};
 };
 const context=vm.createContext({Uint8Array,crypto:webcrypto,btoa,atob,AbortController,fetch,
  setTimeout(cb,delay){if(delay===100)queueMicrotask(cb);return 1;},clearTimeout(){}});
 const {core}=engine(context);core.load(inputProgram(),'INPUT.VMS');core.reset(core.dateNow());
 vm.runInContext(readAsset('../../core/network/phone_vmu_sync.h','VMUSYNC'),context);
 const Sync=vm.runInContext('PhoneVmuSync',context),messages=[];
 const sync=new Sync(core,message=>messages.push(message),storage,'test');
 function fork(record=storage.getItem('test')){
  const values=new Map([['test',record]]);
  const browserStorage={setItem:(k,v)=>values.set(k,v),getItem:k=>values.get(k),removeItem:k=>values.delete(k)};
  const vmCore={flash:new Uint8Array(131072)};
  const tab=new Sync(vmCore,()=>{},browserStorage,'test');
  const bytes=tab.restore();assert.ok(bytes);vmCore.flash.set(bytes);
  return {tab,flash:vmCore.flash,storage:browserStorage};
 }
 return {sync,Sync,core,storage,server,messages,fork,lease:Sync.token()};
}

const normal=setup();
await normal.sync.checkout('0','"1"',normal.lease);
normal.core.run(3);assert.equal(normal.core.flash[0x3000],255,'Execute the real VMU CPU before syncing its flash');
assert.equal(await normal.sync.checkpoint(),true);assert.equal(normal.server.card[0x3000],255);
const requests=normal.server.requests;await normal.sync.checkpoint();assert.equal(normal.server.requests,requests,'Unchanged cards must not be uploaded');
normal.core.key(4,true);normal.core.run(3);
assert.equal(await normal.sync.checkpoint(true),true);assert.equal(normal.server.card[0x3000],239);
assert.equal(normal.sync.owner,null);assert.equal(normal.storage.getItem('test'),undefined);

const offline=setup();await offline.sync.checkout('0','"1"',offline.lease);
offline.core.flash[123]=11;offline.server.failBefore=true;
assert.equal(await offline.sync.checkpoint(),false);assert.equal(offline.server.writes,0);
offline.core.flash[123]=22;
assert.equal(await offline.sync.checkpoint(),true);assert.equal(offline.server.card[123],22,'Retry must send progress made after the failed request');

const lost=setup();await lost.sync.checkout('0','"1"',lost.lease);
lost.core.flash[123]=33;lost.server.loseReply=true;
assert.equal(await lost.sync.checkpoint(),false);assert.equal(lost.server.writes,1);
lost.core.flash[123]=44;
await lost.sync.checkpoint();assert.equal(lost.server.writes,2,'Lost replies must not replay an already saved operation');
assert.equal(lost.server.card[123],44);

const paused=setup();await paused.sync.checkout('0','"1"',paused.lease);
paused.server.paused=true;paused.core.flash[123]=55;
assert.equal(await paused.sync.checkpoint(true),false);assert.ok(paused.sync.owner,'A paused emulator must not be reported as saved');
assert.equal(paused.sync.returning,true,'Do not run the VMU CPU while a return is still waiting to commit');
const restored=new paused.Sync(paused.core,()=>{},paused.storage,'test');
const recovered=restored.restore();assert.equal(recovered[123],55);assert.ok(restored.pending.returning);
paused.core.flash.set(recovered);paused.server.paused=false;
assert.equal(await restored.checkpoint(true),true);assert.equal(paused.server.card[123],55);assert.equal(restored.owner,null);
assert.equal(restored.returned,true,'A recovered return must tell the phone page to switch back to the Dreamcast display');

const disk=setup();await disk.sync.checkout('0','"1"',disk.lease);disk.server.disk=true;disk.core.flash[123]=66;
assert.equal(await disk.sync.checkpoint(true),false);assert.ok(disk.sync.owner);assert.equal(disk.server.writes,0);
disk.server.disk=false;assert.equal(await disk.sync.checkpoint(true),true);assert.equal(disk.server.card[123],66);

const conflict=setup();await conflict.sync.checkout('0','"1"',conflict.lease);conflict.server.conflict=true;conflict.core.flash[123]=77;
assert.equal(await conflict.sync.checkpoint(true),false);assert.equal(conflict.server.writes,0);assert.ok(conflict.sync.blocked);
assert.equal(JSON.parse(conflict.storage.getItem('test')).card.length>0,true,'Conflicts must retain a recoverable local card');
const blockedRequests=conflict.server.requests;await conflict.sync.checkpoint();assert.equal(conflict.server.requests,blockedRequests);

const forced=setup();await forced.sync.checkout('0','"1"',forced.lease);
forced.core.flash[123]=78;assert.equal(await forced.sync.checkpoint(),true);
forced.core.flash[124]=1;forced.server.conflict=true;assert.equal(await forced.sync.checkpoint(),false);
forced.core.flash[124]=0;forced.server.conflict=false;
const beforeForce=forced.server.writes;
assert.equal(await forced.sync.forceTakeover(),true);
assert.equal(forced.server.writes,beforeForce+1,'Forced takeover must upload even when phone bytes match the last saved snapshot');
assert.equal(forced.server.card[123],78);assert.equal(forced.server.card[124],0);

const serialized=setup();await serialized.sync.checkout('0','"1"',serialized.lease);
serialized.core.flash[123]=79;assert.equal(await serialized.sync.checkpoint(),true);
serialized.core.flash[124]=1;serialized.server.conflict=true;assert.equal(await serialized.sync.checkpoint(),false);
serialized.core.flash[124]=0;serialized.server.conflict=false;serialized.server.checkoutPending=true;
const startRequests=serialized.server.requests,startWrites=serialized.server.writes;
const forcePending=serialized.sync.forceTakeover();
await new Promise(resolve=>setImmediate(resolve));
assert.ok(serialized.server.settleCheckout,'Force claim should remain the active conflict operation');
const periodic=serialized.sync.checkpoint(),returning=serialized.sync.checkpoint(true);
assert.equal(serialized.server.requests,startRequests+1,'Periodic and return checkpoints must wait behind force claim');
serialized.server.checkoutPending=false;serialized.server.settleCheckout();
assert.equal(await forcePending,true);assert.equal(await periodic,true);assert.equal(await returning,true);
assert.equal(serialized.server.writes,startWrites+2,'Return during force must perform a separate persisted return upload');
assert.equal(serialized.server.uploads.at(-2).returning,false);
assert.equal(serialized.server.uploads.at(-1).returning,true,'Return requested during takeover must use a returning upload');
assert.equal(serialized.sync.owner,null,'The serialized return must release phone ownership');
assert.equal(serialized.server.owner,null,'The mock desktop must regain ownership after return');

const discarded=setup();await discarded.sync.checkout('0','"1"',discarded.lease);
discarded.server.paused=true;discarded.core.flash[123]=80;
assert.equal(await discarded.sync.checkpoint(),false,'Pending phone progress must remain unsaved before discard');
const desktopBeforeDiscard=new Uint8Array(discarded.server.card),writesBeforeDiscard=discarded.server.writes;
discarded.server.owner='other-lease';
await assert.rejects(discarded.sync.discard(),/another phone/,'A different phone lease must prevent discard from clearing local recovery data');
assert.ok(discarded.sync.owner);assert.ok(discarded.storage.getItem('test'));
discarded.server.owner=discarded.lease;
assert.equal(await discarded.sync.discard(),true);
assert.equal(discarded.server.owner,null,'Discard must release the desktop lease');
assert.equal(discarded.server.writes,writesBeforeDiscard,'Discard must not upload the phone copy');
assert.deepEqual(discarded.server.card,desktopBeforeDiscard,'Discard must preserve the desktop card');
assert.equal(discarded.sync.owner,null);assert.equal(discarded.storage.getItem('test'),undefined);

const checkout=setup();checkout.server.loseReply=true;
await assert.rejects(checkout.sync.checkout('0','"1"',checkout.lease));
const reloaded=new checkout.Sync(checkout.core,()=>{},checkout.storage,'test');
checkout.core.flash.set(reloaded.restore());
assert.equal(await reloaded.checkpoint(true),true,'Recover ownership after losing the checkout reply');
assert.equal(checkout.server.rotations.length,1);
assert.equal(checkout.server.rotations[0].previousLease,checkout.lease);
assert.notEqual(checkout.server.rotations[0].lease,checkout.lease);

const copied=setup();await copied.sync.checkout('0','"1"',copied.lease);
const winner=copied.fork(),staleTab=copied.fork(),staleDiscard=copied.fork();
winner.flash[123]=81;
assert.equal(await winner.tab.checkpoint(),true);
assert.notEqual(copied.server.owner,copied.lease,'A recovered document must use a fresh lease');
copied.core.flash[123]=82;
assert.equal(await copied.sync.checkpoint(),false,'The original tab must lose write access after rotation');
staleTab.flash[123]=83;
assert.equal(await staleTab.tab.checkpoint(),false,'A copied tab cannot rotate a lease already replaced by another document');
await assert.rejects(staleTab.tab.forceTakeover(),/already in use/);
await assert.rejects(staleDiscard.tab.discard(),/in use/);
assert.ok(staleDiscard.storage.getItem('test'),'Discard must preserve local recovery if another document owns the card');
assert.equal(copied.server.card[123],81);
assert.equal(copied.server.writes,1);

const restoredForce=setup();await restoredForce.sync.checkout('0','"1"',restoredForce.lease);
const freeCard=restoredForce.fork();freeCard.flash[123]=84;
restoredForce.server.owner=restoredForce.server.lease=null;restoredForce.server.revision='"2"';
assert.equal(await freeCard.tab.checkpoint(),false);
assert.equal(await freeCard.tab.forceTakeover(),true,'Explicit Keep my phone copy may claim a free card after sharing restarts');
assert.equal(restoredForce.server.card[123],84);

const restoredDiscard=setup();await restoredDiscard.sync.checkout('0','"1"',restoredDiscard.lease);
const freeDesktop=restoredDiscard.fork();
restoredDiscard.server.owner=restoredDiscard.server.lease=null;restoredDiscard.server.revision='"2"';
assert.equal(await freeDesktop.tab.discard(),true,'Use Hollycast copy must still work when the slot is already free');
assert.equal(freeDesktop.storage.getItem('test'),undefined);

const rotation=setup();await rotation.sync.checkout('0','"1"',rotation.lease);
const rotating=rotation.fork();rotating.flash[123]=85;rotation.server.loseReply=true;
assert.equal(await rotating.tab.checkpoint(),false);
const firstRotation=rotation.server.rotations[0],pendingRotation=JSON.parse(rotating.storage.getItem('test')).owner;
assert.equal(pendingRotation.previousLease,rotation.lease);
assert.equal(pendingRotation.lease,firstRotation.lease);
assert.equal(pendingRotation.ready,false);
const resumed=rotation.fork(rotating.storage.getItem('test'));
assert.equal(resumed.flash[123],85,'The in-flight rotation must retain the recovered card bytes');
assert.equal(await resumed.tab.checkpoint(),true);
assert.deepEqual(rotation.server.rotations[1],firstRotation,'Reload must retry the same old/new lease pair');
assert.equal(rotation.server.rotations[2].previousLease,firstRotation.lease);
assert.notEqual(rotation.server.rotations[2].lease,firstRotation.lease,'Reload must then establish its own lease');
assert.equal(rotation.server.card[123],85);
assert.equal(rotation.server.writes,1);
assert.equal(JSON.parse(resumed.storage.getItem('test')).owner.previousLease,undefined);
assert.equal(await rotating.tab.checkpoint(),false,'The interrupted document cannot reclaim the completed rotation');

const lostReturn=setup();await lostReturn.sync.checkout('0','"1"',lostReturn.lease);
lostReturn.core.flash[123]=86;lostReturn.server.loseReply=true;
assert.equal(await lostReturn.sync.checkpoint(true),false);
assert.equal(lostReturn.server.owner,null,'The desktop already reconnected the card before its reply was lost');
const returningTab=lostReturn.fork();
assert.equal(await returningTab.tab.checkpoint(),true);
assert.equal(returningTab.tab.returned,true);
assert.equal(lostReturn.server.writes,1,'Rotating a completed return must preserve its idempotent result');
assert.equal(lostReturn.server.card[123],86);

const local=setup();assert.equal(await local.sync.checkpoint(),true);assert.equal(local.server.requests,0,'Local files must never be uploaded');
console.log('Phone progress sync, VMU execution, retries, reload recovery, disk failures, and conflict checks passed');
