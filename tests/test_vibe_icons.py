#!/usr/bin/env python3
"""Unit tests for tools/vibe_icons.py (app logo generator and checker)."""

from __future__ import annotations

import importlib.util
import struct
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("vibe_icons", ROOT / "tools" / "vibe_icons.py")
vibe_icons = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(vibe_icons)


def png(width: int, height: int, rows: list[bytes], filters: list[int]) -> bytes:
    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    raw = b"".join(bytes([f]) + r for f, r in zip(filters, rows))
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw))
            + chunk(b"IEND", b""))


class VibeIconsTest(unittest.TestCase):
    def test_decode_png_filters(self) -> None:
        # Row 0 unfiltered, row 1 "up" filter (adds the row above).
        rows = [bytes([255, 0, 0, 255, 0, 255, 0, 128]), bytes([0, 0, 0, 0, 0, 0, 255, 0])]
        w, h, pixels = vibe_icons.decode_png(png(2, 2, rows, [0, 2]))
        self.assertEqual((w, h), (2, 2))
        self.assertEqual(pixels, [(255, 0, 0, 255), (0, 255, 0, 128),
                                  (255, 0, 0, 255), (0, 255, 255, 128)])
        # Sub filter (adds the pixel to the left) and Paeth.
        rows = [bytes([10, 20, 30, 40, 1, 1, 1, 1])]
        _, _, pixels = vibe_icons.decode_png(png(2, 1, rows, [1]))
        self.assertEqual(pixels, [(10, 20, 30, 40), (11, 21, 31, 41)])
        _, _, pixels = vibe_icons.decode_png(png(2, 1, rows, [4]))
        self.assertEqual(pixels, [(10, 20, 30, 40), (11, 21, 31, 41)])

    def test_rgb565a8_layout(self) -> None:
        data = vibe_icons.rgb565a8([(255, 255, 255, 255), (255, 0, 0, 0), (0, 0, 255, 7)])
        # Little-endian RGB565 plane, transparent pixels zeroed, then alpha.
        self.assertEqual(data, bytes([0xFF, 0xFF, 0x00, 0x00, 0x1F, 0x00, 255, 0, 7]))

    def test_supported_app_order(self) -> None:
        self.assertEqual([n for n, _ in vibe_icons.APPS], ["orca", "wechat", "chatgpt", "wecom"])
        images = {(n, s): bytes(s * s * 3) for n, _ in vibe_icons.APPS for s in vibe_icons.SIZES}
        header, source = vibe_icons.render(images)
        self.assertIn("vv_icons_96[VV_ICON_COUNT]", header)
        self.assertIn("vv_icons_20[VV_ICON_COUNT] = { &vv_icon_orca_20, &vv_icon_wechat_20, "
                      "&vv_icon_chatgpt_20, &vv_icon_wecom_20 };", source)
        self.assertIn(".header.cf = LV_COLOR_FORMAT_RGB565A8,", source)
        self.assertIn(".header.stride = 192,", source)

    def test_committed_icons_pass_check(self) -> None:
        self.assertEqual(vibe_icons.check(), 0)


if __name__ == "__main__":
    unittest.main()
