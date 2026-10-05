<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Vibe Voice fonts

Generated LVGL 9.5 bitmap fonts for the Vibe Voice Device UI
([`docs/vibe-voice/firmware.md`](../../../docs/vibe-voice/firmware.md)).
Partial Text and Segments are arbitrary Chinese and English, so the body font
covers the whole GB2312 set instead of only the fixed UI strings.

| File | Size / depth | Source face | Coverage | Flash |
| --- | --- | --- | --- | --- |
| `vv_font_body_16.c` | 16 px, 4 bpp, uncompressed | Noto Sans SC Regular | All of GB2312 (6763 hanzi + its symbols, kana, Greek, Cyrillic, box drawing), ASCII `0x20-0x7E`, Latin-1 `0xA0-0xFF`, general punctuation `0x2010-0x2027` and `0x2030-0x203A`, CJK punctuation `0x3000-0x301F`, fullwidth forms `0xFF01-0xFF5E` and `0xFFE0-0xFFE6`: 7648 glyphs, line height 21 px | ~958 KiB |
| `vv_font_title_24.c` | 24 px, 4 bpp | Noto Sans SC Medium | ASCII plus the characters of every `VV_H_*` title in `main/vv_strings.h` (140 glyphs); falls back to the body font | ~23 KiB |
| `vv_font_digits_48.c` | 48 px, 4 bpp | Noto Sans SC Medium | `0-9` and space, for the pairing passkey; falls back to the title font | ~4.4 KiB |

Fallback chain: digits 48 → title 24 → body 16. A code point outside the body
font (for example an emoji, a rare hanzi outside GB2312, or U+302A-U+303F
vertical marks) is drawn as LVGL's placeholder box
(`CONFIG_LV_USE_FONT_PLACEHOLDER=y`); it is never silently dropped. Invalid
UTF-8 from the Companion is replaced with `?` before display.

## Source and license

- Font: [Noto Sans CJK](https://github.com/notofonts/noto-cjk), tag `Sans2.004`,
  `Sans/SubsetOTF/SC/NotoSansSC-Regular.otf` (SHA-256
  `faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9`) and
  `NotoSansSC-Medium.otf` (SHA-256
  `7633f5a016d4dd95e685a69633d818aabc4644c4b08e26bd35b1b30c45ed5dda`).
- License: SIL Open Font License 1.1, copied in [`OFL.txt`](OFL.txt). The
  generated bitmaps are a derivative of the font software and are distributed
  under the same license. The source OTF files are not committed.
- Converter: [`lv_font_conv`](https://github.com/lvgl/lv_font_conv) 1.5.3, run
  through `npx`.

## Regenerate

Needs Node.js (for `npx`) and network access once; the sources are downloaded
to `build/vibe-fonts-src/` and verified against the pinned SHA-256 values.

```bash
python3 tools/vibe_fonts.py generate
python3 tools/vibe_fonts.py check
```

`generate` builds the exact range list (GB2312 decoded by Python's `gb2312`
codec plus the extra ranges above) and runs, for each file:

```text
npx -y lv_font_conv@1.5.3 --font <noto-src>/NotoSansSC-Regular.otf --range <body ranges> \
  --size 16 --bpp 4 --format lvgl --no-compress --lv-include lvgl.h \
  --lv-font-name vv_font_body_16 --output assets/fonts/vibe-voice/vv_font_body_16.c
npx -y lv_font_conv@1.5.3 --font <noto-src>/NotoSansSC-Medium.otf --range <ASCII + VV_H_* chars> \
  --size 24 --bpp 4 --format lvgl --no-compress --lv-include lvgl.h \
  --lv-font-name vv_font_title_24 --output assets/fonts/vibe-voice/vv_font_title_24.c \
  --lv-fallback vv_font_body_16
npx -y lv_font_conv@1.5.3 --font <noto-src>/NotoSansSC-Medium.otf --symbols "0123456789 " \
  --size 48 --bpp 4 --format lvgl --no-compress --lv-include lvgl.h \
  --lv-font-name vv_font_digits_48 --output assets/fonts/vibe-voice/vv_font_digits_48.c \
  --lv-fallback vv_font_title_24
```

The full range strings are recorded in the `Opts:` header of each generated
file. Regenerate after adding or changing a `VV_H_*` title.

## Coverage check

`python3 tools/vibe_fonts.py check` runs in `./tools/validate.sh --static`. It
reads the glyphs actually emitted into each generated file and fails when:

- any GB2312 hanzi or printable ASCII character is missing from the body font;
- any string literal in `main/vv_*.c` / `main/vv_*.h` has a character the body
  font lacks;
- any `VV_H_*` title has a character the title font (or its fallback) lacks;
- the passkey digits are missing, or a bitmap grows past the 1 MiB offset limit
  of LVGL's non-large font format.

`tests/test_vibe_fonts.py` covers the checker itself, including a known-missing
negative case (U+9F98).

## Integration

`main/CMakeLists.txt` adds the three sources with `target_sources`. Widgets set
the font explicitly in `main/vv_ui.c` (`LV_FONT_DECLARE` plus
`lv_obj_set_style_text_font`). The constant bitmaps live in Flash; they need no
heap.
