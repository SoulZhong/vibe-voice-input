#!/usr/bin/env python3
"""Generate and check the Vibe Voice LVGL fonts.

The Device shows arbitrary Chinese/English Partial Text and Segments, so the
16 px body font covers all of GB2312 (6763 hanzi plus its symbols), ASCII,
Latin-1, general punctuation, CJK punctuation, and fullwidth forms. The 24 px
title font and 48 px digit font only contain the glyphs the UI really draws.

    python3 tools/vibe_fonts.py generate [--src-dir DIR]   # needs network + npx
    python3 tools/vibe_fonts.py check                      # offline, part of --static

`generate` downloads the pinned Noto Sans SC sources (verified by SHA-256)
into build/vibe-fonts-src unless --src-dir already holds them, then runs the
pinned lv_font_conv. `check` parses the generated sources and verifies that
every string literal in main/vv_*.[ch] is covered by the font that draws it.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "assets" / "fonts" / "vibe-voice"
MAIN_DIR = ROOT / "main"
STRINGS_HEADER = MAIN_DIR / "vv_strings.h"

LV_FONT_CONV = "lv_font_conv@1.5.3"
NOTO_TAG = "Sans2.004"
NOTO_BASE = f"https://raw.githubusercontent.com/notofonts/noto-cjk/{NOTO_TAG}/Sans/SubsetOTF/SC"
SOURCES = {
    "NotoSansSC-Regular.otf": "faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9",
    "NotoSansSC-Medium.otf": "7633f5a016d4dd95e685a69633d818aabc4644c4b08e26bd35b1b30c45ed5dda",
}

BODY = "vv_font_body_16"
TITLE = "vv_font_title_24"
DIGITS = "vv_font_digits_48"
DIGIT_SYMBOLS = "0123456789 "

# Ranges added to GB2312 for the body font (inclusive).
BODY_EXTRA_RANGES = (
    (0x20, 0x7E),      # ASCII
    (0xA0, 0xFF),      # Latin-1 supplement (includes U+00B7 middle dot)
    (0x2010, 0x2027),  # dashes, quotes, ellipsis
    (0x2030, 0x203A),  # per mille, primes, angle quotes
    (0x3000, 0x301F),  # CJK punctuation (vertical marks U+302A+ skipped: they
                       # would inflate the line height to 31 px)
    (0xFF01, 0xFF5E),  # fullwidth ASCII forms
    (0xFFE0, 0xFFE6),  # fullwidth signs
)

GLYPH_COMMENT_RE = re.compile(r"/\* U\+([0-9A-Fa-f]{4,6}) ")
BITMAP_INDEX_RE = re.compile(r"\.bitmap_index = (\d+)")
STRING_RE = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
TITLE_DEFINE_RE = re.compile(r'^\s*#define\s+VV_H_\w+\s+"((?:[^"\\\n]|\\.)*)"', re.M)


def gb2312_codepoints() -> set[int]:
    points: set[int] = set()
    for high in range(0xA1, 0xF8):
        for low in range(0xA1, 0xFF):
            try:
                points.add(ord(bytes((high, low)).decode("gb2312")))
            except UnicodeDecodeError:
                continue
    return points


def strip_comments(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), source, flags=re.S)
    return re.sub(r"//[^\n]*", "", source)


def printable(text: str) -> set[int]:
    return {ord(ch) for ch in text if ch not in "\n\r\t"}


def ui_literals(paths: list[Path] | None = None) -> list[tuple[Path, int, str]]:
    """All string literals in the Vibe Voice application sources."""
    if paths is None:
        paths = sorted(MAIN_DIR.glob("vv_*.[ch]"))
    found = []
    for path in paths:
        text = strip_comments(path.read_text(encoding="utf-8"))
        for line_no, line in enumerate(text.splitlines(), 1):
            if line.lstrip().startswith("#include"):
                continue
            for match in STRING_RE.finditer(line):
                found.append((path, line_no, match.group(1)))
    return found


def title_literals(header: Path = STRINGS_HEADER) -> list[str]:
    return TITLE_DEFINE_RE.findall(header.read_text(encoding="utf-8"))


def body_codepoints(literals: list[tuple[Path, int, str]]) -> set[int]:
    points = gb2312_codepoints()
    for low, high in BODY_EXTRA_RANGES:
        points.update(range(low, high + 1))
    for _, _, text in literals:
        points.update(printable(text))
    return points


def title_codepoints(titles: list[str]) -> set[int]:
    points = set(range(0x20, 0x7F))
    for text in titles:
        points.update(printable(text))
    return points


def to_ranges(points: set[int]) -> str:
    ordered = sorted(points)
    parts = []
    start = prev = ordered[0]
    for point in ordered[1:]:
        if point == prev + 1:
            prev = point
            continue
        parts.append(f"0x{start:X}" if start == prev else f"0x{start:X}-0x{prev:X}")
        start = prev = point
    parts.append(f"0x{start:X}" if start == prev else f"0x{start:X}-0x{prev:X}")
    return ",".join(parts)


def covered(font_source: Path) -> set[int]:
    """Code points that lv_font_conv actually emitted into a generated font."""
    return {int(m, 16) for m in GLYPH_COMMENT_RE.findall(font_source.read_text(encoding="utf-8"))}


def fetch_sources(src_dir: Path) -> None:
    src_dir.mkdir(parents=True, exist_ok=True)
    for name, digest in SOURCES.items():
        path = src_dir / name
        if not path.exists():
            print(f"downloading {NOTO_BASE}/{name}")
            urllib.request.urlretrieve(f"{NOTO_BASE}/{name}", path)
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != digest:
            raise SystemExit(f"{path}: SHA-256 {actual} != pinned {digest}")


def conv(font: Path, size: int, name: str, ranges: str | None, symbols: str | None,
         fallback: str | None) -> None:
    command = [
        "npx", "-y", LV_FONT_CONV,
        "--font", str(font),
    ]
    if ranges:
        command += ["--range", ranges]
    if symbols:
        command += ["--symbols", symbols]
    command += [
        "--size", str(size), "--bpp", "4", "--format", "lvgl", "--no-compress",
        "--lv-include", "lvgl.h", "--lv-font-name", name,
        "--output", str(OUT_DIR / f"{name}.c"),
    ]
    if fallback:
        command += ["--lv-fallback", fallback]
    print(" ".join(command[:6]), f"... --size {size} --lv-font-name {name}")
    subprocess.run(command, check=True, cwd=ROOT)


def generate(src_dir: Path) -> int:
    fetch_sources(src_dir)
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    literals = ui_literals()
    titles = title_literals()
    regular = src_dir / "NotoSansSC-Regular.otf"
    medium = src_dir / "NotoSansSC-Medium.otf"
    conv(regular, 16, BODY, to_ranges(body_codepoints(literals)), None, None)
    conv(medium, 24, TITLE, to_ranges(title_codepoints(titles)), None, BODY)
    conv(medium, 48, DIGITS, None, DIGIT_SYMBOLS, TITLE)
    # Keep machine-specific download paths out of the committed headers.
    for name in (BODY, TITLE, DIGITS):
        path = OUT_DIR / f"{name}.c"
        text = path.read_text(encoding="utf-8").replace(f"{src_dir.resolve()}/", "<noto-src>/")
        text = text.replace(f"{src_dir}/", "<noto-src>/")
        path.write_text(text, encoding="utf-8")
    return check()


def missing(required: set[int], available: set[int]) -> list[str]:
    return [f"U+{point:04X}" for point in sorted(required - available)]


def check() -> int:
    errors: list[str] = []
    fonts = {name: OUT_DIR / f"{name}.c" for name in (BODY, TITLE, DIGITS)}
    for name, path in fonts.items():
        if not path.is_file():
            errors.append(f"missing generated font {path.relative_to(ROOT)}")
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1

    for name, path in fonts.items():
        # Without CONFIG_LV_FONT_FMT_TXT_LARGE the glyph bitmap offset is 20 bits.
        offsets = [int(v) for v in BITMAP_INDEX_RE.findall(path.read_text(encoding="utf-8"))]
        if offsets and max(offsets) >= 1 << 20:
            errors.append(f"{name}: bitmap exceeds 1 MiB; enable LV_FONT_FMT_TXT_LARGE")

    body = covered(fonts[BODY])
    title = covered(fonts[TITLE]) | body  # title falls back to the body font
    digits = covered(fonts[DIGITS])

    hanzi = {p for p in gb2312_codepoints() if 0x4E00 <= p <= 0x9FFF}
    gaps = missing(hanzi, body)
    if gaps:
        errors.append(f"{BODY}: {len(gaps)} GB2312 hanzi missing, e.g. {gaps[:8]}")
    for low, high in BODY_EXTRA_RANGES[:1]:
        gaps = missing(set(range(low, high + 1)), body)
        if gaps:
            errors.append(f"{BODY}: ASCII missing {gaps}")

    for path, line, text in ui_literals():
        gaps = missing(printable(text), body)
        if gaps:
            errors.append(f"{path.relative_to(ROOT)}:{line}: body font lacks {gaps} in \"{text}\"")
    for text in title_literals():
        gaps = missing(printable(text), title)
        if gaps:
            errors.append(f"VV_H_ \"{text}\": title font lacks {gaps}")
    gaps = missing(printable(DIGIT_SYMBOLS), digits)
    if gaps:
        errors.append(f"{DIGITS}: lacks {gaps}")

    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print(
        f"Vibe Voice fonts: PASS ({BODY} {len(body)} glyphs, "
        f"{TITLE} {len(covered(fonts[TITLE]))} glyphs, {DIGITS} {len(digits)} glyphs)"
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    gen = sub.add_parser("generate", help="download sources and regenerate fonts")
    gen.add_argument("--src-dir", type=Path, default=ROOT / "build" / "vibe-fonts-src")
    sub.add_parser("check", help="verify generated font coverage (offline)")
    args = parser.parse_args()
    if args.command == "generate":
        return generate(args.src_dir)
    return check()


if __name__ == "__main__":
    raise SystemExit(main())
