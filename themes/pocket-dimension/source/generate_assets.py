#!/usr/bin/env python3
"""Build Pocket Dimension's local supporting assets and manifest (Pillow required).

The imagegen device skin is a retained source asset and is never regenerated here.
"""
# Copyright 2026 The Hollycast Authors
# Portions Copyright 2026 The Hollycast Authors
# SPDX-License-Identifier: GPL-2.0-or-later
from pathlib import Path
import json
import random
from PIL import Image, ImageDraw, ImageFilter

PACKAGE = Path(__file__).resolve().parents[1]
ASSETS = PACKAGE / "assets"
ASSETS.mkdir(exist_ok=True)
DEVICE_WIDTH, DEVICE_HEIGHT = 1024, 1536


def paper():
    image = Image.new("RGB", (1600, 1000))
    randomizer = random.Random(27)
    image.putdata([(238 + n, 237 + n, 229 + n)
                   for n in (randomizer.choice((-1, 0, 0, 0, 1)) for _ in range(1600000))])
    image.save(ASSETS / "paper.png", optimize=True)
    frame = Image.new("RGBA", (800, 500))
    shadow = Image.new("RGBA", frame.size)
    ImageDraw.Draw(shadow).rectangle((11, 24, 790, 490), fill="#46543a35")
    frame.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(7)))
    draw = ImageDraw.Draw(frame)
    draw.rectangle((8, 18, 786, 479), fill="#f9f8ef")
    tape = Image.new("RGBA", (114, 33), "#b7c3a77a").rotate(5, expand=True)
    frame.alpha_composite(tape, (450, 2))
    frame.save(ASSETS / "photo-frame.png", optimize=True)
    # A recessed display with a molded rim and a quiet lower hardware rail.
    display = Image.new("RGBA", (1400, 840))
    shadow = Image.new("RGBA", display.size)
    ImageDraw.Draw(shadow).rounded_rectangle((21, 28, 1380, 825), radius=25, fill="#26392d55")
    display.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(10)))
    draw = ImageDraw.Draw(display)
    draw.rounded_rectangle((16, 12, 1383, 812), radius=24, fill="#71816b", outline="#a9b59b", width=2)
    draw.rounded_rectangle((21, 18, 1378, 807), radius=21, fill="#4d604f")
    draw.rounded_rectangle((37, 34, 1362, 771), radius=14, fill="#17271f", outline="#83917b", width=2)
    draw.rounded_rectangle((42, 39, 1357, 766), radius=10, fill="#20332b")
    draw.line((49, 781, 1350, 781), fill="#87947c66", width=1)
    for x in (64, 76, 88):
        draw.ellipse((x, 792, x + 4, 796), fill="#26392d")
    draw.rounded_rectangle((1298, 791, 1334, 797), radius=3, fill="#b1c39a")
    display.save(ASSETS / "display-frame.png", optimize=True)
    empty = Image.new("RGBA", (512, 320), "#20332b")
    draw = ImageDraw.Draw(empty)
    draw.rounded_rectangle((146, 44, 366, 264), radius=12, outline="#667f68", width=3)
    draw.ellipse((177, 75, 335, 233), outline="#a1b38e", width=4)
    draw.ellipse((238, 136, 274, 172), outline="#a1b38e", width=3)
    draw.line((204, 91, 227, 124), fill="#667f68", width=3)
    draw.line((285, 184, 308, 217), fill="#667f68", width=3)
    empty.save(ASSETS / "artwork-placeholder.png", optimize=True)
    # A local, theme-authored fallback for BIOS entries and games without covers.
    fallback = Image.new("RGBA", (24, 24), "#ffffff")
    draw = ImageDraw.Draw(fallback)
    draw.rectangle((5, 5, 18, 18), outline="#000000", width=2)
    draw.rectangle((9, 9, 14, 14), fill="#000000")
    for offset in (7, 11, 15):
        draw.rectangle((offset, 2, offset + 1, 4), fill="#000000")
        draw.rectangle((offset, 19, offset + 1, 21), fill="#000000")
        draw.rectangle((2, offset, 4, offset + 1), fill="#000000")
        draw.rectangle((19, offset, 21, offset + 1), fill="#000000")
    fallback.save(ASSETS / "lcd-placeholder.png", optimize=True)


def element(kind, rect, **values):
    return dict(type=kind, rect=[round(n, 6) for n in rect], **values)


def text(rect, value, size=0.021, color="64775c"):
    return element("text", rect, text=value, fontSize=size, color=color)


def metadata(rect, source, size=0.021, color="3c503f", fallback="", **values):
    return element("metadata", rect, source=source, text=fallback, fontSize=size, color=color, **values)


def details(rect, columns=2, size=0.018):
    return element("details", rect, columns=columns, fontSize=size, color="334238",
                   fields=["developer", "publisher", "genre", "players", "releaseDate", "region",
                           "format", "productId", "playTime", "lastPlayed", "size", "description"])


def button(rect, action, value="", source=None, background="00000000", **appearance):
    result = element("button", rect, action=action, text=value, color=appearance.pop("color", "334238"),
                     background=background, fontSize=0.024, **appearance)
    if source:
        result["source"] = source
    return result


def layout(layout_id, name, image_rect, canvas_aspect):
    x, y, w, h = image_rect

    def region(rect):
        rx, ry, rw, rh = rect
        return [x + w * rx / DEVICE_WIDTH, y + h * ry / DEVICE_HEIGHT,
                w * rw / DEVICE_WIDTH, h * rh / DEVICE_HEIGHT]

    elements = [
        element("image", [0, 0, 1, 1], source="assets/paper.png", fit="stretch", zIndex=-10),
        element("image", image_rect, source="assets/device-polished-v1.png", fit="stretch", zIndex=-2),
        text(region([226, 389, 570, 35]), "GAME SELECT", 0.019, "26392d"),
        element("panel", region([226, 430, 570, 4]), color="455d3e"),
        button(region([165, 1128, 95, 95]), "previous", rounding=0.12),
        button(region([322, 1128, 95, 95]), "next", rounding=0.12),
        button(region([243, 1050, 95, 95]), "up", rounding=0.12),
        button(region([243, 1208, 95, 95]), "down", rounding=0.12),
        button(region([728, 1070, 145, 145]), "artwork", source="cover", rounding=0.5),
        button(region([578, 1164, 145, 145]), "artwork", source="screenshot", rounding=0.5),
    ]
    return dict(id=layout_id, name=name, mode="grid", browser=[round(v, 6) for v in region([225, 446, 573, 409])],
                columns=3, itemAspect=1.06, spacing=6, minItemWidth=42, showTitles=True,
                fontSize=0.019, titleStyle="initials", textColor="26392d", selectedTextColor="26392d",
                selectorColor="26392d", selectorStyle="outline", background="eeede5",
                canvasAspect=canvas_aspect, toolbar="compact", toolbarSelectedTextColor="f5f4e9", activation="doubleClick",
                toolbarLogo="assets/hollycast-logo.png", toolbarLogoHeight=50,
                showItemActions=False, showFavouriteMarker=True, hoverDetails=False, previewSource="cover", artworkSource="cover",
                pixelArt=dict(resolution=128, levels=32, dark="26392d", light="a1b88f"),
                placeholder="assets/lcd-placeholder.png", elements=elements)


def artwork_tabs(y, height, x, widths, font_size):
    appearance = dict(selectedColor="f5f4e9", selectedBackground="60745a", rounding=0.5)
    tabs = []
    for source, label, width in zip(("cover", "screenshot", "miximage", "physical"),
                                    ("Cover", "Screenshot", "Mix", "Disc"), widths):
        tab = button([x, y, width, height], "artwork", label, source=source, **appearance)
        tab["fontSize"] = font_size
        tabs.append(tab)
        x += width
    return tabs


def favourite_button(rect, size):
    result = button(rect, "favourite", "Favourite", selectedText="Favourited",
                    icon="star", selectedIcon="star-filled", color="445c42",
                    background="e3e8da", borderColor="91a189", selectedBorderColor="526b4e",
                    selectedColor="f5f4e9", selectedBackground="526b4e", rounding=0.5)
    result["fontSize"] = size
    return result


def manifest():
    wide = layout("pocket", "Pocket library", [0.005, 0, 0.4, 0.99], 1.65)
    wide.update(narrowLayout="pocket-portrait", narrowAspect=1.15)
    wide["elements"] += [
        text([0.45, 0.055, 0.36, 0.035], "Selected game", 0.019),
        element("panel", [0.89, 0.05, 0.085, 0.038], color="dce2d2"),
        metadata([0.89, 0.058, 0.085, 0.027], "format", 0.017, fallback="Game", align="center"),
        metadata([0.45, 0.108, 0.525, 0.11], "title", 0.057, "26392d"),
        element("image", [0.44, 0.227, 0.54, 0.46], source="assets/display-frame.png", fit="stretch"),
        element("artwork", [0.457, 0.25, 0.506, 0.393], source="preview", fit="cover", color="b1c29b36", zIndex=1),
        element("artwork", [0.47, 0.259, 0.48, 0.376], source="preview", fit="contain",
                fallbackImage="assets/artwork-placeholder.png", zIndex=2),
        details([0.45, 0.712, 0.525, 0.151]),
        element("panel", [0.45, 0.873, 0.525, 0.0015], color="c5cbbb"),
        button([0.86, 0.896, 0.115, 0.077], "activate", "Play", background="526b4e", color="f7f5ea", rounding=0.3),
        favourite_button([0.71, 0.909, 0.14, 0.052], 0.02),
        text([0.04, 0.964, 0.365, 0.031], "< > Browse   A Cover   B Screenshot", 0.019),
    ]
    wide["elements"] += artwork_tabs(0.909, 0.052, 0.45, [0.06, 0.097, 0.043, 0.05], 0.017)
    portrait = layout("pocket-portrait", "Pocket library (portrait)", [0.19, 0, 0.62, 0.5115], 0.55)
    portrait["fontSize"] = 0.016
    portrait["minItemWidth"] = 32
    portrait["elements"][2]["fontSize"] = 0.016
    portrait["elements"] += [
        metadata([0.07, 0.52, 0.86, 0.058], "title", 0.027, "26392d"),
        element("image", [0.06, 0.584, 0.88, 0.191], source="assets/display-frame.png", fit="stretch"),
        element("artwork", [0.085, 0.596, 0.83, 0.159], source="preview", fit="cover", color="b1c29b36", zIndex=1),
        element("artwork", [0.105, 0.601, 0.79, 0.149], source="preview", fit="contain",
                fallbackImage="assets/artwork-placeholder.png", zIndex=2),
        details([0.07, 0.79, 0.86, 0.12], columns=1, size=0.015),
        button([0.7, 0.951, 0.23, 0.041], "activate", "Play", background="526b4e", color="f7f5ea", rounding=0.3),
        favourite_button([0.07, 0.951, 0.6, 0.041], 0.016),
    ]
    portrait["elements"] += artwork_tabs(0.916, 0.029, 0.07, [0.18, 0.32, 0.18, 0.18], 0.014)
    colors = dict(Text="334238", TextDisabled="67785f", WindowBg="eeede5", ChildBg="eeede5",
                  PopupBg="f4f2e6", Border="bcc6af", Separator="c7cbb9", FrameBg="e1e5d5",
                  FrameBgHovered="d3dcc7", FrameBgActive="c3d0b4", Button="eeede5",
                  ButtonHovered="d0dac3", ButtonActive="60745a", Header="cad6bf",
                  HeaderHovered="bccdaf", HeaderActive="a6bc97", CheckMark="536c47",
                  SliderGrab="68815b", SliderGrabActive="455e39", NavCursor="455e39",
                  ScrollbarBg="9eae9100", ScrollbarGrab="71886699", ScrollbarGrabHovered="536e48",
                  ScrollbarGrabActive="354e2d", TitleBg="dce2d0", TitleBgActive="cad6bf",
                  TableHeaderBg="d5dfca", TextLink="3e6434")
    return dict(formatVersion=1, id="pocket-dimension", name="Pocket Dimension",
                author="Hollycast contributors",
                description="A tactile VMU library with LCD artwork, physical browse buttons, and a photo of the selected game.",
                baseTheme=1, colors=colors,
                fonts=dict(body="assets/SpaceMono-Regular.ttf", bold="assets/SpaceMono-Regular.ttf"),
                layouts=[wide, portrait])


if __name__ == "__main__":
    paper()
    (PACKAGE / "theme.json").write_text(json.dumps(manifest(), indent=2) + "\n", encoding="utf-8")
    print("Rebuilt Pocket Dimension supporting artwork and layouts.")
