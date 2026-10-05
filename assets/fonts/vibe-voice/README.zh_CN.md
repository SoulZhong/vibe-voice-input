<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# Vibe Voice 字库

Vibe Voice 设备界面使用的 LVGL 9.5 位图字库
（[`docs/vibe-voice/firmware.zh_CN.md`](../../../docs/vibe-voice/firmware.zh_CN.md)）。
临时文本（Partial Text）和片段（Segment）是任意中英文，所以正文字库覆盖整个
GB2312 字符集，而不是只覆盖固定界面文字。

| 文件 | 字号 / 位深 | 来源字重 | 覆盖范围 | Flash |
| --- | --- | --- | --- | --- |
| `vv_font_body_16.c` | 16 px，4 bpp，不压缩 | Noto Sans SC Regular | 完整 GB2312（6763 个汉字及其符号、假名、希腊/西里尔字母、制表符），ASCII `0x20-0x7E`，Latin-1 `0xA0-0xFF`，通用标点 `0x2010-0x2027` 与 `0x2030-0x203A`，CJK 标点 `0x3000-0x301F`，全角字符 `0xFF01-0xFF5E` 与 `0xFFE0-0xFFE6`：共 7648 个字形，行高 21 px | 约 958 KiB |
| `vv_font_title_24.c` | 24 px，4 bpp | Noto Sans SC Medium | ASCII 加 `main/vv_strings.h` 中所有 `VV_H_*` 标题用到的字（140 个字形）；缺字回退到正文字库 | 约 23 KiB |
| `vv_font_digits_48.c` | 48 px，4 bpp | Noto Sans SC Medium | `0-9` 与空格，用于配对码；回退到标题字库 | 约 4.4 KiB |

回退链：数字 48 → 标题 24 → 正文 16。正文字库之外的码位（例如 emoji、GB2312
之外的生僻字、U+302A-U+303F 竖排符号）显示为 LVGL 占位框
（`CONFIG_LV_USE_FONT_PLACEHOLDER=y`），不会被静默丢弃。来自 Companion 的非法
UTF-8 在显示前替换为 `?`。

## 来源与许可

- 字体：[Noto Sans CJK](https://github.com/notofonts/noto-cjk)，标签 `Sans2.004`，
  `Sans/SubsetOTF/SC/NotoSansSC-Regular.otf`（SHA-256
  `faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9`）和
  `NotoSansSC-Medium.otf`（SHA-256
  `7633f5a016d4dd95e685a69633d818aabc4644c4b08e26bd35b1b30c45ed5dda`）。
- 许可：SIL Open Font License 1.1，全文见 [`OFL.txt`](OFL.txt)。生成的位图属于该字体
  软件的衍生作品，以同一许可分发。源 OTF 文件不提交。
- 转换器：[`lv_font_conv`](https://github.com/lvgl/lv_font_conv) 1.5.3，通过 `npx` 运行。

## 重新生成

需要 Node.js（`npx`）并联网一次；源字体下载到 `build/vibe-fonts-src/`，并按固定的
SHA-256 校验。

```bash
python3 tools/vibe_fonts.py generate
python3 tools/vibe_fonts.py check
```

`generate` 生成精确的码位列表（用 Python 的 `gb2312` 编解码器枚举 GB2312，加上
上表的额外范围），并对每个文件执行：

```text
npx -y lv_font_conv@1.5.3 --font <noto-src>/NotoSansSC-Regular.otf --range <正文范围> \
  --size 16 --bpp 4 --format lvgl --no-compress --lv-include lvgl.h \
  --lv-font-name vv_font_body_16 --output assets/fonts/vibe-voice/vv_font_body_16.c
npx -y lv_font_conv@1.5.3 --font <noto-src>/NotoSansSC-Medium.otf --range <ASCII + VV_H_* 用字> \
  --size 24 --bpp 4 --format lvgl --no-compress --lv-include lvgl.h \
  --lv-font-name vv_font_title_24 --output assets/fonts/vibe-voice/vv_font_title_24.c \
  --lv-fallback vv_font_body_16
npx -y lv_font_conv@1.5.3 --font <noto-src>/NotoSansSC-Medium.otf --symbols "0123456789 " \
  --size 48 --bpp 4 --format lvgl --no-compress --lv-include lvgl.h \
  --lv-font-name vv_font_digits_48 --output assets/fonts/vibe-voice/vv_font_digits_48.c \
  --lv-fallback vv_font_title_24
```

完整的范围字符串记录在每个生成文件头部的 `Opts:` 中。新增或修改 `VV_H_*` 标题后
需重新生成。

## 覆盖检查

`python3 tools/vibe_fonts.py check` 是 `./tools/validate.sh --static` 的一部分。它读取
每个生成文件中实际输出的字形，以下情况失败：

- 正文字库缺少任一 GB2312 汉字或可打印 ASCII 字符；
- `main/vv_*.c` / `main/vv_*.h` 中任一字符串字面量含有正文字库没有的字符；
- 任一 `VV_H_*` 标题含有标题字库（及其回退）没有的字符；
- 缺少配对码数字，或位图超过 LVGL 非 large 字库格式的 1 MiB 偏移上限。

`tests/test_vibe_fonts.py` 测试检查器本身，包括一个已知缺字的反例（U+9F98）。

## 集成方式

`main/CMakeLists.txt` 用 `target_sources` 编译这三个文件。`main/vv_ui.c` 显式为控件
设置字体（`LV_FONT_DECLARE` 加 `lv_obj_set_style_text_font`）。常量位图位于 Flash，
不占用堆。
