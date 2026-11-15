// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
import assert from 'node:assert/strict';
import vm from 'node:vm';
import {webcrypto} from 'node:crypto';
import {readAsset} from './embedded_assets.mjs';
import {engine,inputProgram} from './mini_game.mjs';

function setup(){
 const values=new Map(),storage={setItem:(k,v)=>values.set(k,v),getItem:k=>values.get(k),removeItem:k=>values.delete(k)};
 const server={owner:null,operations:new Map(),requests:0,writes:0,paused:false,failBefore:false,loseReply:false,disk:false,conflict:false,card:null};
 const fetch=async(path,options={})=>{
  ++server.requests;
  if(server.failBefore){server.failBefore=false;throw new Error('offline');}
  const headers=options.headers||{},lease=headers['X-VMU-Lease'],operation=headers['X-VMU-Operation'];
  let status;
  if(server.conflict)status=409;
  else if(path.startsWith('checkout/')){
   status=server.owner&&server.owner!==lease?409:200;if(status===200)server.owner=lease;
  }else{
   assert.equal(path,'sync/0');assert.equal(server.owner,lease);
   let entry=server.operations.get(operation);
   if(!entry&&options.method==='POST'){
    entry={data:new Uint8Array(options.body),returning:headers['X-VMU-Return']==='1',status:202};server.operations.set(operation,entry);
   }
   if(entry?.status===202&&!server.paused){
    entry.status=server.disk?500:200;
    if(entry.status===200){server.card=entry.data;++server.writes;}
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
 return {sync,Sync,core,storage,server,messages,lease:Sync.token()};
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

const checkout=setup();checkout.server.loseReply=true;
await assert.rejects(checkout.sync.checkout('0','"1"',checkout.lease));
const reloaded=new checkout.Sync(checkout.core,()=>{},checkout.storage,'test');
assert.ok(reloaded.restore());assert.equal(await reloaded.checkpoint(true),true,'Recover ownership after losing the checkout reply');

const local=setup();assert.equal(await local.sync.checkpoint(),true);assert.equal(local.server.requests,0,'Local files must never be uploaded');
console.log('Phone progress sync, VMU execution, retries, reload recovery, disk failures, and conflict checks passed');
