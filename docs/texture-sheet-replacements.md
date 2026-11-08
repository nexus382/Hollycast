<!-- Portions Copyright 2026 The Hollycast Authors -->
# Texture sheet replacements

Enable **Advanced → Texture Dumping → Split Texture Sheets** for a title that stores multiple textures in one image. It is off by default and can be saved as a per-game setting. Enable **Texture Dumping** in the same section while collecting textures, then play through the areas you want to edit.

The title's normal dump folder receives:

- The original `<hash>.png` sheets.
- Individual `atlas_<hash>_<x>_<y>_<width>_<height>.png` pieces.
- `texture-atlas.json`, which records each sheet's hash, original dimensions and crop rectangles.

Coordinates in the mapping and filenames are measured from the top-left of the dumped PNG. Keep filenames unchanged. The mapping stays beside the images; no separate installation location is needed.

Upscale or edit the individual pieces, then copy **the mapping, original sheets and pieces** into that title's usual replacement folder (`data/textures/<game id>`). Keep the original sheets at their dumped dimensions. All supplied pieces from a particular sheet must use exactly the same scale in both dimensions. Different sheets can use different scales. For example, at 4×, a 32×32 piece must be 128×128 and a 64×32 piece must be 256×128.

Enable **Load Custom Textures** and **Split Texture Sheets**, then start the title. Hollycast rebuilds each sheet in memory before uploading it, including when preloading textures. Missing pieces and areas outside the extracted rectangles retain the original sheet's pixels, resized with nearest-neighbor sampling to the selected scale. Mismatched scales, changed aspect ratios, invalid mappings and renderer size limits cause the sheet replacement to be rejected with a notification and details in the log. Scales must place all supplied crop boundaries on whole output pixels; 2× and 4× always satisfy this requirement.

Extraction matches complementary triangles covering rectangular UV regions before renderer UV adjustments, including triangles submitted as separate strips or in different frames. Boundaries must align to texels or texel centers; arbitrary camera-clipping intersections are rejected. It skips full-sheet and nearly full-sheet use, wrapping outside the sheet, irregular regions, overlapping crops, second texture layers and Naomi 2 transformed geometry. These areas remain available in the original sheet. Captured regions reflect submitted geometry, including geometry hidden behind other surfaces. Only visited areas can contribute new pieces. A complete face discovered later can replace its contained partial cut in the mapping; previous PNGs are left untouched. Finish collecting the dump before editing or copying its mapping into a pack.

This is an experimental extraction aid, not a guarantee of recovering a game's original authored textures. Confirm the cuts and results in game before distributing a pack. With the toggle off, normal texture dumping and replacement loading continue to use complete images.
