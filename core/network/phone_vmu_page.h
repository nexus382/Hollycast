// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
namespace phone_vmu {
// Adjacent literals keep each chunk below MSVC's 16,380-byte limit.
inline constexpr char page[] = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#e4e7dc"><meta name="referrer" content="no-referrer">
<title>Hollycast VMU</title>
<style>
:root {
  color-scheme: light;
  font-family: Arial, Helvetica, sans-serif;
  color: #46534c;
  background: #c7cdc4;
  --shell: #e4e7dc;
  --shell-light: #f7f8ef;
  --shell-edge: #b6c0b0;
  --print: #707c70;
  --lcd: #9eb3a5;
  --key-light: #c4ced0;
  --key-dark: #96a5aa;
}
* { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
/* Suppress double-tap zoom on the casing and overlay as well as the LCD/buttons. */
html, body { margin: 0; width: 100%; height: 100%; overflow: hidden; overscroll-behavior: none; touch-action: manipulation; }
body { user-select: none; -webkit-user-select: none; }

/* The viewport is the moulded casing, with no surrounding page or device card. */
.device {
  container-type: size;
  position: fixed;
  inset: 0;
  display: grid;
  grid-template-rows: minmax(78px, .48fr) minmax(0, 1.25fr) minmax(210px, 1fr);
  gap: clamp(8px, 1.8vh, 18px);
  padding: max(24px, env(safe-area-inset-top)) clamp(18px, 6vw, 36px)
    max(34px, env(safe-area-inset-bottom));
  border: 1px solid #b9c2b3;
  border-radius: 14px 14px 54px 54px / 14px 14px 28px 28px;
  background: linear-gradient(105deg, #f1f3e9 0%, var(--shell) 14%, #e7eade 75%, #d9dfd0 100%);
  box-shadow: inset 3px 0 3px #ffffffb0, inset -4px 0 4px #a1ae9c50,
    inset 0 -5px 3px #9ba99450, inset 0 2px 1px #fff;
}
.device::before, .device::after {
  content: '';
  position: absolute;
  pointer-events: none;
  border-radius: 8px 8px 44px 44px / 8px 8px 22px 22px;
}
.device::before {
  inset: 30px 11px 12px;
  border-left: 1px solid #b5c0ac6b;
  border-right: 1px solid #b5c0ac6b;
  box-shadow: -2px 0 1px #fff8, 2px 0 1px #fff8;
}
.device::after {
  inset: 38px 19px 6px;
  border-bottom: 1px solid #a8b69c66;
  box-shadow: 0 2px 1px #ffffffa0;
}
.connector {
  position: absolute;
  top: 0;
  left: 20%;
  width: 60%;
  height: 22px;
  border: 1px solid #777e75;
  border-top: 0;
  border-radius: 0 0 3px 3px;
  background: linear-gradient(#626d63, #899385);
  box-shadow: inset 0 -2px 2px #3b49364d;
}
.connector::before {
  content: '';
  position: absolute;
  left: 50%; top: 0; bottom: 0;
  border-left: 2px solid #44503f88;
  border-right: 1px solid #b6bfae66;
}
.brand {
  display: flex;
  align-items: center;
  justify-content: center;
  align-self: end;
  gap: 8px;
  padding-bottom: 7px;
  color: #778170;
  font-size: clamp(27px, 7.5vw, 39px);
  font-weight: 500;
  letter-spacing: -1.6px;
  line-height: 1;
  text-shadow: 0 1px 0 #fff9;
}
.brand svg { width: 39px; height: 39px; overflow: visible; flex: none; }
.screen { display: grid; place-items: center; min-width: 0; min-height: 0; }
.screen-bezel {
  width: 100%;
  aspect-ratio: 1.16;
  display: grid;
  align-items: center;
  padding: clamp(11px, 3.6vw, 18px);
  border: 1px solid #dbe1d1;
  border-radius: 9px;
  background: linear-gradient(130deg, #f0f3e6, #d3dcc8);
  box-shadow: -1px -1px 2px #a0ae9255, 2px 2px 2px #fff9, inset 1px 1px 1px #fff;
}
@supports (width: 1cqh) {
  .screen { container-type: size; }
  .screen-bezel { width: min(100%, 116cqh); }
}
.lcd-glass {
  position: relative;
  aspect-ratio: 6 / 5;
  overflow: hidden;
  border: 1px solid #718577;
  border-radius: 5px;
  background: var(--lcd);
  box-shadow: 0 0 0 2px #bac7b5, 0 1px 1px #fff;
}
.lcd-glass::after {
  content: '';
  position: absolute;
  inset: 0;
  border-radius: inherit;
  box-shadow: inset 3px 4px 7px #253d3040, inset -1px -1px 2px #e0e9d44d;
  pointer-events: none;
}
canvas {
  display: block;
  width: 100%;
  height: 80%;
  background: var(--lcd);
  image-rendering: pixelated;
  image-rendering: crisp-edges;
  touch-action: manipulation;
}
canvas[height="40"] { height: 100%; }
canvas.rotated { transform: rotate(180deg); }
.lcd-icons {
  height: 20%;
  display: flex;
  align-items: center;
  justify-content: space-around;
  padding: 0 6%;
  color: #52705c;
  opacity: .18;
  pointer-events: none;
}
.lcd-icons svg { width: 13%; height: 64%; }
canvas[height="40"] + .lcd-icons { display: none; }

.hardware { position: relative; min-height: 0; }
.system {
  position: absolute;
  z-index: 2;
  top: 4%; right: 0;
  width: 51%;
  display: flex;
  justify-content: space-between;
  gap: 7px;
}
.system button, .actions button {
  position: relative;
  padding: 0;
  border: 0;
  border-radius: 0;
  background: none;
  box-shadow: none;
  color: var(--print);
}
.system button { width: 48%; height: 76px; }
.key-label { display: block; pointer-events: none; }
.system .key-label {
  position: absolute;
  top: 0; left: 0; right: 0;
  font-family: 'Arial Narrow', Arial, sans-serif;
  font-size: clamp(12px, 3.7vw, 17px);
  font-weight: 600;
  letter-spacing: -.5px;
  text-transform: uppercase;
  text-shadow: 0 1px 0 #fff8;
}
.system button::before, .actions button::before {
  content: '';
  position: absolute;
  left: 50%;
  transform: translateX(-50%);
  border-radius: 50%;
  border: 1px solid #829393;
  background: linear-gradient(140deg, var(--key-light), var(--key-dark));
  box-shadow: 0 2px 2px #53675c7a, 0 0 0 3px #d0d9c6,
    0 0 1px 4px #8e9d8677, inset 1px 2px 2px #eff6edc9, inset -1px -2px 2px #7d919050;
}
.system button::before { top: 29px; width: clamp(24px, 7.4vw, 33px); height: clamp(24px, 7.4vw, 33px); }
.pad { position: absolute; inset: 0; }
.dpad {
  position: absolute;
  top: 51%; left: -1%;
  transform: translateY(-50%);
  width: clamp(132px, 43vw, 172px);
  aspect-ratio: 1;
}
.dpad::before {
  content: '';
  position: absolute;
  inset: -1px;
  border-radius: 50%;
  background: linear-gradient(135deg, #64776c, #b8c2b4 65%, #d1d8cb);
  border: 1px solid #a3b19a;
  box-shadow: 0 0 0 4px #d9e1cf, 0 0 1px 5px #bfccb4, inset 2px 3px 4px #3a4d4666;
}
.dpad-face {
  position: absolute;
  inset: 0;
  width: 100%; height: 100%;
  filter: drop-shadow(0 3px 1px #5a71698f);
  pointer-events: none;
}
.dpad button {
  position: absolute;
  z-index: 1;
  width: calc(100% / 3); height: calc(100% / 3);
  min-height: 0;
  padding: 0;
  border: 0;
  border-radius: 7px;
  background: none;
  color: transparent;
  box-shadow: none;
}
.dpad button[data-key="0"] { top: 0; left: calc(100% / 3); }
.dpad button[data-key="1"] { bottom: 0; left: calc(100% / 3); }
.dpad button[data-key="2"] { top: calc(100% / 3); left: 0; }
.dpad button[data-key="3"] { top: calc(100% / 3); right: 0; }
.actions {
  position: absolute;
  top: 47%; right: 0;
  width: 49%;
  display: flex;
  justify-content: space-between;
  gap: 12px;
}
.actions button { flex: 1; min-width: 44px; height: 101px; }
.actions button::before { top: 6px; width: clamp(44px, 14vw, 61px); height: clamp(44px, 14vw, 61px); }
.actions .key-label {
  position: absolute;
  bottom: 0; left: 0; right: 0;
  font-size: clamp(22px, 6vw, 29px);
  font-weight: 600;
  line-height: 1;
  text-shadow: 0 1px 0 #fff8;
}
.actions button[data-key="4"] { transform: translateY(12px); }
.actions button[data-key="5"] { transform: translateY(-12px); }
.hardware button:disabled { opacity: .8; }
.hardware button.pressed::before, .hardware button:active::before {
  margin-top: 2px;
  background: #92a3a4;
  box-shadow: 0 0 0 3px #d0d9c6, inset 1px 2px 3px #5f776c66;
}
.dpad button.pressed, .dpad button:active { background: #e9f5e329; box-shadow: inset 0 2px 3px #415b5133; }
.dpad:has(button.pressed) .dpad-face { transform: translateY(1px); filter: brightness(.96) drop-shadow(0 1px 1px #5a71698f); }

#menu {
  position: fixed;
  top: max(8px, env(safe-area-inset-top)); right: max(8px, env(safe-area-inset-right));
  z-index: 2;
  font-size: 13px;
  color: #52614e;
  background: #f2f6e9e8;
}
#controls {
  position: fixed;
  z-index: 3;
  inset: max(60px, env(safe-area-inset-top)) 12px auto;
  max-height: calc(100% - 80px);
  overflow: auto;
  margin-left: auto;
  width: min(440px, calc(100% - 24px));
  padding: 16px;
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 12px;
  background: #f2f5ecee;
  border: 1px solid #b5c2ab;
  border-radius: 14px;
  box-shadow: 0 6px 24px #42553c38;
}
#controls[hidden], button[hidden], label[hidden] { display: none; }
#controls p, #controls details { margin: 0; width: 100%; font-size: 14px; line-height: 1.5; }
#status {
  position: fixed;
  z-index: 1;
  bottom: max(12px, env(safe-area-inset-bottom)); left: 16px; right: 16px;
  width: fit-content;
  max-width: calc(100% - 32px);
  margin: 0 auto;
  padding: 10px 14px;
  border-radius: 12px;
  background: #f2f5ecee;
  font-size: 14px;
  line-height: 1.5;
  text-align: center;
  pointer-events: none;
}
body.controls-hidden #menu { opacity: 0; pointer-events: none; }
body.controls-hidden #menu:focus-visible { opacity: 1; pointer-events: auto; }
body.connected.controls-hidden #status { display: none; }
label { display: flex; align-items: center; gap: 8px; font-size: 16px; }
select, button, .file {
  font: inherit;
  font-size: 16px;
  min-height: 44px;
  border: 1px solid #bac5b2;
  border-radius: 8px;
  padding: 8px 14px;
  color: inherit;
  background: #f9fbf4;
  touch-action: manipulation;
}
button, .file { cursor: pointer; }
button:disabled { opacity: .5; cursor: default; }
select:focus-visible, button:focus-visible, .file:focus-within { outline: 3px solid #607da7; outline-offset: 3px; }
.file input { position: absolute; opacity: 0; width: 1px; height: 1px; }
#controls button[aria-pressed="true"] { background: #526657; color: #fff; }

body.lcd-only .device { display: block; padding: 0; border: 0; border-radius: 0; background: var(--lcd); box-shadow: none; }
body.lcd-only .device::before, body.lcd-only .device::after,
body.lcd-only .brand, body.lcd-only .connector, body.lcd-only .hardware { display: none; }
body.lcd-only .screen { height: 100%; container-type: normal; }
body.lcd-only .screen-bezel, body.lcd-only .lcd-glass {
  position: fixed; inset: 0; width: 100%; height: 100%; aspect-ratio: auto;
  padding: 0; border: 0; border-radius: 0; box-shadow: none; background: var(--lcd);
}
body.lcd-only .lcd-glass::after, body.lcd-only .lcd-icons { display: none; }
body.lcd-only canvas { width: 100%; height: 100%; }

@media (orientation: landscape) {
  .device {
    grid-template: 48px minmax(0, 1fr) / minmax(0, 1fr) minmax(280px, .68fr);
    gap: 10px 24px;
    padding: max(18px, env(safe-area-inset-top)) max(24px, env(safe-area-inset-right))
      max(22px, env(safe-area-inset-bottom)) max(24px, env(safe-area-inset-left));
  }
  .connector { left: 12%; width: 30%; height: 12px; }
  .brand { grid-area: 1 / 1; align-self: center; padding: 0; font-size: 26px; }
  .brand svg { width: 29px; height: 29px; }
  .screen { grid-area: 2 / 1; }
  .screen-bezel { padding: 11px; }
  .hardware { grid-area: 1 / 2 / 3 / 3; }
  .dpad { width: 132px; top: 56%; }
  .system { top: 10%; width: 48%; }
  .system button { height: 70px; }
  .system .key-label { font-size: 13px; }
  .system button::before { width: 25px; height: 25px; top: 26px; }
  .actions { top: 52%; width: 48%; gap: 9px; }
  .actions button { height: 90px; }
  .actions button::before { width: 46px; height: 46px; }
  .actions .key-label { font-size: 22px; }
}
</style></head><body>
<main class="device" aria-label="Virtual Memory Unit">
<div class="connector" aria-hidden="true"></div>
<div class="brand" aria-hidden="true">
)HTML"
R"HTML(<svg viewBox="0 0 64 64" fill="none"><path d="M32.00,30.80 L32.13,30.63 L32.30,30.48 L32.49,30.35 L32.71,30.25 L32.96,30.17 L33.22,30.12 L33.51,30.11 L33.80,30.14 L34.10,30.21 L34.41,30.33 L34.71,30.48 L35.00,30.68 L35.28,30.92 L35.54,31.20 L35.77,31.52 L35.97,31.88 L36.14,32.28 L36.26,32.70 L36.34,33.15 L36.37,33.63 L36.35,34.12 L36.28,34.61 L36.14,35.12 L35.95,35.62 L35.70,36.11 L35.39,36.59 L35.02,37.04 L34.60,37.47 L34.12,37.85 L33.59,38.20 L33.01,38.49 L32.40,38.74 L31.74,38.92 L31.05,39.03 L30.34,39.07 L29.61,39.05 L28.87,38.94 L28.13,38.76 L27.40,38.50 L26.69,38.16 L25.99,37.74 L25.33,37.24 L24.71,36.67 L24.14,36.02 L23.63,35.31 L23.19,34.54 L22.81,33.71 L22.52,32.84 L22.31,31.92 L22.19,30.97 L22.16,29.99 L22.24,29.01 L22.41,28.02 L22.68,27.03 L23.06,26.06 L23.54,25.12 L24.12,24.21 L24.79,23.36 L25.56,22.56 L26.41,21.83 L27.35,21.19 L28.36,20.62 L29.43,20.16 L30.56,19.79 L31.74,19.54 L32.95,19.40 L34.19,19.38 L35.44,19.48 L36.69,19.70 L37.93,20.06 L39.14,20.54 L40.32,21.14 L41.44,21.86 L42.50,22.70 L43.49,23.65 L44.39,24.71 L45.19,25.86 L45.88,27.10 L46.46,28.42 L46.91,29.80 L47.22,31.23 L47.40,32.71 L47.43,34.21 L47.31,35.73 L47.05,37.24 L46.63,38.74 L46.06,40.20 L45.35,41.62 L44.49,42.97 L43.50,44.25 L42.37,45.43 L41.13,46.52 L39.77,47.48 L38.30,48.32 L36.75,49.02 L35.12,49.56 L33.43,49.96 L31.70,50.18 L29.93,50.24 L28.15,50.13 L26.37,49.84 L24.61,49.37 L22.89,48.73 L21.23,47.92 L19.63,46.95 L18.13,45.81 L16.73,44.52 L15.45,43.09 L14.31,41.53 L13.32,39.85 L12.48,38.07 L11.82,36.20 L11.34,34.26 L11.04,32.26 L10.94,30.23 L11.04,28.18 L11.34,26.14 L11.83,24.11 L12.53,22.13 L13.42,20.21 L14.51,18.37 L15.77,16.63 L17.21,15.01 L18.82,13.53 L20.57,12.20 L22.45,11.03 L24.46,10.05 L26.56,9.25 L28.75,8.66 L31.01,8.29 L33.30,8.13 L35.62,8.19 L37.93,8.48 L40.23,9.00 L42.47,9.74 L44.66,10.70 L46.75,11.88 L48.73,13.26 L50.58,14.84 L52.28,16.60 L53.82,18.53 L55.17,20.62 L56.32,22.84 L57.25,25.18 L57.96,27.61 L58.44,30.11 L58.67,32.67 L58.66,35.25 L58.39,37.84 L57.87,40.41" stroke="currentColor" stroke-width="4.2" stroke-linecap="round" stroke-linejoin="round"/></svg>
<span>Dreamcast</span>
</div>
<div class="screen"><div class="screen-bezel"><div class="lcd-glass">
<canvas id="lcd" width="48" height="32" role="img" aria-label="Live VMU LCD. Tap to show controls."></canvas>
<div class="lcd-icons" aria-hidden="true">
<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M6 3h9l3 3v15H6zM9 10h6M9 14h6M9 18h4"/></svg>
<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M7 7h10l4 10-3 2-4-4h-4l-4 4-3-2zM6 12h6M9 9v6M16 11h1M18 14h1"/></svg>
<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="9"/><path d="M12 6v7h5"/></svg>
<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M9 15l6-6M8 10l-3 3a4 4 0 0 0 6 6l3-3M10 8l3-3a4 4 0 0 1 6 6l-3 3"/></svg>
</div></div></div></div>
<div class="hardware">
<div class="system">
<button type="button" data-key="7" aria-label="Sleep or wake VMU"><span class="key-label">Sleep</span></button>
<button type="button" data-key="6" aria-label="VMU Mode"><span class="key-label">Mode</span></button>
</div>
<div class="pad"><div class="dpad" role="group" aria-label="Directional pad">
<svg class="dpad-face" viewBox="0 0 100 100" aria-hidden="true">
<defs><linearGradient id="dpad-surface" x1="0" y1="0" x2="1" y2="1"><stop stop-color="#c9d2d1"/><stop offset=".45" stop-color="#aebdbf"/><stop offset="1" stop-color="#8d9fa4"/></linearGradient></defs>
<path d="M36 8H64Q69 8 69 13V31H87Q92 31 92 36V64Q92 69 87 69H69V87Q69 92 64 92H36Q31 92 31 87V69H13Q8 69 8 64V36Q8 31 13 31H31V13Q31 8 36 8Z" fill="url(#dpad-surface)" stroke="#d7e0d9" stroke-width="1.2"/>
<circle cx="50" cy="50" r="1.7" fill="#9eafb0" stroke="#d4ded5" stroke-width=".8"/>
</svg>
<button type="button" data-key="0" aria-label="Up">▲</button><button type="button" data-key="2" aria-label="Left">◀</button>
<button type="button" data-key="3" aria-label="Right">▶</button><button type="button" data-key="1" aria-label="Down">▼</button>
</div>
<div class="actions" role="group" aria-label="VMU action buttons">
<button type="button" data-key="4" aria-label="VMU A"><span class="key-label">A</span></button>
<button type="button" data-key="5" aria-label="VMU B"><span class="key-label">B</span></button>
</div></div></div>
</main>
<p id="status" role="status" aria-live="polite">Connecting to Hollycast…</p>
<button id="menu" type="button" aria-controls="controls" aria-expanded="true">Controls</button>
<aside id="controls" aria-label="VMU controls">
<label for="slot">VMU <select id="slot"><option value="0">A1</option><option value="1">A2</option><option value="2">B1</option><option value="3">B2</option><option value="4">C1</option><option value="5">C2</option><option value="6">D1</option><option value="7">D2</option></select></label>
<button id="layout" type="button">LCD only</button><button id="rotate" type="button" aria-pressed="false">Rotate LCD</button><button id="fullscreen" type="button" hidden>Fullscreen</button>
<button id="awake" type="button" aria-pressed="true">Keep awake: On</button>
<p id="awake-status" role="status">Tap the VMU to keep this screen awake.</p>
<button id="play" type="button">Play mini-game on this card</button>
<label class="file">Open VMU game<input id="game-file" type="file" accept=".vms,.bin,.vmu"></label>
<button id="mirror" type="button" hidden>Return to Dreamcast</button><button id="restart" type="button" hidden>Restart mini-game</button><button id="download" type="button" hidden>Download VMU copy</button>
<p id="sync-status" role="status" aria-live="polite"></p>
<button id="sound" type="button" aria-pressed="false">Sound off</button>
<label class="file">Load VMU BIOS<input id="bios-file" type="file" accept=".bin"></label>
<p>Mode opens the card's mini-game. Progress syncs to Hollycast automatically. Choose Return to Dreamcast to reconnect the card. Games opened from your phone stay local.</p>
<p>Game files: .vms, .bin or .vmu. Optional BIOS: your own 64 KiB VMU firmware dump.</p>
<details><summary>Credits and license</summary>VMU core: Bucanero's JavaScript port of Marcus Comstedt's SoftVMS 1.10. <a href="vmu-core.js">Source</a> · <a href="vmu-core.LICENSE">GPLv3 license</a>. Not cycle exact; some games may need the original VMU BIOS. Keep-awake media: Rich Tibbett's <a href="wake-media.js">NoSleep.js assets and MIT license</a>.</details>
<button id="hide" type="button">Hide controls</button></aside>
<script src="vmu-core.js"></script><script src="vmu-player.js"></script><script src="vmu-sync.js"></script><script src="wake-media.js"></script>
<script>
'use strict';
const $ = id => document.getElementById(id);
const lcd=$('lcd'),ctx=lcd.getContext('2d'),slot=$('slot'),status=$('status'),controls=$('controls'),menu=$('menu');
let controlsTimeout,bios=null,mirrorSleeping=false,loading=false,audio,oscillator,gain,muted=true;
function hideControls(){controls.hidden=true;menu.setAttribute('aria-expanded','false');document.body.classList.add('controls-hidden');}
function showControls(){controls.hidden=false;menu.setAttribute('aria-expanded','true');document.body.classList.remove('controls-hidden');clearTimeout(controlsTimeout);controlsTimeout=setTimeout(hideControls,4000);}
function notice(message,connected=false){status.textContent=message;document.body.classList.toggle('connected',connected);}
function blank(height=32){if(lcd.height!==height)lcd.height=height;ctx.fillStyle='#9eb3a5';ctx.fillRect(0,0,48,height);}
function draw(pixels,height){
 if(lcd.height!==height)lcd.height=height;
 const image=ctx.createImageData(48,height);
 for(let i=0;i<48*height;++i)image.data.set(pixels[i]?[35,57,46,255]:[158,179,165,255],i*4);
 ctx.putImageData(image,0,0);
}
function tone(frequency){
 if(!gain)return;
 if(frequency>0&&!muted&&!document.hidden){oscillator.frequency.setValueAtTime(frequency/2,audio.currentTime);gain.gain.setValueAtTime(.06,audio.currentTime);}
 else gain.gain.setValueAtTime(0,audio.currentTime);
}
function startAudio(){
 if(muted)return;
 try{
  if(audio){audio.resume().catch(()=>{});return;}
  const Audio=window.AudioContext||window.webkitAudioContext;if(!Audio)return;
  audio=new Audio();oscillator=audio.createOscillator();gain=audio.createGain();oscillator.type='square';gain.gain.value=0;
  oscillator.connect(gain).connect(audio.destination);oscillator.start();
 }catch(_){audio=oscillator=gain=null;}
}
const player=new PhoneVmuPlayer(VMS,pixels=>draw(pixels,40),message=>notice(message,player.active&&!player.sleeping&&!VMS.halted),tone);
let progressStorage=null;try{progressStorage=window.sessionStorage;}catch(_){}
const cardSync=new PhoneVmuSync(VMS,message=>{$('sync-status').textContent=message;updateMode();},progressStorage,'hollycast-vmu-progress:'+window.location?.href);
let keepAwake=true,wakeLock=null,wakeVideo=null,wakePending=false,wakeGeneration=0,wakeInteracted=false,pageVisible=true;
const awakeStatus=$('awake-status');
function wantsWakeLock(){return keepAwake&&pageVisible&&!document.hidden&&!mirrorSleeping&&!(player.active&&player.sleeping);}
function releaseWakeLock(){
 ++wakeGeneration;
 const lock=wakeLock;wakeLock=null;if(lock)lock.release().catch(()=>{});
 if(wakeVideo)wakeVideo.pause();
}
async function keepScreenAwake(){
 if(!wantsWakeLock()){
  releaseWakeLock();awakeStatus.textContent=keepAwake?'Keep awake paused.':'Automatic screen sleep allowed.';return;
 }
 if(wakeLock||wakePending||(wakeVideo&&!wakeVideo.paused))return;
 const native=!!navigator.wakeLock;
 if(!native&&!wakeInteracted)return;
 const generation=wakeGeneration;wakePending=true;
 try{
  if(native){
   const lock=await navigator.wakeLock.request('screen');
   if(generation!==wakeGeneration||!wantsWakeLock()){await lock.release();return;}
   wakeLock=lock;
   lock.addEventListener('release',()=>{if(wakeLock===lock){wakeLock=null;awakeStatus.textContent='Keep awake released. Tap the VMU to retry.';}});
  }else{
   // Local HTTP cannot use the native API. Play NoSleep's silent inline video
   // from a user gesture instead; never use the old page-reload workaround.
   if(!wakeVideo){
    wakeVideo=document.createElement('video');wakeVideo.setAttribute('playsinline','');wakeVideo.setAttribute('aria-hidden','true');
    wakeVideo.addEventListener('pause',()=>{if(wantsWakeLock())awakeStatus.textContent='Keep awake paused. Tap the VMU to retry.';});
    for(const type of ['webm','mp4']){const source=document.createElement('source');source.type='video/'+type;source.src=PhoneVmuWakeMedia[type];wakeVideo.appendChild(source);}
    wakeVideo.addEventListener('loadedmetadata',()=>{
     if(wakeVideo.duration<=1)wakeVideo.loop=true;
     else wakeVideo.addEventListener('timeupdate',()=>{if(wakeVideo.currentTime>.5)wakeVideo.currentTime=Math.random();});
    });
   }
   await wakeVideo.play();
   if(generation!==wakeGeneration||!wantsWakeLock()){wakeVideo.pause();return;}
  }
  awakeStatus.textContent='Keeping this screen awake.';
 }catch(_){
  if(generation===wakeGeneration)awakeStatus.textContent='Keep awake unavailable. Tap to retry, or adjust your phone’s Auto-Lock setting.';
 }finally{
  wakePending=false;
  if(generation!==wakeGeneration&&wantsWakeLock())keepScreenAwake();
 }
}
function wakeInteraction(){wakeInteracted=true;keepScreenAwake();}
document.addEventListener('pointerdown',wakeInteraction);
$('awake').addEventListener('click',()=>{keepAwake=!keepAwake;$('awake').textContent=keepAwake?'Keep awake: On':'Keep awake: Off';$('awake').setAttribute('aria-pressed',String(keepAwake));wakeInteracted=true;keepScreenAwake();showControls();});
const hardware=Array.from(document.querySelectorAll('[data-key]')),held=new Map();
function releaseAll(){held.clear();player.releaseAll();hardware.forEach(button=>button.classList.remove('pressed'));}
)HTML"
R"HTML(
function updateMode(){slot.disabled=player.active||loading;$('game-file').disabled=!!cardSync.owner||loading;$('mirror').disabled=cardOpening();$('restart').disabled=cardOpening();for(const id of ['mirror','restart','download'])$(id).hidden=!player.active;$('play').hidden=player.active;hardware.forEach(button=>{button.disabled=+button.dataset.key<6&&!player.active;});}
function openGame(bytes,name){releaseAll();player.load(bytes,name,bios);mirrorSleeping=false;updateMode();showControls();startAudio();keepScreenAwake();}
function returnToDisplay(){cardSync.returned=false;releaseAll();player.stop();mirrorSleeping=false;updateMode();blank();notice('Connecting to Hollycast…');showControls();keepScreenAwake();}
function cardOpening(){return loading||!!(cardSync.owner&&!cardSync.owner.ready);}
async function playCard(){
)HTML"
R"HTML( if(loading||player.active)return;loading=true;updateMode();$('play').disabled=true;notice('Opening the VMU mini-game…');
 const controller=new AbortController(),timeout=setTimeout(()=>controller.abort(),5000);
 try{
  const response=await fetch('card/'+slot.value,{cache:'no-store',signal:controller.signal});
  if(!response.ok)throw new Error('No VMU card is available in this slot. Start a game or open a VMU game file.');
  const revision=response.headers.get('ETag');if(!revision)throw new Error('Update Hollycast to enable automatic VMU progress sync.');
  openGame(new Uint8Array(await response.arrayBuffer()),'hollycast-'+slot.selectedOptions[0].text+'.bin');
  await cardSync.checkout(slot.value,revision,PhoneVmuSync.token());
 }catch(error){
  if(player.active&&!cardSync.owner){returnToDisplay();$('sync-status').textContent='The VMU mini-game did not start. '+error.message;}
  notice(error.name==='AbortError'?'Hollycast did not respond. Check the connection and try again.':error.message);showControls();
 }
 finally{clearTimeout(timeout);loading=false;$('play').disabled=false;updateMode();}
}
function key(index,pressed,source){
 if(cardOpening()||cardSync.returning)return;
 let sources=held.get(index);if(!sources){sources=new Set();held.set(index,sources);}
 const was=sources.size>0;if(pressed)sources.add(source);else sources.delete(source);
 const down=sources.size>0;if(was===down)return;
 const button=hardware.find(button=>+button.dataset.key===index);if(button)button.classList.toggle('pressed',down);
 if(pressed)startAudio();
 if(index===7){if(pressed){if(player.active){player.sleep();cardSync.checkpoint();}else{mirrorSleeping=!mirrorSleeping;if(mirrorSleeping)blank();notice(mirrorSleeping?'Sleeping. Press Sleep to wake the VMU.':'VMU waking…');}keepScreenAwake();}}
 else if(index===6&&!player.active){if(pressed)playCard();}
 else player.key(index,down);
}
hardware.forEach(button=>{
 const index=+button.dataset.key;
 button.addEventListener('pointerdown',event=>{event.preventDefault();button.setPointerCapture(event.pointerId);key(index,true,event.pointerId);});
 for(const event of ['pointerup','pointercancel','lostpointercapture'])button.addEventListener(event,e=>{e.preventDefault();key(index,false,e.pointerId);});
 for(const event of ['keydown','keyup'])button.addEventListener(event,e=>{
  if(e.key==='Enter'||e.key===' '){e.preventDefault();e.stopPropagation();if(!e.repeat)key(index,event==='keydown','button:'+e.code);}
 });
});
const keys={ArrowUp:0,ArrowDown:1,ArrowLeft:2,ArrowRight:3,a:4,z:4,b:5,x:5,m:6,Enter:6,s:7,' ':7};
document.addEventListener('keydown',event=>{
 wakeInteraction();
 if(event.defaultPrevented)return;
 if(event.key==='Escape'){hideControls();return;}if(event.key==='Tab'){showControls();return;}
 if(event.target.closest&&event.target.closest('#controls'))return;
 const index=keys[event.key.length===1?event.key.toLowerCase():event.key];
 if(index!==undefined){event.preventDefault();if(!event.repeat)key(index,true,'key:'+event.code);}
});
document.addEventListener('keyup',event=>{if(event.defaultPrevented)return;const index=keys[event.key.length===1?event.key.toLowerCase():event.key];if(index!==undefined){event.preventDefault();key(index,false,'key:'+event.code);}});
window.addEventListener('blur',()=>{releaseAll();tone(-1);});window.addEventListener('pagehide',()=>{pageVisible=false;releaseAll();tone(-1);cardSync.remember();keepScreenAwake();});
window.addEventListener('pageshow',()=>{pageVisible=true;keepScreenAwake();});
document.addEventListener('visibilitychange',()=>{if(document.hidden){releaseAll();tone(-1);if(!loading)cardSync.checkpoint();}keepScreenAwake();});
lcd.addEventListener('pointerdown',showControls);menu.addEventListener('click',showControls);menu.addEventListener('focus',showControls);
controls.addEventListener('pointerdown',showControls);controls.addEventListener('focusin',()=>clearTimeout(controlsTimeout));controls.addEventListener('focusout',()=>{if(!controls.hidden)showControls();});
$('hide').addEventListener('click',hideControls);$('play').addEventListener('click',playCard);
$('mirror').addEventListener('click',async()=>{if(!player.active||cardOpening())return;releaseAll();$('mirror').disabled=true;try{if(!await cardSync.checkpoint(true)){showControls();return;}returnToDisplay();}finally{$('mirror').disabled=false;}});
$('restart').addEventListener('click',()=>{if(cardOpening()||cardSync.returning)return;releaseAll();player.reset();showControls();keepScreenAwake();});
$('layout').addEventListener('click',()=>{const only=document.body.classList.toggle('lcd-only');$('layout').textContent=only?'Show buttons':'LCD only';showControls();});
$('rotate').addEventListener('click',()=>{$('rotate').setAttribute('aria-pressed',String(lcd.classList.toggle('rotated')));showControls();});
$('sound').addEventListener('click',()=>{muted=!muted;$('sound').textContent=muted?'Sound off':'Sound on';$('sound').setAttribute('aria-pressed',String(!muted));if(muted)tone(-1);else startAudio();showControls();});
slot.addEventListener('change',()=>{releaseAll();blank();notice('Connecting to VMU '+slot.selectedOptions[0].text+'…');showControls();});
$('game-file').addEventListener('change',async event=>{
 if(cardSync.owner)return;
 const file=event.target.files[0];if(!file)return;
 try{if(!/\.(vms|bin|vmu)$/i.test(file.name)||file.size>131072)throw new Error('Choose a .vms game or a 128 KiB .bin/.vmu card.');openGame(new Uint8Array(await file.arrayBuffer()),file.name);}
 catch(error){notice(error.message);showControls();}event.target.value='';
});
$('bios-file').addEventListener('change',async event=>{
 if(cardOpening()||cardSync.returning){event.target.value='';return;}
 const file=event.target.files[0];if(!file)return;
 try{if(file.size!==65536)throw new Error('A VMU BIOS must be exactly 64 KiB.');bios=new Uint8Array(await file.arrayBuffer());notice('VMU BIOS loaded for this tab. Open a mini-game to use it.');showControls();if(player.active){player.core.setBios(bios);player.reset();keepScreenAwake();}}
 catch(error){notice(error.message);showControls();}event.target.value='';
});
$('download').addEventListener('click',()=>{const url=URL.createObjectURL(new Blob([player.core.flash],{type:'application/octet-stream'}));const link=document.createElement('a');link.href=url;link.download='hollycast-vmu-copy.bin';link.click();setTimeout(()=>URL.revokeObjectURL(url),1000);showControls();});
if(document.documentElement.requestFullscreen){
 $('fullscreen').hidden=false;
 $('fullscreen').addEventListener('click',async()=>{try{if(document.fullscreenElement)await document.exitFullscreen();else await document.documentElement.requestFullscreen();}catch(_){notice('Fullscreen is unavailable in this browser.');}showControls();});
 document.addEventListener('fullscreenchange',()=>{$('fullscreen').textContent=document.fullscreenElement?'Exit fullscreen':'Fullscreen';});
}
)HTML"
R"HTML(async function poll(){
 let delay=player.active?1000:50;
 if(player.active&&!loading)cardSync.checkpoint();
 if(!document.hidden){
  const selected=slot.value,controller=new AbortController(),timeout=setTimeout(()=>controller.abort(),2500);
  try{
   const response=await fetch('frame/'+selected,{cache:'no-store',signal:controller.signal});if(!response.ok)throw new Error('Disconnected');
   const bytes=new Uint8Array(await response.arrayBuffer());if(bytes.length!==193)throw new Error('Invalid frame');
   if(!player.active&&slot.value===selected&&!mirrorSleeping&&!loading){
    const active=bytes[0]!==0,pixels=new Uint8Array(48*32);
    for(let i=0;i<pixels.length;++i)pixels[i]=active&&(bytes[1+(i>>3)]&(1<<(i&7)))?1:0;
    draw(pixels,32);notice(active?'VMU '+slot.selectedOptions[0].text+' connected':'Waiting for VMU '+slot.selectedOptions[0].text+'. Start a Dreamcast game, or open a VMU mini-game.',active);
   }
  }catch(_){if(!player.active){blank();notice('Connection lost. Keep Connect Phone enabled in Hollycast. Retrying…');}delay=1000;}
  finally{clearTimeout(timeout);}
 }else delay=500;
 setTimeout(poll,delay);
}
function animate(time){if(cardSync.returned)returnToDisplay();player.tick(time,document.hidden||cardOpening()||cardSync.returning);requestAnimationFrame(animate);}
const recovered=cardSync.restore();if(recovered){try{slot.value=cardSync.owner.slot;openGame(recovered,'recovered-vmu.bin');$('sync-status').textContent='Recovered phone progress. Reconnecting to Hollycast…';}catch(_){$('sync-status').textContent='Phone progress could not be reopened. Keep this page open.';}}
blank();updateMode();showControls();keepScreenAwake();poll();requestAnimationFrame(animate);
</script></body></html>)HTML";
}
