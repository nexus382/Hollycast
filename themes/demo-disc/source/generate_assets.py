#!/usr/bin/env python3
"""Generate the Demo Disc artwork and manifest. Requires Pillow."""
import json
import math
import random
import struct
import wave
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets"
INK = "0b0d12"
ORANGE = "ff4f00"
SILVER = "c9ced6"
SCALE = 2  # Rasterize decorations at twice the design size.


def rgb(value):
    return tuple(int(value[i:i + 2], 16) for i in (0, 2, 4))


def font(size, weight=900):
    return ImageFont.truetype(str(ASSETS / f"Orbitron-{weight}.ttf"), size)


def slanted(text, size, fill, slant=0.2, weight=900):
    """Draw text sheared like an italic: the font ships upright only."""
    face = font(size, weight)
    left, top, right, bottom = face.getbbox(text)
    pad = int(size * 0.3)
    width = right - left + pad * 2 + int(slant * (bottom + pad))
    height = bottom - top + pad * 2
    layer = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    ImageDraw.Draw(layer).text((pad - left, pad - top), text, font=face, fill=fill)
    return layer.transform(layer.size, Image.AFFINE, (1, slant, -slant * height, 0, 1, 0), Image.BICUBIC)


def background():
    width, height = 1920, 1080
    # A brushed-metal sheen with a hard diagonal glint just past the middle.
    small = Image.new("L", (480, 270))
    pixels = small.load()
    for y in range(270):
        for x in range(480):
            t = (x / 479 * 0.8 + y / 269 * 0.2)
            pixels[x, y] = int(232 - 46 * t / 0.48) if t < 0.5 else int(226 - 58 * (t - 0.5) / 0.5)
    sheen = small.resize((width, height), Image.BICUBIC)
    image = Image.merge("RGB", (sheen, sheen.point(lambda v: min(255, v + 2)), sheen.point(lambda v: min(255, v + 6))))
    draw = ImageDraw.Draw(image, "RGBA")
    step = 60
    for x in range(0, width, step):
        draw.line((x, 0, x, height), fill=(10, 14, 22, 24))
    for y in range(0, height, step):
        draw.line((0, y, width, y), fill=(10, 14, 22, 24))
    image.save(ASSETS / "background.png", optimize=True)


def disc():
    size = 1024
    colors = ["f4f6f8", "9fe6ff", "f7b6ec", "ffe59a", "b7f7c0", "f4f6f8", "9fb6ff", "ffc2a1"]
    stops = [rgb(c) for c in colors] + [rgb(colors[0])]
    image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    pixels = image.load()
    center = (size - 1) / 2
    for y in range(size):
        for x in range(size):
            dx, dy = x - center, y - center
            radius = math.hypot(dx, dy) / center
            if radius > 1:
                continue
            t = (math.atan2(dy, dx) / (2 * math.pi) + 0.5) * (len(stops) - 1)
            index = min(int(t), len(stops) - 2)
            mix = t - index
            base = [stops[index][i] * (1 - mix) + stops[index + 1][i] * mix for i in range(3)]
            if radius < 0.12:
                base = rgb("e9ecef")
            elif radius < 0.13:
                base = rgb("7c8591")
            elif radius < 0.27:
                base = [v * 0.5 + 255 * 0.5 for v in base]
            alpha = min(255, int((1 - radius) * center * 2 * 255)) if radius > 0.995 else 255
            pixels[x, y] = (int(base[0]), int(base[1]), int(base[2]), alpha)
    draw = ImageDraw.Draw(image)
    edge = (size - 1) * 0.5
    draw.ellipse((edge - edge, edge - edge, edge + edge, edge + edge), outline=(124, 133, 145, 255), width=3)
    image.save(ASSETS / "disc.png", optimize=True)


def chamfered(width, height, cut, fill):
    image = Image.new("RGBA", (width * SCALE, height * SCALE), (0, 0, 0, 0))
    w, h, c = width * SCALE, height * SCALE, cut * SCALE
    ImageDraw.Draw(image).polygon([(0, 0), (w - c, 0), (w, c), (w, h), (c, h), (0, h - c)], fill=fill)
    return image


def screen():
    chamfered(392, 392, 26, rgb(INK) + (255,)).save(ASSETS / "screen-frame.png", optimize=True)
    lines = Image.new("RGBA", (368, 368), (0, 0, 0, 0))
    draw = ImageDraw.Draw(lines)
    for y in range(0, 368, 3):
        draw.line((0, y, 368, y), fill=(0, 0, 0, 40))
    lines.save(ASSETS / "scanlines.png", optimize=True)
    sweep = Image.new("RGBA", (8, 96), (255, 255, 255, 0))
    for y in range(96):
        strength = max(0.0, 1 - abs(y - 47.5) / 47.5)
        for x in range(8):
            sweep.putpixel((x, y), (255, 255, 255, int(70 * strength)))
    sweep.save(ASSETS / "sweep.png", optimize=True)
    bright = Image.new("RGBA", (8, 128), (255, 255, 255, 0))
    for y in range(128):
        strength = max(0.0, 1 - abs(y - 63.5) / 63.5)
        for x in range(8):
            bright.putpixel((x, y), (255, 255, 255, int(235 * strength ** 0.7)))
    bright.save(ASSETS / "sweep-launch.png", optimize=True)


def play_button():
    width, height, cut = 230 * SCALE, 68 * SCALE, 22 * SCALE
    for name, fill in (("play-button", ORANGE), ("play-button-hover", "d94300")):
        image = Image.new("RGBA", (width, height), (0, 0, 0, 0))
        ImageDraw.Draw(image).polygon([(0, 0), (width, 0), (width - cut, height), (0, height)], fill=rgb(fill) + (255,))
        label = slanted("PLAY", 30 * SCALE, (255, 255, 255, 255))
        image.alpha_composite(label, ((width - cut - label.width) // 2, (height - label.height) // 2))
        image.save(ASSETS / f"{name}.png", optimize=True)


def star_points(cx, cy, radius):
    return [(cx + math.cos(-math.pi / 2 + i * math.pi / 5) * (radius if i % 2 == 0 else radius * 0.44),
             cy + math.sin(-math.pi / 2 + i * math.pi / 5) * (radius if i % 2 == 0 else radius * 0.44)) for i in range(10)]


def favourite_button():
    """Interlocks with the Play button's slant. Every state keeps a strong text contrast."""
    width, height, cut, border = 186 * SCALE, 68 * SCALE, 22 * SCALE, 2 * SCALE
    states = (
        ("favourite", "FAVOURITE", "eef0f3", INK, INK, False),
        ("favourite-hover", "FAVOURITE", "ffffff", ORANGE, INK, False),
        ("favourited", "FAVOURITED", INK, INK, "ffffff", True),
        ("favourited-hover", "FAVOURITED", "2b303b", ORANGE, "ffffff", True),
    )
    shape = [(cut, 0), (width, 0), (width - cut, height), (0, height)]
    inner = [(cut + border * 1.3, border), (width - border * 1.3, border), (width - cut - border * 0.6, height - border), (border * 1.3, height - border)]
    for name, text, fill, edge, ink, solid in states:
        # Drawn large and reduced, so the star and the slanted edges come out smooth.
        image = Image.new("RGBA", (width * 3, height * 3), (0, 0, 0, 0))
        draw = ImageDraw.Draw(image)
        draw.polygon([(x * 3, y * 3) for x, y in shape], fill=rgb(edge) + (255,))
        draw.polygon([(x * 3, y * 3) for x, y in inner], fill=rgb(fill) + (255,))
        star = [(x * 3, y * 3) for x, y in star_points(36 * SCALE, height / 2, 11 * SCALE)]
        if solid:
            draw.polygon(star, fill=rgb(ORANGE) + (255,))
        else:
            draw.polygon(star, outline=rgb(ORANGE) + (255,), width=4 * SCALE * 1)
        image = image.resize((width, height), Image.LANCZOS)
        face = font(13 * SCALE, 900)
        ImageDraw.Draw(image).text((52 * SCALE, height / 2), text, font=face, fill=rgb(ink) + (255,), anchor="lm")
        image.save(ASSETS / f"{name}.png", optimize=True)


def logo():
    white = slanted("HOLLYCAST", 56 * SCALE, rgb(INK) + (255,), 0.22)
    slash = slanted("/", 56 * SCALE, rgb(ORANGE) + (255,), 0.22)
    white, slash = white.crop(white.getbbox()), slash.crop(slash.getbbox())
    gap = 10 * SCALE
    image = Image.new("RGBA", (white.width + gap + slash.width, max(white.height, slash.height)), (0, 0, 0, 0))
    image.alpha_composite(white, (0, 0))
    image.alpha_composite(slash, (white.width + gap, 0))
    image.save(ASSETS / "logo.png", optimize=True)
    # A short mark, so that the portrait toolbar fits on one line.
    letter = slanted("H", 56 * SCALE, rgb(INK) + (255,), 0.22)
    letter = letter.crop(letter.getbbox())
    mark = Image.new("RGBA", (letter.width + gap + slash.width, max(letter.height, slash.height)), (0, 0, 0, 0))
    mark.alpha_composite(letter, (0, 0))
    mark.alpha_composite(slash, (letter.width + gap, 0))
    mark.save(ASSETS / "logo-mark.png", optimize=True)


def lamps():
    for name, fill, glow in (("lamp-on", ORANGE, True), ("lamp-off", "4d545f", False)):
        size = 96
        image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        if glow:
            halo = Image.new("RGBA", (size, size), (0, 0, 0, 0))
            ImageDraw.Draw(halo).ellipse((22, 22, 74, 74), fill=rgb(fill) + (190,))
            image.alpha_composite(halo.filter(ImageFilter.GaussianBlur(10)))
        ImageDraw.Draw(image).ellipse((28, 28, 68, 68), fill=rgb(fill) + (255,))
        image.save(ASSETS / f"{name}.png", optimize=True)


def placeholder():
    image = Image.new("RGB", (512, 512), rgb("15181f"))
    draw = ImageDraw.Draw(image)
    draw.rectangle((22, 22, 490, 490), outline=rgb("4d545f"), width=4)
    label = slanted("NO PREVIEW", 40, (159, 168, 180, 255), 0.2, 700)
    image.paste(label, ((512 - label.width) // 2, (512 - label.height) // 2), label)
    image.save(ASSETS / "placeholder.png", optimize=True)


def move_sound():
    """A short whoosh: noise through a filter that sweeps up and back, panned past the
    listener. Synthesized here so the package owns it outright; the seed keeps it stable."""
    rate = 44100
    length = int(rate * 0.22)
    noise = random.Random(42)
    low = band = dark = 0.0
    samples = []
    for i in range(length):
        t = i / length
        cutoff = 240 + 1500 * math.sin(math.pi * min(1.0, t / 0.8)) ** 2
        f = 2 * math.sin(math.pi * cutoff / rate)
        high = noise.uniform(-1, 1) - low - 0.55 * band
        band += f * high
        low += f * band
        dark += (band - dark) * min(1.0, 2 * math.pi * cutoff * 1.6 / rate)  # Takes off the hiss.
        rise = min(1.0, t / 0.38)
        fall = min(1.0, (1 - t) / 0.62)
        samples.append((dark, math.sin(rise * math.pi / 2) ** 2 * math.sin(fall * math.pi / 2) ** 2, t))
    peak = max(abs(value * envelope) for value, envelope, _ in samples)
    frames = bytearray()
    for value, envelope, t in samples:
        level = value * envelope / peak * 0.3 * 32767
        pan = 0.5 + 0.35 * (2 * t - 1)
        frames += struct.pack("<hh", int(level * math.cos(pan * math.pi / 2) * 1.2), int(level * math.sin(pan * math.pi / 2) * 1.2))
    with wave.open(str(ASSETS / "move.wav"), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(bytes(frames))


def launch_sound():
    """A menu confirm chime, a chord swelling up an octave as the disc spins up, and a sparkle at the flash."""
    rate = 44100
    length = int(rate * 1.05)
    left = [0.0] * length
    right = [0.0] * length

    def bell(start, frequency, level, pan, decay=7.0):
        # Two-operator FM: a bright attack that mellows as it rings, plus one echo.
        for offset, gain in ((0.0, 1.0), (0.13, 0.32)):
            first = int((start + offset) * rate)
            for i in range(first, min(length, first + int(rate * 0.6))):
                t = (i - first) / rate
                envelope = math.exp(-t * decay) * min(1.0, t / 0.002)
                index = 1.6 * math.exp(-t * 16)
                value = math.sin(2 * math.pi * frequency * t + index * math.sin(2 * math.pi * frequency * 2 * t))
                value *= level * gain * envelope
                left[i] += value * (1 - pan)
                right[i] += value * (1 + pan)

    bell(0.0, 659.25, 0.5, -0.25)
    bell(0.075, 987.77, 0.55, 0.25)
    bell(0.70, 1318.51, 0.2, -0.3, 9.0)
    bell(0.74, 1975.53, 0.16, 0.3, 9.0)

    # The swell: a soft E major chord, detuned in pairs, sliding up an octave with a flutter that speeds up like a disc.
    chord = [(164.81, 0.0), (246.94, -0.4), (329.63, 0.4), (415.30, -0.2), (493.88, 0.2)]
    phases = [[0.0, 0.0] for _ in chord]
    flutter = 0.0
    for i in range(length):
        t = i / length
        rise = 2 ** (t ** 2.4)
        flutter += 2 * math.pi * (5 + 38 * t ** 2) / rate
        level = 0.17 * t ** 1.5 * (0.72 + 0.28 * math.sin(flutter))
        for note, (frequency, pan) in enumerate(chord):
            for voice, detune in enumerate((0.996, 1.004)):
                phases[note][voice] += 2 * math.pi * frequency * detune * rise / rate
                phase = phases[note][voice]
                value = level * (math.sin(phase) + 0.22 * math.sin(phase * 2) + 0.1 * math.sin(phase * 3))
                left[i] += value * (1 - pan) * (1.15 if voice == 0 else 0.85)
                right[i] += value * (1 + pan) * (0.85 if voice == 0 else 1.15)

    # Air rushing past: band-limited noise, different on each side, opening with the swell.
    for channel, seed in ((left, 7), (right, 11)):
        noise = random.Random(seed)
        low = band = 0.0
        for i in range(length):
            t = i / length
            f = 2 * math.sin(math.pi * (350 + 6500 * t ** 2) / rate)
            high = noise.uniform(-1, 1) - low - 1.1 * band
            band += f * high
            low += f * band
            channel[i] += band * 0.5 * t ** 2.2

    frames = bytearray()
    shaped = []
    for i in range(length):
        release = min(1.0, (length - i) / (rate * 0.05))
        shaped.append((math.tanh(left[i] * 1.2) * release, math.tanh(right[i] * 1.2) * release))
    peak = max(max(abs(a), abs(b)) for a, b in shaped)
    for a, b in shaped:
        frames += struct.pack("<hh", int(a / peak * 0.3 * 32767), int(b / peak * 0.3 * 32767))
    with wave.open(str(ASSETS / "launch.wav"), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(bytes(frames))


def manifest():
    def box(width, height):
        return lambda x, y, w, h: [round(x / width, 4), round(y / height, 4), round(w / width, 4), round(h / height, 4)]

    colors = {
        "Text": INK, "TextDisabled": "4d545f", "WindowBg": SILVER, "ChildBg": "d7dbe1", "PopupBg": "ffffff",
        "Border": "6e7682", "Separator": "8d95a1", "FrameBg": "e4e7eb", "FrameBgHovered": "f3f4f6", "FrameBgActive": "ffffff",
        "Button": "e4e7eb", "ButtonHovered": "ffffff", "ButtonActive": ORANGE,
        "Header": "ffb48f", "HeaderHovered": "ffc9ad", "HeaderActive": "ff9a66",
        "CheckMark": ORANGE, "SliderGrab": ORANGE, "SliderGrabActive": "c93d00", "NavCursor": ORANGE,
        "ScrollbarBg": "00000000", "ScrollbarGrab": "6e768299", "ScrollbarGrabHovered": "4d545f", "ScrollbarGrabActive": INK,
        "TitleBg": "b9bfc8", "TitleBgActive": "a7aeb9", "TableHeaderBg": "b9bfc8", "TextLink": "b83500",
    }
    rail = {
        "visible": 4, "tilt": 0.024, "skew": 0.03, "falloff": 0.045, "fade": 0.15, "rowColor": INK, "favouriteMark": True,
        "badges": [["system"], ["genre", "region", "format"]],
    }
    shared = {
        "mode": "rail", "activation": "doubleClick", "toolbar": "compact", "toolbarSelectedTextColor": "ffffff",
        "toolbarLogo": "assets/logo.png", "toolbarLogoHeight": 36, "hoverDetails": False, "showItemActions": False,
        "textColor": "eef1f4", "selectedTextColor": "ffffff", "selectorColor": ORANGE, "background": SILVER,
        "attract": {"idleSeconds": 20, "intervalSeconds": 3.5}, "rail": rail, "sounds": {"move": "assets/move.wav", "launch": "assets/launch.wav"}, "launch": {"seconds": 1.05},
    }
    ticker_fields = ["description", "publisher", "developer", "genre", "players", "releaseDate", "system", "region", "productId"]
    detail_fields = ["system", "developer", "publisher", "genre", "players", "releaseDate", "region", "productId"]

    def picture(rect, source, z, **extra):
        return {"type": "image", "rect": rect, "source": source, "zIndex": z, **extra}

    def label(rect, text, size, z=1, **extra):
        return {"type": "text", "rect": rect, "text": text, "fontSize": size, "color": extra.pop("color", "3a414d"), "zIndex": z, **extra}

    def value(rect, source, size, fallback="-", **extra):
        return {"type": "metadata", "rect": rect, "source": source, "text": fallback, "fontSize": size, "color": extra.pop("color", INK), "zIndex": 1, **extra}

    def play(rect):
        return {"type": "button", "rect": rect, "action": "activate", "text": "", "zIndex": 3,
                "image": "assets/play-button.png", "hoverImage": "assets/play-button-hover.png"}

    def favourite(rect):
        return {"type": "button", "rect": rect, "action": "favourite", "text": "", "zIndex": 3,
                "image": "assets/favourite.png", "hoverImage": "assets/favourite-hover.png",
                "selectedImage": "assets/favourited.png", "selectedHoverImage": "assets/favourited-hover.png"}

    def joined(rect, fields, size, z=1, **extra):
        return {"type": "metadata", "rect": rect, "fields": fields, "fontSize": size, "color": extra.pop("color", INK), "zIndex": z, **extra}

    # Landscape: designed at 1280x720.
    r = box(1280, 720)
    landscape = {
        "id": "demo-disc", "name": "Demo Disc", "canvasAspect": 1.7778, "browser": r(22, 8, 584, 640), "rowHeight": 0.0861, "fontSize": 0.0278,
        "narrowLayout": "demo-disc-portrait", "narrowAspect": 1.1, **shared,
        "elements": [
            picture([0, 0, 1, 1], "assets/background.png", -100, fit="cover", bleed=True),
            picture([0.5, 0, 0.5, 1], "assets/disc.png", -90, bleed=True, spin={"seconds": 14, "launchSeconds": 0.07, "center": [0.875, 0.1], "radius": 0.232}),
            {"type": "artwork", "rect": [0.5, 0, 0.5, 1], "source": "disc", "fallback": "none", "zIndex": -89, "bleed": True, "spin": {"seconds": 14, "launchSeconds": 0.07, "center": [0.875, 0.1], "radius": 0.232, "hole": 0.13}},
            picture(r(626, 42, 392, 392), "assets/screen-frame.png", -80, fit="stretch"),
            {"type": "artwork", "rect": r(638, 54, 368, 368), "source": "cover", "fit": "contain", "fallbackImage": "assets/placeholder.png", "zIndex": -70},
            picture(r(638, 54, 368, 368), "assets/scanlines.png", -60, fit="stretch"),
            picture(r(638, 54, 368, 368), "assets/sweep.png", -59, sweepSeconds=3.2),
            picture(r(638, 54, 368, 368), "assets/sweep-launch.png", -58, sweepSeconds=0.5, when="launching"),
            picture(r(626, 15, 18, 18), "assets/lamp-on.png", 1, when="attract"),
            picture(r(626, 15, 18, 18), "assets/lamp-off.png", 1, when="interactive"),
            label(r(650, 16, 240, 20), "ATTRACT MODE", 0.0195, color=INK),
            value(r(626, 442, 640, 50), "title", 0.052, slant=0.2),
            {"type": "details", "rect": r(626, 492, 630, 112), "fields": detail_fields, "columns": 2, "fontSize": 0.0222, "color": INK, "text": "No details known for this game.", "zIndex": 1},
            play(r(626, 606, 230, 68)),
            favourite(r(838, 606, 186, 68)),
            label(r(1036, 604, 106, 18), "PLAY TIME", 0.0167),
            value(r(1036, 622, 106, 24), "playTime", 0.0236, fallback="Not yet"),
            label(r(1146, 604, 122, 18), "LAST RUN", 0.0167),
            value(r(1146, 622, 122, 50), "lastPlayed", 0.0236, fallback="Never"),
            {"type": "panel", "rect": r(0, 688, 1280, 32), "color": INK, "zIndex": 4, "bleed": True},
            joined(r(0, 694, 1280, 24), ticker_fields, 0.0222, 5, color="ff9b6b", separator="   *   ", marqueeSeconds=9, bleed=True),
            {"type": "panel", "rect": [0, 0, 1, 1], "color": "ffffff", "zIndex": 90, "bleed": True, "when": "launching", "launchFadeFrom": 0.68},
        ],
    }
    # Portrait: designed at 620x912, for narrow windows and phones. The cover and details share
    # the top, so that the rail, which is what the player browses, gets all the remaining height.
    p = box(620, 912)
    portrait = {
        "id": "demo-disc-portrait", "name": "Demo Disc (portrait)", "canvasAspect": 0.68, "browser": p(12, 396, 596, 472), "rowHeight": 0.0548, "fontSize": 0.0225,
        **shared, "toolbarLogo": "assets/logo-mark.png", "toolbarLogoHeight": 30, "rail": {**rail, "visible": 6, "center": 0.2, "tilt": 0.014, "falloff": 0.026, "fade": 0.11},
        "elements": [
            picture([0, 0, 1, 1], "assets/background.png", -100, fit="cover", bleed=True),
            picture([0.4, 0, 0.6, 0.4], "assets/disc.png", -90, bleed=True, spin={"seconds": 14, "launchSeconds": 0.07, "center": [1.02, 0.02], "radius": 0.28}),
            {"type": "artwork", "rect": [0.4, 0, 0.6, 0.4], "source": "disc", "fallback": "none", "zIndex": -89, "bleed": True, "spin": {"seconds": 14, "launchSeconds": 0.07, "center": [1.02, 0.02], "radius": 0.28, "hole": 0.13}},
            picture(p(18, 32, 284, 284), "assets/screen-frame.png", -80, fit="stretch"),
            {"type": "artwork", "rect": p(27, 41, 266, 266), "source": "cover", "fit": "contain", "fallbackImage": "assets/placeholder.png", "zIndex": -70},
            picture(p(27, 41, 266, 266), "assets/scanlines.png", -60, fit="stretch"),
            picture(p(27, 41, 266, 266), "assets/sweep.png", -59, sweepSeconds=3.2),
            picture(p(27, 41, 266, 266), "assets/sweep-launch.png", -58, sweepSeconds=0.5, when="launching"),
            picture(p(18, 8, 18, 18), "assets/lamp-on.png", 1, when="attract"),
            picture(p(18, 8, 18, 18), "assets/lamp-off.png", 1, when="interactive"),
            label(p(42, 10, 240, 20), "ATTRACT MODE", 0.0164, color=INK),
            value(p(316, 34, 290, 62), "title", 0.0307, slant=0.16),
            {"type": "details", "rect": p(316, 98, 290, 222), "fields": detail_fields, "columns": 1, "fontSize": 0.0175, "color": INK, "text": "No details known for this game.", "zIndex": 1},
            play(p(18, 326, 189, 56)),
            favourite(p(204, 326, 153, 56)),
            label(p(372, 326, 112, 18), "PLAY TIME", 0.0137),
            value(p(372, 344, 112, 38), "playTime", 0.0197, fallback="Not yet"),
            label(p(488, 326, 120, 18), "LAST RUN", 0.0137),
            value(p(488, 344, 120, 44), "lastPlayed", 0.0197, fallback="Never"),
            {"type": "panel", "rect": p(0, 878, 620, 34), "color": INK, "zIndex": 4, "bleed": True},
            joined(p(0, 885, 620, 24), ticker_fields, 0.0197, 5, color="ff9b6b", separator="   *   ", marqueeSeconds=7, bleed=True),
            {"type": "panel", "rect": [0, 0, 1, 1], "color": "ffffff", "zIndex": 90, "bleed": True, "when": "launching", "launchFadeFrom": 0.68},
        ],
    }
    theme = {
        "formatVersion": 1, "id": "demo-disc", "name": "Demo Disc", "author": "Hollycast contributors",
        "description": "Brushed silver and loud type: a tilted game rail tagged from each game's details, a spinning disc, and an attract mode that rolls through your library.",
        "baseTheme": 1, "colors": colors, "fonts": {"body": "assets/Orbitron-700.ttf", "bold": "assets/Orbitron-900.ttf"},
        "layouts": [landscape, portrait],
    }
    (ROOT / "theme.json").write_text(json.dumps(theme, indent=2) + "\n")


if __name__ == "__main__":
    background()
    disc()
    screen()
    play_button()
    favourite_button()
    logo()
    lamps()
    placeholder()
    move_sound()
    launch_sound()
    manifest()
