<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Vibe Voice app logos

Generated LVGL 9.5 images of the four Supported Apps for the Vibe Voice Device
UI ([`docs/vibe-voice/firmware.md`](../../../docs/vibe-voice/firmware.md)): the
96 px logo in the Idle screen, the 20 px logo before the Target pill and at the
start of each root picker row.

| File | Content |
| --- | --- |
| `vv_icons.c` | 8 images, `LV_COLOR_FORMAT_RGB565A8` (little-endian RGB565 plane, then an 8-bit alpha plane): 96 × 96 and 20 × 20 of each app |
| `vv_icons.h` | `vv_icons_96[4]` and `vv_icons_20[4]`, indexed by Supported App: 0 Orca, 1 WeChat, 2 ChatGPT, 3 WeCom (the protocol's TARGET_STATE `app` and root row order) |

Flash cost: 115 392 bytes of image data (about 113 KiB): 27 648 bytes per 96 px
logo and 1 200 bytes per 20 px logo.

## Source and trademarks

The logos are the icons of the apps installed on the developer's Mac, read
from each app's `Info.plist` `CFBundleIconFile`:

| App | Icon file |
| --- | --- |
| Orca | `/Applications/Orca.app/Contents/Resources/icon.icns` |
| WeChat | `/Applications/WeChat.app/Contents/Resources/AppIcon.icns` |
| ChatGPT | `/Applications/ChatGPT.app/Contents/Resources/electron.icns` |
| WeCom | WeCom's `AppIcon.icns` in `/Applications` |

The logos are trademarks of their respective owners. They were extracted from
locally installed apps for personal use on this Device, only to show which app
receives the dictated text; no endorsement is implied. Do not redistribute
them separately or use them for anything else.

## Regenerate

Needs macOS (`sips`) with the four apps installed in `/Applications`; no
network and no third-party Python modules:

```bash
python3 tools/vibe_icons.py generate
python3 tools/vibe_icons.py check     # also part of ./tools/validate.sh --static
```

`generate` rasterizes each `.icns` with `sips` at both sizes, decodes the PNG
and writes `vv_icons.c` and `vv_icons.h`. App updates can change the icons;
regenerate and review the diff before committing.
