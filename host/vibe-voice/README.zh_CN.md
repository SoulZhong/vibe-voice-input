[English](README.md) · **简体中文**

# Vibe Voice Companion（macOS）

Vibe Voice 语音输入的 **Companion**：通过低功耗蓝牙与 AI Passport（**Device**）配对，
用 Apple Speech（普通话 `zh-CN`，可用时走端侧识别）识别传来的语音，再把每个
**Segment** 送到 **Target**——某个支持的应用的当前会话，或一个在线的 Orca 终端。
术语见 [`docs/vibe-voice/CONTEXT.zh_CN.md`](../../docs/vibe-voice/CONTEXT.zh_CN.md)，
线上格式见 [`docs/vibe-voice/protocol.zh_CN.md`](../../docs/vibe-voice/protocol.zh_CN.md)。

## 环境要求

- macOS 13 及以上（Apple 芯片或 Intel），蓝牙已打开。
- Rust 1.88 及以上（`cargo`）。
- 离线识别需要普通话端侧模型（系统设置 > 键盘 > 听写，添加中文）；没有时会使用 Apple 服务器。

## 构建与测试

```bash
cd host/vibe-voice
cargo test                       # 协议、ADPCM 黄金向量、会话逻辑
cargo clippy --all-targets -- -D warnings
scripts/bundle.sh                # release 构建 -> target/bundle/VibeVoice.app
```

`bundle.sh` 生成 `VibeVoice.app`（bundle id `cn.folotoy.vibevoice`，菜单栏常驻、无 Dock
图标）并做 ad-hoc 签名。可复制到 `/Applications`，从访达或 `open` 启动。

## 权限

用 `open target/bundle/VibeVoice.app`（或在访达中）启动。macOS 会逐项请求一次：

| 权限 | 用途 | 修改位置 |
| --- | --- | --- |
| 蓝牙 | 与 Device 通信 | 隐私与安全性 > 蓝牙 |
| 语音识别 | 把音频转成文字 | 隐私与安全性 > 语音识别 |
| 辅助功能 | 激活应用、读取窗口标题、发送 Cmd+V / 回车 / 删除 | 隐私与安全性 > 辅助功能 |

不需要麦克风权限：音频来自 Device。缺少权限会以 STATUS 码告知 Device（1 = 语音识别，
2 = 辅助功能）。ad-hoc 签名每次重新构建都会变化，重建后请运行 `tccutil reset Accessibility cn.folotoy.vibevoice`
再重新授权；或打包时设置 `VV_SIGN_IDENTITY="<钥匙串中的签名身份>"`，授权即可在重建后保留。

请通过 `open` 或访达启动，不要在终端里直接执行二进制：macOS 会向*启动它的应用*（你的
终端）索取语音识别用途说明，终端没有该说明时进程会被系统终止。因此在应用包之外，程序
不会主动请求语音识别权限。

## 配对

1. 打开 Device 并启动 Vibe Voice；扫描名为 `VibeVoice-XXXX` 的设备时菜单栏显示 `VV ○`。
2. 首次连接时 macOS 弹出配对框，输入 Device 屏幕上的 6 位配对码。输入期间 Companion
   最多重试两分钟。
3. 菜单栏变为 `VV ●`。双方都会记住绑定；睡眠、重启或走出范围后自动重连。

附近有多台设备时，可用 `VIBE_VOICE_DEVICE=VibeVoice-XXXX` 启动以固定一台。需要重新配对时，
在“系统设置 > 蓝牙”中移除设备，并清除 Device 上的绑定。

## Target

支持的应用为内置列表，依次为：Orca、微信（`com.tencent.xinWeChat`）、ChatGPT
（`/Applications/ChatGPT.app`，bundle id `com.openai.codex`）和企业微信
（`com.tencent.WeWorkMac`）；bundle id 均读取自已安装应用的 `Info.plist`。无需任何配置。

Target 保存在 `~/.config/vibe-voice/target.json`，Device 和 Companion 重启后仍然保留：

- **跟随焦点。** 只要最前面是支持的应用（每 2 秒检查一次，每次 Insert 和 Submit 前再检查
  一次），Target 就变为该应用的当前会话：Orca 为其当前工作树中活动标签页的活动终端，其他
  应用为其正在显示的聊天。焦点在其他应用时 Target 不变。
- **默认。** 尚无 Target，或目标 Orca 会话已关闭时，静默使用（并保存）Orca 的当前会话。
  Orca 未运行时，Insert 或 Submit 会用 `orca open` 启动它（最长 20 秒）。
- **跳转。** 在 Device 的选择页（长按 OK）中先选应用、再选会话：它成为 Target 并被切到
  前台。Orca 列表显示所有工作树中的 Orca 会话，当前会话在最前；其他应用只显示其当前会话。

送达前总是先把 Target 切到前台：

- **Orca 会话**：执行 `orca terminal switch` 并激活 Orca，然后文字通过
  `orca terminal send --text` 发送，Submit 用 `--enter`，Undo 发送 DEL 字符。不使用剪贴板。
  Segment 中的换行会变成空格，避免误提交。
- **微信、ChatGPT、企业微信**：从不自动启动。应用未运行时 Device 收到
  `TARGET_UNAVAILABLE`；否则 Companion 激活应用，用 Cmd+V 粘贴 Segment，约 0.4 秒后恢复
  原剪贴板。Submit 按回车；Undo 按上一个 Segment 的字符数逐个删除。焦点窗口标题作为
  Target Title 显示在 Device 上。
- **Undo** 作用于上一个 Segment 送达的位置，即使 Target 之后已经改变。

旧版本在同一目录写过 `targets.toml` 和 `state.toml`；现在不再读取，也不会改动它们。可选的
`~/.config/vibe-voice/vocabulary.txt` 每行一个词，用于让识别偏向项目术语。

## 诊断

```bash
target/release/vibe-voice --check        # 权限与识别器状态
target/release/vibe-voice --orca-list    # Device 将看到的 Orca Session 列表，当前会话标 *
# 把 16 kHz 单声道 16 位 WAV 经 ADPCM、协议逻辑和 Apple Speech 识别一遍。
# 不会插入任何内容，只打印帧。
say -v Tingting "把这个函数改成异步" -o /tmp/clip.wav --data-format=LEI16@16000 --file-format=WAVE
open -W -n --stdout /tmp/sim.txt target/bundle/VibeVoice.app --args --simulate /tmp/clip.wav
cat /tmp/sim.txt
```

从访达启动时日志写入 `~/Library/Logs/VibeVoice.log`（加 `--verbose` 可看到逐帧细节）。

## 故障排查

| 现象 | 检查 |
| --- | --- |
| 一直在“搜索设备” | Device 是否在广播？是否已授予蓝牙权限？ |
| 不弹配对框 | 在蓝牙设置和 Device 上删除旧绑定后重连。 |
| Device 显示 STATUS 1 或 RESULT 5 | 授予语音识别权限后重启应用。 |
| Device 显示 STATUS 2 | Target 在微信、ChatGPT 或企业微信中：授予辅助功能权限（每次临时签名重建后需重新添加）。 |
| Device 显示 STATUS 3 | 无法启动 Orca 或其 CLI 出错：检查 `orca status` 和 `orca` CLI（`/usr/local/bin/orca`，或设置 `VIBE_VOICE_ORCA`）。 |
| Device 显示 STATUS 4 | `zh-CN` 识别器不可用：检查网络或安装听写语言。 |
| 文字进了错误的会话 | 说话前看 Device 顶部栏：切到支持的应用后 2 秒内会改变 Target；也可在选择页跳转到其他会话。 |
| 粘贴成功但剪贴板丢失 | 0.4 秒内又发生了复制；此时有意保留较新的剪贴板。 |

## 源码结构

| 文件 | 作用 |
| --- | --- |
| `src/protocol.rs` | 帧编解码、180 字节上限、UTF-8 尾部截断 |
| `src/adpcm.rs` | IMA-ADPCM 解码与编码（黄金向量测试） |
| `src/audio.rs` | AUDIO 帧转 PCM，丢帧处补静音 |
| `src/session.rs` | Companion 状态机，依赖 `Injector` / `Recognizer` / `OrcaApi` trait |
| `src/config.rs` | 支持的应用与保存的 Target（`target.json`） |
| `src/orca.rs` | Orca CLI 客户端：从 `worktree ps` 与可视布局得出当前会话、启动、发送 |
| `src/ble.rs`、`src/speech.rs`、`src/inject_macos.rs`、`src/ui.rs` | macOS 胶水层 |
