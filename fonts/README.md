# SolarOS Font Generation

This directory holds source font faces and offline tooling for generating
SolarOS bitmap fonts. The firmware should consume generated U8g2 C arrays, not
runtime TTF rendering.

The generator expects these exact source face filenames in this directory:

- `Regular.ttf`
- `Italic.ttf`
- `Bold.ttf`
- `BoldItalic.ttf`

Build the vendored `bdfconv` tool:

```sh
make -C fonts/tools/bdfconv
```

Generate the default U8g2 C arrays, BDF files, and preview image:

```sh
python3 fonts/generate_u8g2_fonts.py
```

The output is written below `fonts/build/`:

- `bdf/` - monochrome fixed-cell BDF files
- `u8g2/` - generated U8g2 C arrays
- `preview/default_preview.png` - enlarged bitmap preview
- `manifest.json` - generated metrics and filenames

Default generated C arrays are committed to the repository:

- `fonts/build/u8g2/u8g2_font_solar_os_default_*_tf.c`

Terminal text size `8` uses the public-domain U8g2 `5x8` bitmap face in
`components/u8g2/src/fonts/u8g2_font_5x8_tf.c`, with a six-pixel cell advance.
That fixed face is independent of the TTF generator and retains its source and
copyright notice in the vendored file.

The firmware automatically compiles those arrays when
`fonts/build/u8g2/u8g2_font_solar_os_default_r_14_tf.c` exists. Intermediate
BDF files, previews, manifests, Python caches, and non-default generated
experiments are ignored.

To replace the default font set:

1. Replace the four TTF files, keeping the exact filenames listed above.
2. Regenerate the arrays:

```sh
make -C fonts/tools/bdfconv
python3 fonts/generate_u8g2_fonts.py
```

3. Inspect `fonts/build/preview/default_preview.png`.
4. Build and test the shell, terminal text sizes, and graphical document rendering on the device.
5. Commit the replacement TTF files and generated
   `fonts/build/u8g2/u8g2_font_solar_os_default_*_tf.c` files.

An external `bdfconv` can still be used explicitly:

```sh
python3 fonts/generate_u8g2_fonts.py --bdfconv /path/to/bdfconv
```

Defaults:

- Sizes: `10, 12, 14, 16, 18, 20`
- Styles: regular, bold, italic, bold italic
- Glyphs: ASCII plus Latin-1 supplement, `0x20-0x7e` and `0xa0-0xff`
- Rendering: monochrome FreeType output, no antialiasing

Useful options:

```sh
python3 fonts/generate_u8g2_fonts.py --sizes 14,18
python3 fonts/generate_u8g2_fonts.py --glyph-ranges 0x20-0x7e
python3 fonts/generate_u8g2_fonts.py --antialias --threshold 160
python3 fonts/generate_u8g2_fonts.py --no-u8g2
```
