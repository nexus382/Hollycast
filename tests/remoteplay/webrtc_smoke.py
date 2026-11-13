#!/usr/bin/env python3
"""Decode real Remote Play video and stereo audio. Needs aiortc and websockets.

REMOTEPLAY_PIN=... python webrtc_smoke.py --url http://127.0.0.1:8765
The host must be running a game with Remote Play enabled. No PIN, cookie,
SDP, or tunnel credential is printed. Closing the WebSocket frees the slot.
"""
import argparse
from array import array
import asyncio
import json
import os
import time
import urllib.request

from aiortc import RTCConfiguration, RTCPeerConnection, RTCSessionDescription
from aiortc.mediastreams import MediaStreamError
from aiortc.sdp import candidate_from_sdp
import websockets


async def check(url, pin, player, minimum_frames, timeout):
    request = urllib.request.Request(
        url + '/api/login', data=json.dumps({'pin': pin, 'player': player}).encode(),
        headers={'Content-Type': 'application/json'})
    with urllib.request.urlopen(request, timeout=5) as response:
        cookie = response.headers['Set-Cookie'].split(';', 1)[0]
        assigned = json.load(response)['player']
    peer = RTCPeerConnection(RTCConfiguration(iceServers=[]))
    frames, size = 0, None
    audio_frames, audible_frames, audio_rate, audio_channels = 0, 0, 0, 0
    received = asyncio.Event()
    consumers = []
    decode_errors = []
    input_ready = asyncio.Event()
    pong = asyncio.Event()
    input_channel = None
    ping_start = 0
    rtts = []

    @peer.on('datachannel')
    def on_datachannel(channel):
        nonlocal input_channel
        if channel.label != 'input':
            return
        input_channel = channel
        if channel.readyState == 'open':
            input_ready.set()

        @channel.on('open')
        def on_open():
            input_ready.set()

        @channel.on('message')
        def on_message(raw):
            if json.loads(raw).get('t') == 'pong':
                rtts.append((time.monotonic() - ping_start) * 1000)
                pong.set()

    @peer.on('track')
    def on_track(track):
        async def consume():
            nonlocal frames, size, audio_frames, audible_frames, audio_rate, audio_channels
            try:
                while True:
                    frame = await track.recv()
                    if track.kind == 'video':
                        frames += 1
                        size = (frame.width, frame.height)
                    else:
                        audio_frames += 1
                        audio_rate = frame.sample_rate
                        audio_channels = len(frame.layout.channels)
                        assert frame.format.name in ('s16', 's16p'), f'Unexpected PCM format: {frame.format.name}'
                        pcm = array('h')
                        for plane in frame.planes:
                            samples = frame.samples * (1 if frame.format.is_planar else audio_channels)
                            pcm.frombytes(bytes(plane)[:samples * 2])
                        if max(map(abs, pcm), default=0) > 100:
                            audible_frames += 1
                    if frames >= minimum_frames and audio_frames >= 50 and audible_frames >= 5:
                        received.set()
            except MediaStreamError:
                return
            except Exception as error:
                decode_errors.append(f'{track.kind}: {type(error).__name__}: {error}')
                received.set()
        consumers.append(asyncio.create_task(consume()))

    websocket_url = url.replace('https://', 'wss://', 1).replace('http://', 'ws://', 1) + '/ws'
    try:
        async with websockets.connect(websocket_url, additional_headers={'Cookie': cookie}) as socket:
            async def signal():
                async for raw in socket:
                    message = json.loads(raw)
                    if message.get('t') != 'rtc':
                        continue
                    if message['kind'] == 'offer':
                        await peer.setRemoteDescription(RTCSessionDescription(message['sdp'], 'offer'))
                        await peer.setLocalDescription(await peer.createAnswer())
                        await socket.send(json.dumps({'t': 'rtc', 'kind': 'answer', 'sdp': peer.localDescription.sdp}))
                    elif message['kind'] == 'candidate':
                        candidate = candidate_from_sdp(message['candidate'])
                        candidate.sdpMid = message.get('mid', 'video')
                        await peer.addIceCandidate(candidate)
            signaling = asyncio.create_task(signal())
            waiting = asyncio.create_task(received.wait())
            try:
                done, _ = await asyncio.wait([signaling, waiting], timeout=timeout, return_when=asyncio.FIRST_COMPLETED)
                if signaling in done:
                    await signaling
                assert not decode_errors, '; '.join(decode_errors)
                assert received.is_set(), (f'Decoded {frames} video / {audio_frames} audio / {audible_frames} audible frames '
                                           f'in {timeout}s; peer={peer.connectionState}')
                assert size and min(size) >= 160, f'Unexpected video dimensions: {size}'
                assert audio_rate == 48000 and audio_channels == 2, f'Unexpected audio format: {audio_rate} Hz / {audio_channels} channels'
                await asyncio.wait_for(input_ready.wait(), 3)
                for _ in range(5):
                    pong.clear()
                    ping_start = time.monotonic()
                    input_channel.send(json.dumps({'t': 'ping'}))
                    await asyncio.wait_for(pong.wait(), 3)
                print(f'PASS: player {assigned + 1}, {frames} H.264 frames, {size[0]}x{size[1]}, '
                      f'{audio_frames} Opus frames / {audible_frames} audible, stereo {audio_rate} Hz, '
                      f'direct input RTT {sum(rtts) / len(rtts):.1f} ms average / {max(rtts):.1f} ms max')
            finally:
                signaling.cancel()
                waiting.cancel()
                await asyncio.gather(signaling, waiting, return_exceptions=True)
    finally:
        await peer.close()
        for task in consumers:
            task.cancel()
        await asyncio.gather(*consumers, return_exceptions=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', default='http://127.0.0.1:8765')
    parser.add_argument('--player', type=int, choices=range(4), default=-1, help='Zero-based controller port; default first available')
    parser.add_argument('--frames', type=int, default=60)
    parser.add_argument('--timeout', type=float, default=15)
    args = parser.parse_args()
    pin = os.environ.get('REMOTEPLAY_PIN')
    if not pin:
        parser.error('Set REMOTEPLAY_PIN to the PIN shown by Hollycast')
    asyncio.run(check(args.url.rstrip('/'), pin, args.player, args.frames, args.timeout))
