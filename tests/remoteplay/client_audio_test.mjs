// Run with node tests/remoteplay/client_audio_test.mjs.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

const source = fs.readFileSync(new URL('../../core/ui/remoteplay/remote_play_page.h', import.meta.url), 'utf8');
const script = source.split('<script>')[1].split('</script>')[0];
const elements = new Map();
let audioAllowed = false;
const element = key => {
  if (!elements.has(key)) elements.set(key, {
    style: {}, hidden: true, muted: true, videoWidth: 0, handlers: {},
    addEventListener(name, callback) { this.handlers[name] = callback; },
    classList: { add() {}, remove() {} }, setAttribute() {}, removeAttribute() {}, pause() {},
    play() { return this.muted || audioAllowed ? Promise.resolve() : Promise.reject(new Error('NotAllowedError')); }
  });
  return elements.get(key);
};
class MediaStream {
  tracks = [];
  addTrack(track) { this.tracks.push(track); }
}
class RTCPeerConnection { close() {} }
const context = vm.createContext({
  document: { querySelector: element, querySelectorAll: () => [], addEventListener() {} },
  window: { RTCPeerConnection, addEventListener() {} }, navigator: {},
  RTCPeerConnection, MediaStream, Date, console, requestAnimationFrame() {},
  setTimeout() {}, clearTimeout() {}, clearInterval() {},
});
vm.runInContext(script, context);
vm.runInContext('startRtc(); rtcPc.ontrack({track:{kind:"audio"}});', context);
await new Promise(setImmediate);
const video = element('#rtcVideo'), sound = element('#sound');
video.onloadeddata();
assert.equal(vm.runInContext('rtcLive', context), false, 'Audio alone must not mark video as ready');
assert.equal(sound.hidden, false);
assert.equal(video.muted, true, 'Blocked audible autoplay must keep video available muted');
assert.equal(sound.textContent, 'Enable sound');
vm.runInContext('rtcPc.ontrack({track:{kind:"video"}});', context);
await new Promise(setImmediate);
assert.deepEqual(Array.from(video.srcObject.tracks, track => track.kind), ['audio', 'video'],
  'An arriving track must not replace the other media track');
audioAllowed = true;
sound.handlers.click();
await new Promise(setImmediate);
assert.equal(video.muted, false, 'A user tap must enable sound');
assert.equal(sound.textContent, 'Mute sound');
sound.handlers.click();
await new Promise(setImmediate);
assert.equal(video.muted, true, 'The same control must mute sound');
vm.runInContext('useMjpeg();', context);
assert.equal(sound.hidden, true, 'The video-only fallback must not offer unavailable audio');
console.log('PASS: combined media, mobile autoplay recovery, sound control, fallback');
