// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
namespace phone_vmu {
inline constexpr char vmuLink[] = R"VMULINK(
'use strict';
class PhoneVmuLink {
  constructor(core, report, applyInput) {
    this.core = core;
    this.report = report;
    this.applyInput = applyInput;
    this.state = 'closed';
    this.session = null;
    this.signalRelease = null;
    this.client = Array.from(crypto.getRandomValues(new Uint8Array(16)),
      byte => byte.toString(16).padStart(2, '0')).join('');
  }

  async start(room, offerer) {
    if (this.session) this.close();
    if (!/^[0-9a-f]{32}$/.test(room)) {
      this.report('Invalid VMU link code.');
      return;
    }
    if (typeof RTCPeerConnection !== 'function') {
      this.report('This browser does not support VMU links.');
      return;
    }
    const session = {
      abort: new AbortController(),
      headers: {'X-VMU-Link': room, 'X-VMU-Client': this.client},
      postedSignal: null, channel: null, clock: null
    };
    this.session = session;
    this.state = 'waiting';
    session.timeout = setTimeout(() => this.fail(session, 'VMU link timed out. Create a fresh invite and try again.'), 120000);
    this.report('Waiting for the other VMU…');
    try {
      // Let an earlier session release its signaling claim before retrying.
      await this.signalRelease;
      if (this.session !== session) return;
      const peer = session.peer = new RTCPeerConnection({iceServers: []});
      peer.onconnectionstatechange = () => {
        if (['failed', 'disconnected', 'closed'].includes(peer.connectionState))
          this.fail(session, 'VMU link disconnected.');
      };
      peer.ondatachannel = ({channel}) => {
        if (offerer || session.channel || channel.label !== 'vmu' || !channel.ordered ||
            channel.maxRetransmits !== null || channel.maxPacketLifeTime !== null) {
          channel.close();
          this.fail(session, 'Invalid VMU link channel.');
          return;
        }
        this.attachChannel(session, channel);
      };
      if (offerer) {
        this.attachChannel(session, peer.createDataChannel('vmu', {ordered: true}));
        const offer = await this.localSdp(session, await peer.createOffer());
        if (this.session !== session) return;
        session.postedSignal = 'offer';
        await this.signal(session, 'offer', offer);
        const answer = await this.waitForSdp(session, 'answer');
        if (this.session !== session) return;
        await peer.setRemoteDescription({type: 'answer', sdp: answer});
      } else {
        const offer = await this.waitForSdp(session, 'offer');
        if (this.session !== session) return;
        await peer.setRemoteDescription({type: 'offer', sdp: offer});
        const answer = await this.localSdp(session, await peer.createAnswer());
        if (this.session !== session) return;
        session.postedSignal = 'answer';
        await this.signal(session, 'answer', answer);
      }
    } catch (error) {
      this.fail(session, error.message || 'Could not connect the VMUs.');
    }
  }

  async localSdp(session, description) {
    const {peer, abort} = session;
    await peer.setLocalDescription(description);
    if (abort.signal.aborted) throw abort.signal.reason;
    // Exchange one complete SDP per peer, including the local ICE candidates.
    if (peer.iceGatheringState !== 'complete') {
      await new Promise((resolve, reject) => {
        const cleanup = () => {
          peer.removeEventListener('icegatheringstatechange', onGathering);
          abort.signal.removeEventListener('abort', onAbort);
        };
        const onGathering = () => {
          if (peer.iceGatheringState !== 'complete') return;
          cleanup();
          resolve();
        };
        const onAbort = () => {
          cleanup();
          reject(abort.signal.reason);
        };
        peer.addEventListener('icegatheringstatechange', onGathering);
        abort.signal.addEventListener('abort', onAbort, {once: true});
      });
    }
    return peer.localDescription.sdp;
  }

  async signal(session, path, sdp) {
    const response = await fetch('link/' + path, {
      method: sdp === undefined ? 'GET' : 'POST',
      headers: {...session.headers, ...(sdp === undefined ? {} : {'Content-Type': 'application/octet-stream'})},
      body: sdp,
      cache: 'no-store',
      signal: session.abort.signal
    });
    if (sdp === undefined && response.status === 404) return null;
    if (response.status === 409) {
      throw new Error(path === 'offer' && sdp !== undefined
        ? 'Another VMU invite is open. Disconnect that link before creating a new one.'
        : 'This VMU invite is already in use or has closed. Ask for a fresh invite.');
    }
    if (!response.ok) throw new Error('VMU link signaling failed (HTTP ' + response.status + ').');
    if (sdp === undefined) return response.text();
  }

  async waitForSdp(session, path) {
    for (;;) {
      const sdp = await this.signal(session, path);
      if (sdp !== null) return sdp;
      if (session.abort.signal.aborted) throw session.abort.signal.reason;
      await new Promise(resolve => {
        session.resumePolling = resolve;
        session.poll = setTimeout(resolve, 500);
      });
      session.resumePolling = null;
      if (session.abort.signal.aborted) throw session.abort.signal.reason;
    }
  }

  attachChannel(session, channel) {
    if (this.session !== session) {
      channel.close();
      return;
    }
    session.channel = channel;
    channel.onopen = () => {
      if (this.session !== session) return;
      clearTimeout(session.timeout);
      session.clock = new PhoneVmuLinkClock(this.core, this.applyInput,
        message => channel.send(JSON.stringify(message)));
      this.state = 'linked';
      if (session.postedSignal === 'offer') this.releaseSignal(session);
      this.report('VMUs linked.');
    };
    channel.onclose = () => this.fail(session, 'VMU link disconnected.');
    channel.onerror = () => this.fail(session, 'VMU link disconnected.');
    channel.onmessage = ({data}) => {
      if (this.session !== session) return;
      try {
        if (!session.clock || typeof data !== 'string' || data.length > 100)
          throw new Error('Invalid VMU link message.');
        session.clock.receive(JSON.parse(data));
      } catch (error) {
        this.fail(session, error.message || 'VMU link disconnected.');
      }
    };
  }

  tick(time) {
    const session = this.session;
    if (!session?.clock) return;
    try {
      session.clock.tick(time);
    } catch (error) {
      this.fail(session, error.message || 'VMU link disconnected.');
    }
  }

  releaseSignal(session) {
    const path = session.postedSignal;
    if (!path) return;
    session.postedSignal = null;
    const abort = new AbortController();
    const timeout = setTimeout(() => abort.abort(), 5000);
    this.signalRelease = fetch('link/' + path, {
      method: 'DELETE', headers: session.headers, signal: abort.signal, keepalive: true
    })
      .catch(() => {})
      .finally(() => clearTimeout(timeout));
  }

  fail(session, message) {
    if (this.session === session) this.close(message);
  }

  close(message = 'VMU link closed.') {
    const session = this.session;
    if (!session) return;
    this.session = null;
    this.state = 'closed';
    clearTimeout(session.timeout);
    clearTimeout(session.poll);
    session.abort.abort();
    session.resumePolling?.();
    if (session.clock) this.core.connectSerial(null);
    session.channel?.close();
    session.peer?.close();
    this.releaseSignal(session);
    this.report(message);
  }
}

class PhoneVmuLinkClock {
  constructor(core, applyInput, send) {
    this.core = core;
    this.applyInput = applyInput;
    this.send = send;
    this.round = 0;
    this.time = 0;
    this.window = 2 * core.linkTicksPerSlice;
    this.end = this.window;
    this.budget = 0;
    this.lastFrame = null;
    this.local = null;
    this.remote = null;
    this.awaitingReply = false;
    core.connectSerial(byte => { this.sentByte = byte; }, true);
    core.alignLinkTimer();
  }

  tick(time) {
    if (this.lastFrame === null) this.lastFrame = time;
    this.budget = Math.min(100, this.budget + Math.max(0, time - this.lastFrame));
    this.lastFrame = time;
    if (this.local && time - this.proposedAt > 5000)
      throw new Error('The other VMU stopped advancing its clock.');
    this.propose();
  }

  propose() {
    if (this.local || this.budget < 20) return;
    if (this.time === this.end - this.window) this.applyInput();
    this.local = this.core.probeSerial(this.end);
    this.proposedAt = this.lastFrame;
    this.send([0, this.round, this.local.time, this.local.byte]);
    this.commit();
  }

  receive(message) {
    const isByte = value => Number.isInteger(value) && value >= 0 && value <= 255;
    if (!Array.isArray(message) || message[1] !== this.round) throw new Error('VMU link clock lost synchronization.');
    if (message[0] === 1 && message.length === 3 && this.awaitingReply && isByte(message[2])) {
      this.core.completeSerial(message[2]);
      this.finishRound();
      return;
    }
    const [kind, , time, byte] = message;
    if (kind !== 0 || message.length !== 4 || this.remote || !Number.isSafeInteger(time)
        || time <= this.time || time > this.end || (byte === null ? time !== this.end : !isByte(byte)))
      throw new Error('Invalid VMU clock proposal.');
    this.remote = { time, byte };
    this.commit();
  }

  commit() {
    if (!this.local || !this.remote) return;
    const time = Math.min(this.local.time, this.remote.time);
    const localByte = this.local.time === time ? this.local.byte : null;
    const remoteByte = this.remote.time === time ? this.remote.byte : null;
    // Both peers independently propose their next output using a private
    // checkpoint. Commit only up to the first wire event on either side.
    this.sentByte = null;
    this.core.runSerialUntil(time);
    if (this.sentByte !== localByte) throw new Error('VMU clock prediction did not match execution.');
    this.time = time;
    this.awaitingReply = localByte !== null;
    if (remoteByte !== null)
      this.send([1, this.round, this.core.receiveSerialByte(remoteByte)]);
    if (!this.awaitingReply) this.finishRound();
  }

  finishRound() {
    ++this.round;
    this.local = this.remote = null;
    this.awaitingReply = false;
    if (this.time === this.end) {
      this.end += this.window;
      this.budget -= 20;
    }
    this.propose();
  }
}
)VMULINK";
} // namespace phone_vmu
