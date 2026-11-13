/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#pragma once
// The phone web client, served by the Remote Play server at GET /.
// PIN login, WebRTC with MJPEG fallback, touch and Bluetooth gamepads.
namespace remoteplay {

inline const char *remotePlayPage()
{
	return R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no, viewport-fit=cover">
<meta name="theme-color" content="#101214">
<title>Hollycast Remote Play</title>
<style>
  :root { --bg:#101214; --panel:#1b1e21; --line:#33383d; --text:#f2f2ef; --muted:#a7adb2; --accent:#e8562a; }
  * { box-sizing:border-box; -webkit-tap-highlight-color:transparent; }
  html, body { margin:0; padding:0; background:var(--bg); color:var(--text); height:100%; overflow:hidden;
    font-family: ui-rounded, "SF Pro Rounded", system-ui, -apple-system, "Segoe UI", sans-serif; touch-action:none; }
  button { font:inherit; color:inherit; }
  #login { min-height:100%; display:flex; align-items:center; justify-content:center; padding:24px; }
  .card { width:min(420px, 100%); background:var(--panel); border:1px solid var(--line); border-radius:18px; padding:28px 24px; }
  .card h1 { font-size:22px; margin:0 0 6px; letter-spacing:.2px; }
  .card p { color:var(--muted); font-size:14px; line-height:1.45; margin:0 0 20px; }
  label { display:block; font-size:13px; font-weight:600; margin:16px 0 8px; }
  input, select { width:100%; background:#121417; border:1px solid var(--line); border-radius:10px; color:var(--text);
    font-size:20px; padding:12px 14px; letter-spacing:4px; }
  select { letter-spacing:0; font-size:16px; }
  #connect { width:100%; margin-top:22px; background:var(--accent); border:0; border-radius:10px; padding:14px;
    font-size:16px; font-weight:700; cursor:pointer; }
  #connect:active { filter:brightness(.85); }
  #loginError { color:#ff8f70; font-size:14px; min-height:20px; margin-top:12px; }
  #game { display:none; position:fixed; inset:0; background:#000; }
  #streamWrap { position:absolute; inset:0; display:flex; align-items:center; justify-content:center; }
  #stream { max-width:100%; max-height:100%; width:100%; height:100%; object-fit:contain; }
  #hud { position:absolute; top:0; left:0; right:0; display:flex; justify-content:space-between; align-items:center;
    padding:calc(8px + env(safe-area-inset-top)) 12px 8px; font-size:13px; color:var(--muted); pointer-events:none; }
  #hud .badge { background:rgba(20,22,24,.82); border:1px solid var(--line); padding:5px 10px; border-radius:999px; }
  #hud .dot { color:#7ee787; }
  #sound { pointer-events:auto; cursor:pointer; font:inherit; color:var(--text); }
  .cluster { position:absolute; bottom:calc(14px + env(safe-area-inset-bottom)); display:flex; flex-direction:column; gap:14px; }
  #leftCluster { left:calc(12px + env(safe-area-inset-left)); align-items:flex-start; }
  #rightCluster { right:calc(12px + env(safe-area-inset-right)); align-items:flex-end; }
  .padbtn { width:58px; height:58px; border-radius:50%; border:1px solid var(--line); background:rgba(27,30,33,.78);
    font-weight:700; font-size:15px; display:flex; align-items:center; justify-content:center; user-select:none; }
  .padbtn.on { background:var(--accent); border-color:var(--accent); }
  .dpad { display:grid; grid-template-columns:repeat(3, 46px); grid-template-rows:repeat(3, 46px); }
  .dpad .padbtn { width:46px; height:46px; border-radius:12px; font-size:13px; }
  .diamond { display:grid; grid-template-columns:repeat(3, 56px); grid-template-rows:repeat(3, 56px); }
  .trigrow { display:flex; gap:10px; }
  .trig { width:64px; height:40px; border-radius:10px; font-size:13px; }
  #stick { width:124px; height:124px; border-radius:50%; border:1px solid var(--line); background:rgba(27,30,33,.6); position:relative; }
  #knob { width:54px; height:54px; border-radius:50%; background:rgba(242,242,239,.9); position:absolute; left:35px; top:35px; }
  #startBtn { position:absolute; left:50%; transform:translateX(-50%); bottom:calc(16px + env(safe-area-inset-bottom));
    width:auto; padding:0 18px; height:38px; border-radius:999px; font-size:12px; letter-spacing:1px; }
  @media (max-height:420px) { .padbtn{width:50px;height:50px} .diamond{grid-template-columns:repeat(3,50px);grid-template-rows:repeat(3,50px)} #stick{width:104px;height:104px} #knob{left:25px;top:25px} }
</style>
</head>
<body>
<div id="login">
  <form class="card" id="loginForm">
    <h1>Hollycast Remote Play</h1>
    <p>Enter the PIN shown on the host computer. Up to four phones can play at once, one per Dreamcast controller port.</p>
    <label for="pin">PIN</label>
    <input id="pin" inputmode="numeric" autocomplete="one-time-code" maxlength="6" placeholder="6-digit PIN" required>
    <label for="player">Controller</label>
    <select id="player">
      <option value="-1">First available</option>
      <option value="0">Player 1</option>
      <option value="1">Player 2</option>
      <option value="2">Player 3</option>
      <option value="3">Player 4</option>
    </select>
    <button id="connect" type="submit">Connect</button>
    <div id="loginError" role="alert"></div>
  </form>
</div>
<div id="game">
  <div id="streamWrap"><img id="stream" alt="Game stream"><video id="rtcVideo" autoplay playsinline muted style="display:none;max-width:100%;max-height:100%;width:100%;height:100%;object-fit:contain"></video></div>
  <div id="hud"><span class="badge"><span class="dot">&#9679;</span> <span id="hudPlayer">Player ?</span></span><button class="badge" id="sound" type="button" hidden>Enable sound</button><span class="badge" id="hudPing">-- ms</span></div>
  <div class="cluster" id="leftCluster">
    <div class="dpad">
      <span></span><div class="padbtn" data-btn="UP">&#9650;</div><span></span>
      <div class="padbtn" data-btn="LEFT">&#9664;</div><span></span><div class="padbtn" data-btn="RIGHT">&#9654;</div>
      <span></span><div class="padbtn" data-btn="DOWN">&#9660;</div><span></span>
    </div>
    <div id="stick"><div id="knob"></div></div>
  </div>
  <div class="cluster" id="rightCluster">
    <div class="trigrow"><div class="padbtn trig" data-trig="LT">L</div><div class="padbtn trig" data-trig="RT">R</div></div>
    <div class="diamond">
      <span></span><div class="padbtn" data-btn="Y">Y</div><span></span>
      <div class="padbtn" data-btn="X">X</div><span></span><div class="padbtn" data-btn="B">B</div>
      <span></span><div class="padbtn" data-btn="A">A</div><span></span>
    </div>
  </div>
  <div class="padbtn" id="startBtn" data-btn="START">START</div>
</div>
<script>
"use strict";
const $ = s => document.querySelector(s);
let ws = null, player = -1, pingStart = 0;
let pingTimer = null, keepaliveTimer = null;

function stopConnectionTimers() {
  clearTimeout(pingTimer);
  clearInterval(keepaliveTimer);
  pingTimer = keepaliveTimer = null;
}

async function login(ev) {
  ev.preventDefault();
  $("#loginError").textContent = "";
  try {
    const res = await fetch("/api/login", { method:"POST", headers:{"Content-Type":"application/json"},
      body: JSON.stringify({ pin: $("#pin").value.trim(), player: parseInt($("#player").value, 10) }) });
    const data = await res.json().catch(() => ({}));
    if (!res.ok) throw new Error(data.error || ("HTTP " + res.status));
    player = data.player;
    startGame();
  } catch (e) { $("#loginError").textContent = e.message || "Could not connect."; }
}
$("#loginForm").addEventListener("submit", login);

let inputChannel = null;
function send(obj) {
  const message = JSON.stringify(obj);
  if (inputChannel && inputChannel.readyState === "open" && obj.t !== "rtc") inputChannel.send(message);
  else if (ws && ws.readyState === 1) ws.send(message);
}
function receivedPong() {
  const direct = inputChannel && inputChannel.readyState === "open";
  $("#hudPing").textContent = (Date.now() - pingStart) + " ms · " + (direct ? "WebRTC" : "Tunnel");
  clearTimeout(pingTimer);
  pingTimer = setTimeout(() => { pingStart = Date.now(); send({t:"ping"}); }, 2000);
}

let rtcPc = null, rtcTimer = null, rtcLive = false;
let soundMuted = false;

function updateSoundButton() {
  const video = $("#rtcVideo");
  $("#sound").textContent = video.muted ? "Enable sound" : "Mute sound";
  $("#sound").setAttribute("aria-pressed", String(!video.muted));
}
function playRemoteVideo() {
  const video = $("#rtcVideo");
  return video.play().then(updateSoundButton).catch(() => {
    // Mobile browsers may require another tap before audible playback.
    // Keep the video running and make that action visible.
    video.muted = true;
    updateSoundButton();
    return video.play().catch(() => {});
  });
}
$("#sound").addEventListener("click", () => {
  const video = $("#rtcVideo");
  soundMuted = !video.muted;
  video.muted = soundMuted;
  playRemoteVideo();
});

function useMjpeg() {
  if (rtcLive) return;
  if (rtcTimer) { clearTimeout(rtcTimer); rtcTimer = null; }
  $("#rtcVideo").style.display = "none";
  $("#rtcVideo").muted = true;
  $("#sound").hidden = true;
  $("#stream").style.display = "";
  if (!$("#stream").src) $("#stream").src = "/stream.mjpg?t=" + Date.now();
}

let rtcPendingCandidates = [], rtcSignaling = Promise.resolve();

function startRtc() {
  if (!window.RTCPeerConnection) { useMjpeg(); return; }
  if (rtcPc) return;
  // The host sends the offer; this phone answers.
  rtcPc = new RTCPeerConnection({ iceServers: [{ urls: "stun:stun.l.google.com:19302" }] });
  const pc = rtcPc;
  const stream = new MediaStream();
  const video = $("#rtcVideo");
  video.srcObject = stream;
  video.muted = soundMuted;
  pc.ondatachannel = e => {
    if (e.channel.label !== "input") return;
    inputChannel = e.channel;
    e.channel.onmessage = event => {
      try { if (JSON.parse(event.data).t === "pong") receivedPong(); } catch (e) {}
    };
    e.channel.onclose = () => { if (inputChannel === e.channel) inputChannel = null; };
  };
  rtcPc.ontrack = e => {
    if (rtcPc !== pc) return;
    stream.addTrack(e.track);
    if (e.track.kind === "audio") $("#sound").hidden = false;
    const showVideo = () => {
      if (rtcPc !== pc || !video.videoWidth) return;
      rtcLive = true;
      if (rtcTimer) { clearTimeout(rtcTimer); rtcTimer = null; }
      video.style.display = "";
      $("#stream").style.display = "none";
      $("#stream").removeAttribute("src");
    };
    video.onloadeddata = showVideo;
    video.onresize = showVideo;
    playRemoteVideo();
  };
  rtcPc.onicecandidate = e => {
    if (e.candidate) send({ t:"rtc", kind:"candidate", candidate: e.candidate.candidate, mid: e.candidate.sdpMid || "video" });
  };
  rtcPc.oniceconnectionstatechange = () => {
    if (rtcPc === pc && (pc.iceConnectionState === "failed" || pc.iceConnectionState === "closed")) { rtcLive = false; useMjpeg(); }
  };
}

async function handleRtc(m) {
  if (!rtcPc) startRtc();
  if (!rtcPc) return;
  try {
    if (m.kind === "offer") {
      await rtcPc.setRemoteDescription({ type: "offer", sdp: m.sdp });
      for (const c of rtcPendingCandidates) await rtcPc.addIceCandidate(c);
      rtcPendingCandidates = [];
      const answer = await rtcPc.createAnswer();
      await rtcPc.setLocalDescription(answer);
      send({ t:"rtc", kind:"answer", sdp: rtcPc.localDescription.sdp });
      // No video within 8 seconds (NAT blocked WebRTC): fall back to MJPEG.
      if (rtcTimer) clearTimeout(rtcTimer);
      rtcTimer = setTimeout(() => { if (!rtcLive) useMjpeg(); }, 8000);
    }
    else if (m.kind === "candidate" && m.candidate) {
      const cand = { candidate: m.candidate, sdpMid: m.mid || "video" };
      if (rtcPc.remoteDescription) await rtcPc.addIceCandidate(cand);
      else rtcPendingCandidates.push(cand);
    }
  } catch (e) { useMjpeg(); }
}

function startGame() {
  $("#login").style.display = "none";
  $("#game").style.display = "block";
  $("#hudPlayer").textContent = "Player " + (player + 1);
  // Start with MJPEG hidden until WebRTC proves itself; useMjpeg shows it.
  $("#stream").style.display = "none";
  // If the host never offers WebRTC, fall back to MJPEG shortly.
  rtcTimer = setTimeout(() => { if (!rtcLive && !rtcPc) useMjpeg(); }, 3000);
  connectWs();
  if (navigator.wakeLock) navigator.wakeLock.request("screen").catch(() => {});
}

function connectWs() {
  const proto = location.protocol === "https:" ? "wss://" : "ws://";
  ws = new WebSocket(proto + location.host + "/ws");
  ws.onopen = () => {
    stopConnectionTimers();
    pingStart = Date.now();
    send({t:"ping"});
    // Proxies still need traffic on the signaling socket after controller
    // messages and latency probes move onto the direct WebRTC channel.
    keepaliveTimer = setInterval(() => {
      if (ws && ws.readyState === 1) ws.send(JSON.stringify({t:"keepalive"}));
    }, 15000);
  };
  ws.onmessage = ev => {
    try { const m = JSON.parse(ev.data);
      if (m.t === "pong") receivedPong();
      else if (m.t === "rtc-ready") startRtc();
      else if (m.t === "rtc") rtcSignaling = rtcSignaling.then(() => handleRtc(m));
    } catch (e) {}
  };
  ws.onclose = () => {
    stopConnectionTimers();
    if (rtcTimer) clearTimeout(rtcTimer);
    const pc = rtcPc;
    rtcPc = null;
    inputChannel = null;
    rtcLive = false;
    rtcPendingCandidates = [];
    rtcSignaling = Promise.resolve();
    if (pc) pc.close();
    $("#rtcVideo").pause();
    $("#rtcVideo").srcObject = null;
    $("#sound").hidden = true;
    $("#stream").removeAttribute("src");
    $("#game").style.display = "none";
    $("#login").style.display = "flex";
    $("#loginError").textContent = "Disconnected from Hollycast. Connect again to rejoin.";
  };
}

function bindButton(el, down, up) {
  el.addEventListener("pointerdown", e => { e.preventDefault(); el.setPointerCapture(e.pointerId); el.classList.add("on"); down(); });
  const end = e => { el.classList.remove("on"); up(); };
  el.addEventListener("pointerup", end);
  el.addEventListener("pointercancel", end);
  el.addEventListener("lostpointercapture", () => el.classList.remove("on"));
}
document.querySelectorAll("[data-btn]").forEach(el => {
  const name = el.getAttribute("data-btn");
  bindButton(el, () => send({t:"btn", k:name, p:1}), () => send({t:"btn", k:name, p:0}));
});
document.querySelectorAll("[data-trig]").forEach(el => {
  const name = el.getAttribute("data-trig");
  bindButton(el, () => send({t:"axis", k:name, v:1}), () => send({t:"axis", k:name, v:0}));
});

// Analog stick
(function () {
  const stick = $("#stick"), knob = $("#knob");
  let active = null, lastSent = 0;
  function move(e) {
    const r = stick.getBoundingClientRect();
    let dx = (e.clientX - (r.left + r.width / 2)) / (r.width / 2);
    let dy = (e.clientY - (r.top + r.height / 2)) / (r.height / 2);
    const len = Math.hypot(dx, dy);
    if (len > 1) { dx /= len; dy /= len; }
    knob.style.transform = "translate(" + (dx * 32) + "px," + (dy * 32) + "px)";
    const now = Date.now();
    if (now - lastSent > 16) { lastSent = now; send({t:"stick", x:+dx.toFixed(3), y:+dy.toFixed(3)}); }
  }
  stick.addEventListener("pointerdown", e => { active = e.pointerId; stick.setPointerCapture(active); move(e); });
  stick.addEventListener("pointermove", e => { if (e.pointerId === active) move(e); });
  const end = () => { active = null; knob.style.transform = ""; send({t:"stick", x:0, y:0}); };
  stick.addEventListener("pointerup", end);
  stick.addEventListener("pointercancel", end);
})();

// Bluetooth / USB gamepad paired to the phone (standard mapping)
(function () {
  const prev = [];
  const names = ["A","B","X","Y",null,null,"LT","RT",null,"START",null,null,"UP","DOWN","LEFT","RIGHT"];
  function poll() {
    const gps = navigator.getGamepads ? navigator.getGamepads() : [];
    for (const gp of gps) {
      if (!gp) continue;
      gp.buttons.forEach((b, i) => {
        const name = names[i]; if (!name) return;
        const key = gp.index + ":" + i, was = !!prev[key], now = b.pressed;
        if (now !== was) { prev[key] = now;
          if (name === "LT" || name === "RT") send({t:"axis", k:name, v: now ? b.value : 0});
          else send({t:"btn", k:name, p: now ? 1 : 0});
        } else if ((name === "LT" || name === "RT") && now) send({t:"axis", k:name, v:b.value});
      });
      const x = gp.axes[0] || 0, y = gp.axes[1] || 0;
      const key = gp.index + ":stick", last = prev[key] || [0,0];
      if (Math.abs(x - last[0]) > 0.02 || Math.abs(y - last[1]) > 0.02) {
        prev[key] = [x, y];
        send({t:"stick", x:+x.toFixed(3), y:+y.toFixed(3)});
      }
    }
    requestAnimationFrame(poll);
  }
  requestAnimationFrame(poll);
})();

window.addEventListener("pagehide", () => send({t:"release"}));
document.addEventListener("contextmenu", e => e.preventDefault());
</script>
</body>
</html>
)HTML";
}

} // namespace remoteplay
