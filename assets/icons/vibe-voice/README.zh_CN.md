<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# Vibe Voice 应用图标

为 Vibe Voice 设备界面（[`docs/vibe-voice/firmware.zh_CN.md`](../../../docs/vibe-voice/firmware.zh_CN.md)）生成的四个支持的应用的 LVGL 9.5 图片：空闲界面的 96 px 图标，以及目标标签前和选择页根列表每行开头的 20 px 图标。

| 文件 | 内容 |
| --- | --- |
| `vv_icons.c` | 8 张图片，`LV_COLOR_FORMAT_RGB565A8`（先是小端 RGB565 平面，再是 8 位 alpha 平面）：每个应用 96 × 96 和 20 × 20 各一张 |
| `vv_icons.h` | `vv_icons_96[4]` 和 `vv_icons_20[4]`，按支持的应用编号索引：0 Orca，1 微信，2 ChatGPT，3 企业微信（即协议中 TARGET_STATE 的 `app` 和根列表行序） |

Flash 占用：图片数据 115 392 字节（约 113 KiB）：每个 96 px 图标 27 648 字节，每个 20 px 图标 1 200 字节。

## 来源与商标

图标取自开发者 Mac 上已安装应用的图标，按各应用 `Info.plist` 中的 `CFBundleIconFile` 读取：

| 应用 | 图标文件 |
| --- | --- |
| Orca | `/Applications/Orca.app/Contents/Resources/icon.icns` |
| 微信 | `/Applications/WeChat.app/Contents/Resources/AppIcon.icns` |
| ChatGPT | `/Applications/ChatGPT.app/Contents/Resources/electron.icns` |
| 企业微信 | `/Applications/企业微信.app/Contents/Resources/AppIcon.icns` |

这些图标是其各自所有者的商标，仅为在本设备上个人使用、显示听写文字将发往哪个应用而从本机已安装的应用中提取，不代表任何认可。请勿单独再分发或另作他用。

## 重新生成

需要 macOS（`sips`），并在 `/Applications` 中安装这四个应用；不需要网络，也不需要第三方 Python 模块：

```bash
python3 tools/vibe_icons.py generate
python3 tools/vibe_icons.py check     # 也是 ./tools/validate.sh --static 的一部分
```

`generate` 用 `sips` 把每个 `.icns` 栅格化为两种尺寸，解码 PNG 后写出 `vv_icons.c` 和 `vv_icons.h`。应用更新可能改变图标；重新生成后请先检查差异再提交。
