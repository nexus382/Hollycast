// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
namespace phone_vmu {
inline constexpr char cardSync[] = R"VMUSYNC(
'use strict';
// Each checkpoint has an idempotency token. A lost HTTP reply must never mark
// unsaved bytes as saved, or replay an old card over newer progress.
class PhoneVmuSync {
 constructor(core,report,storage,key){
  this.core=core;this.report=report;this.storage=storage;this.key=key;
  this.owner=null;this.pending=null;this.saved=null;this.inFlight=null;this.returning=false;this.returned=false;this.blocked=false;
 }
 static token(){return Array.from(crypto.getRandomValues(new Uint8Array(16)),b=>b.toString(16).padStart(2,'0')).join('');}
 static encode(bytes){let text='';for(const b of bytes)text+=String.fromCharCode(b);return btoa(text);}
 static decode(text){return Uint8Array.from(atob(text),c=>c.charCodeAt(0));}
 remember(){
  if(!this.owner)return;
  try{
   this.storage?.setItem(this.key,JSON.stringify({owner:this.owner,card:PhoneVmuSync.encode(this.core.flash),
    pending:this.pending?{...this.pending,data:PhoneVmuSync.encode(this.pending.data)}:null}));
  }catch(_){this.report('Browser storage unavailable. Keep this tab open until progress is synced.');}
 }
 restore(){
  try{
   const saved=JSON.parse(this.storage?.getItem(this.key)||'null');if(!saved)return null;
   const bytes=PhoneVmuSync.decode(saved.card);
   if(bytes.length!==131072||!saved.owner||!/^[0-7]$/.test(saved.owner.slot)||!/^[0-9a-f]{32}$/.test(saved.owner.lease)||typeof saved.owner.revision!=='string'||!Number.isSafeInteger(saved.owner.sequence)||saved.owner.sequence<0)return null;
   if(saved.pending){
    const data=PhoneVmuSync.decode(saved.pending.data);
    if(data.length!==131072||!/^[0-9a-f]{32}$/.test(saved.pending.operation))return null;
    this.pending={...saved.pending,data};
    this.returning=!!this.pending.returning;
   }
   this.owner=saved.owner;return bytes;
  }catch(_){return null;}
 }
 async request(path,options={}){
  const controller=new AbortController(),timeout=setTimeout(()=>controller.abort(),5000);
  try{return await fetch(path,{...options,cache:'no-store',signal:controller.signal});}
  finally{clearTimeout(timeout);}
 }
 async checkout(slot,revision,lease){
  this.owner={slot,revision,lease,ready:false,sequence:0};this.saved=null;this.pending=null;this.returned=false;this.blocked=false;this.remember();
  try{await this.claim();}catch(error){
   if(error.conflict){this.owner=null;try{this.storage?.removeItem(this.key);}catch(_){}this.report('Automatic sync unavailable. Download this local copy to keep its progress.');}
   throw error;
  }
  this.report('Progress sync is on. The VMU is unplugged from the Dreamcast while you play.');
 }
 async claim(){
  const {slot,revision,lease}=this.owner;
  const response=await this.request('checkout/'+slot,{method:'POST',headers:{'Content-Type':'application/octet-stream','If-Match':revision,'X-VMU-Lease':lease},body:new Uint8Array([0])});
  if(!response.ok){const error=new Error(response.status===409?'This VMU changed or is already in use. Try again after the Dreamcast finishes saving. If its phone session was lost, turn Connect Phone off and on in Hollycast, then scan the new QR code.':'This VMU cannot sync. Use a writable card outside netplay.');error.conflict=response.status===403||response.status===409;throw error;}
  this.owner.ready=true;this.remember();
 }
 checkpoint(returning=false){
  if(!this.owner)return Promise.resolve(true);
  if(this.inFlight)return returning?this.inFlight.then(ok=>ok?this.checkpoint(true):false):this.inFlight;
  if(this.blocked)return Promise.resolve(false);
  this.returning=returning||!!this.pending?.returning;
  this.inFlight=this.transfer(returning).finally(()=>{this.inFlight=null;this.returning=!!this.pending?.returning;});
  return this.inFlight;
 }
 async transfer(returning){
  try{
   if(!this.owner.ready)await this.claim();
   // Finish the previous operation before creating another. Its result may
   // have reached disk even when the phone lost the response or reloaded.
   if(this.pending){this.remember();await this.sendPending();}
   if(!this.owner)return true;
   const data=new Uint8Array(this.core.flash);
   if(!returning&&this.saved&&data.every((b,i)=>b===this.saved[i]))return true;
   this.pending={operation:(++this.owner.sequence).toString(16).padStart(32,'0'),data,returning};this.remember();
   await this.sendPending();return true;
  }catch(error){
   if(error.conflict)this.blocked=true;
   this.report(error.conflict?'Sync stopped because the desktop card changed or sharing ended. Your phone copy is preserved; download it to keep it.':
    error.disk?'Hollycast could not save the VMU. Your phone copy is preserved. Check disk access, then retry returning.':
    'Progress is waiting to sync. Keep Hollycast running and resume the Dreamcast if paused. Retrying automatically.');
   return false;
  }
 }
 async sendPending(){
  const operation=this.pending,owner=this.owner;
  const headers={'Content-Type':'application/octet-stream','X-VMU-Lease':owner.lease,'X-VMU-Operation':operation.operation,'X-VMU-Return':operation.returning?'1':'0'};
  let response=await this.request('sync/'+owner.slot,{method:'POST',headers,body:operation.data});
  for(let attempt=0;response.status===202&&attempt<20;++attempt){
   await new Promise(resolve=>setTimeout(resolve,100));
   response=await this.request('sync/'+owner.slot,{headers});
  }
  if(response.status!==200){
   const error=new Error('Sync pending');error.conflict=response.status===409||response.status===403||response.status===404;error.disk=response.status===500;
   // A failed disk write is acknowledged as failed, so retry with a fresh
   // operation while retaining the phone's complete current card.
   if(error.disk)this.pending=null;
   throw error;
  }
  this.saved=operation.data;this.pending=null;
  if(operation.returning){
   this.owner=null;this.saved=null;this.returned=true;
   try{this.storage?.removeItem(this.key);}catch(_){}
   this.report('Progress saved to Hollycast. VMU reconnected.');
  }else{
   this.remember();this.report('Progress saved to Hollycast. Choose Return to Dreamcast when you finish.');
  }
 }
}
)VMUSYNC";
}
