#!/usr/bin/env python3
"""Unit tests for tools/vibe_fonts.py (font coverage checker)."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("vibe_fonts", ROOT / "tools" / "vibe_fonts.py")
vibe_fonts = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(vibe_fonts)


class VibeFontsTest(unittest.TestCase):
    def test_gb2312_inventory(self) -> None:
        points = vibe_fonts.gb2312_codepoints()
        hanzi = [p for p in points if 0x4E00 <= p <= 0x9FFF]
        self.assertEqual(len(hanzi), 6763)
        self.assertIn(0x4E2D, points)        # 中
        self.assertIn(0x25B2, points)        # ▲ used in hints
        self.assertNotIn(0x9F98, points)     # 龘 is outside GB2312

    def test_ranges_round_trip(self) -> None:
        self.assertEqual(vibe_fonts.to_ranges({1, 2, 3, 7, 9, 10}), "0x1-0x3,0x7,0x9-0xA")

    def test_literals_skip_comments_and_includes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "vv_x.c"
            source.write_text(
                '#include "lvgl.h"\n'
                '// "注释" is not drawn\n'
                '/* "块注释" */ const char *a = "听写中";\n'
                'const char *b = "a\\"b";\n',
                encoding="utf-8",
            )
            texts = [text for _, _, text in vibe_fonts.ui_literals([source])]
        self.assertEqual(texts, ["听写中", 'a\\"b'])

    def test_title_defines(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            header = Path(tmp) / "vv_strings.h"
            header.write_text(
                '#define VV_H_A "就绪"\n#define VV_T_B "正文"\n', encoding="utf-8"
            )
            self.assertEqual(vibe_fonts.title_literals(header), ["就绪"])

    def test_coverage_detects_missing_glyph(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            font = Path(tmp) / "font.c"
            font.write_text('/* U+4E2D "中" */\n/* U+0041 "A" */\n', encoding="utf-8")
            available = vibe_fonts.covered(font)
        self.assertEqual(available, {0x4E2D, 0x41})
        self.assertEqual(vibe_fonts.missing(vibe_fonts.printable("中A"), available), [])
        # Known-missing negative case: the check cannot pass unconditionally.
        self.assertEqual(vibe_fonts.missing(vibe_fonts.printable("中龘"), available), ["U+9F98"])

    def test_generated_body_font_covers_ui(self) -> None:
        body = vibe_fonts.covered(vibe_fonts.OUT_DIR / f"{vibe_fonts.BODY}.c")
        self.assertNotIn(0x9F98, body)
        for _, _, text in vibe_fonts.ui_literals():
            self.assertEqual(vibe_fonts.missing(vibe_fonts.printable(text), body), [], text)


if __name__ == "__main__":
    unittest.main()
