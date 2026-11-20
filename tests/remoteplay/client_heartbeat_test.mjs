// Run with node tests/remoteplay/client_heartbeat_test.mjs.
// Exercise the actual served client with deterministic browser/timer substitutes.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

const source = fs.readFileSync(new URL('../../core/ui/remoteplay/remote_play_page.h', import.meta.url), 'utf8');
const script = source.split('<script>')[1].split('</script>')[0];
const timers = new Map();
let timerId = 0;
const elements = new Map();
const element = key => {
  if (!elements.has(key)) elements.set(key, {
    style: {}, classList: { add() {}, remove() {} }, addEventListener() {},
    removeAttribute() {}, setAttribute() {}, pause() {}, play() { return Promise.resolve(); }
  });
  return elements.get(key);
};
const sockets = [];
class WebSocket {
  constructor() { this.readyState = 1; this.sent = []; sockets.push(this); }
  send(message) { this.sent.push(JSON.parse(message)); }
}
const context = vm.createContext({
  document: { querySelector: element, querySelectorAll: () => [], addEventListener() {} },
  window: { addEventListener() {} }, navigator: {}, location: { protocol: 'https:', host: 'remote.test' },
  WebSocket, Date, console, requestAnimationFrame() {},
  setTimeout: callback => { timers.set(++timerId, { callback, repeat: false }); return timerId; },
  setInterval: callback => { timers.set(++timerId, { callback, repeat: true }); return timerId; },
  clearTimeout: id => timers.delete(id), clearInterval: id => timers.delete(id),
});
vm.runInContext(script, context);
vm.runInContext('connectWs(); ws.onopen(); inputChannel = { readyState: "open", sent: [], send(message) { this.sent.push(JSON.parse(message)); } };', context);
// Transition from the initial tunnel ping to direct WebRTC latency probes.
sockets[0].onmessage({ data: '{"t":"pong"}' });
sockets[0].sent.length = 0;
for (const [id, { callback, repeat }] of [...timers]) {
  if (!repeat) timers.delete(id);
  callback();
}
assert.ok(vm.runInContext('inputChannel.sent.some(message => message.t === "ping")', context),
  'Latency probes must follow the direct controller connection');
assert.ok(sockets[0].sent.length > 0,
  'The tunnel signaling socket needs traffic while controller input uses WebRTC');
sockets[0].onclose();
assert.equal(timers.size, 0, 'Disconnect must cancel timers before another login');
console.log('PASS: direct input probes, tunnel heartbeat, disconnect cleanup');
