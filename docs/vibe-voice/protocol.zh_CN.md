[English](protocol.md) · **简体中文**

# Vibe Voice BLE 协议（v2）

**设备**（`main/` 中的 AI Passport 固件）与**配套程序**之间的约定。术语见 [`CONTEXT.zh_CN.md`](CONTEXT.zh_CN.md)。

配套程序现已并入 [Voice Notes](https://github.com/SoulZhong/voice-notes)（macOS 与 Windows，代码在 `src-tauri/vibe-device/src/protocol.rs`）；原来位于 `host/vibe-voice/` 的独立 macOS 程序已删除。本文仍是唯一标准：改协议时先改本文并升版本号，再通知 Voice Notes 跟进。能不升版就不升版，因为设备需要人工刷机。版本不一致时，Voice Notes 会提示用户该刷固件还是该升级 Voice Notes。

第 2 版用“保存且跟随焦点的目标”和“跳转”选择页取代了 v1 的可配置目标列表：TARGETS_REQ 的列表编号、TARGET_ITEM 的 flags 和 TARGET_STATE 的格式都已变化。v1 的对端会看到版本不一致。

## 传输

- BLE GATT，沿用 Nordic UART Service 布局，设备为外设。
  - 服务 `6e400001-b5a3-f393-e0a9-e50e24dcca9e`
  - RX `6e400002-…` 写 / 无响应写：配套程序 → 设备
  - TX `6e400003-…` 通知：设备 → 配套程序
- 广播名：`VibeVoice-XXXX`（MAC 末两字节，大写十六进制）。
- 安全：LE Secure Connections、绑定、MITM。设备 IO 能力为 DisplayOnly，屏幕显示 6 位配对码，用户在 macOS 的配对弹框或 Windows 上的 Voice Notes 里输入。RX、TX 及 TX CCCD 都要求加密链路。绑定信息存于 NVS。
- 每次 GATT 写或通知承载一个协议帧，每帧最多 **180 字节**。不分片；长文本只发送**尾部**，在 UTF-8 字符边界截断。
- 整数为小端序。文本为不带结束符的 UTF-8，长度即帧剩余部分。

## 帧结构

```text
字节 0   类型
字节 1.. 负载（依类型而定）
```

### 设备 → 配套程序

| 类型 | 名称 | 负载 | 含义 |
| --- | --- | --- | --- |
| `0x01` | HELLO | `ver u8`（=2）、`fw 文本` | TX CCCD 打开后发送，并每 1 秒重发直到收到 HELLO_ACK（绑定重连时 CCCD 可能在配套程序开始监听前就已恢复）。 |
| `0x10` | DICT_START | `dict u8` | 听写开始，`dict` 每次递增（回绕）。 |
| `0x11` | AUDIO | `dict u8`、`seq u16`、`pred i16`、`index u8`、`adpcm[160]` | 20 ms 音频 = 320 个采样。`pred`/`index` 为本帧**之前**的编码器状态，因此每帧可独立解码；`seq` 跳号表示丢帧。 |
| `0x12` | DICT_STOP | `dict u8` | 用户结束听写（OK）或达到 5 分钟上限。配套程序完成识别、插入并回复 RESULT。 |
| `0x13` | DICT_CANCEL | `dict u8` | 取消：丢弃、不插入，回复 RESULT 状态 `CANCELLED`。 |
| `0x20` | SUBMIT | — | 在目标中按回车，回复 ACTION_RESULT。 |
| `0x21` | UNDO | — | 从目标删除最近一个片段，回复 ACTION_RESULT。 |
| `0x30` | TARGETS_REQ | `list u8` | 请求选择页列表。`0` 为支持的应用；`n`（1–4）为根列表第 `n - 1` 行应用的会话：`1` Orca 会话，`2` 微信，`3` ChatGPT，`4` 企业微信。 |
| `0x31` | TARGET_SELECT | `list u8`、`index u8` | 跳转到最近收到的列表中的一行，回复 TARGET_STATE。 |
| `0x40` | NOTES_TOGGLE | — | 没有进行中的 Voice Notes 录音时开始录音，否则停止（双击 OK）。回复 NOTES_STATE。 |
| `0x50` | ALERT_OPEN | `id u8` | 打开提醒：跳转到其 Orca 会话。回复 TARGET_STATE。 |
| `0x51` | ALERT_DISMISS | `id u8` | 忽略提醒。无回复。 |

### 配套程序 → 设备

| 类型 | 名称 | 负载 | 含义 |
| --- | --- | --- | --- |
| `0x81` | HELLO_ACK | `ver u8`（=2） | 随后发送 TARGET_STATE。 |
| `0x82` | STATUS | `code u8`、`文本` | 配套程序层面的问题，在设备上显示（代码见下，`0` 清除）。 |
| `0x90` | PARTIAL | `dict u8`、`文本` | 最新临时文字（尾部），替换上一条。 |
| `0x91` | RESULT | `dict u8`、`status u8`、`文本` | 听写结果；插入成功时 `文本` 为片段（尾部）。 |
| `0xA0` | ACTION_RESULT | `action u8`（`0x20`/`0x21`）、`status u8` | SUBMIT/UNDO 的结果。 |
| `0xB0` | TARGET_ITEM | `list u8`、`index u8`、`count u8`、`flags u8`、`label 文本` | 列表中的一行。`flags`：bit0 当前（根列表：目标所在的应用；子列表：该应用的当前会话），bit1 可进入子列表，bit2 应用未运行。 |
| `0xB1` | TARGET_END | `list u8`、`count u8` | 列表结束。 |
| `0xB2` | TARGET_STATE | `status u8`、`kind u8`、`app u8`、`label 文本` | 目标。`kind`：1 支持的应用的当前会话，2 Orca 会话（0 保留）。`app`：目标所在的支持的应用，`0` Orca，`1` 微信，`2` ChatGPT，`3` 企业微信，`0xFF` 无；设备据此显示应用图标。已知标题时 `label` 为 `"<应用> · <目标标题>"`。 |
| `0xC0` | NOTES_STATE | `state u8`、`elapsed_s u32`、`notice u8` | Voice Notes 录音状态（7 字节）。`state`：0 空闲，1 录音中，2 已暂停，3 启动中，4 停止中。`elapsed_s`：已录时长（不含暂停），录音中设备据此继续计时。`notice`：用于提示条的一次性事件，见下。 |
| `0xD0` | ALERT | `id u8`、`app u8`、`label_len u8`、`label 文本`（`label_len` 字节）、`message 文本`（其余部分） | 某个 Orca agent 会话在等待用户。`id` 对每个会话固定；已知 `id` 的 ALERT 替换该提醒。`app` 为支持的应用（0 Orca）。`label` 为 `"<工作树> · <标题>"`（最多 63 字节），`message` 为 agent 最后说的话的开头（其余部分由 ALERT_MORE 续传）。 |
| `0xD1` | ALERT_CLEAR | `id u8` | 该会话不再等待（重新开始工作或已关闭）：删除提醒。 |
| `0xD2` | ALERT_MORE | `id u8`、`offset u16`、`文本` | 提醒消息的下一段，从消息的第 `offset` 字节开始。整条消息最多 360 字节（截断时保留尾部并以 `…` 开头），每段都在 UTF-8 边界截断。只有 `offset` 等于设备已收到的字节数时才追加；未知 `id` 或其他偏移的分段一律忽略。 |

### 状态码

RESULT / ACTION_RESULT / TARGET_STATE 的 `status`：

| 代码 | 名称 | 含义 |
| --- | --- | --- |
| 0 | OK | 已插入 / 已完成 / 目标可用（Orca 未运行时 Orca 目标仍可用：会被启动）。 |
| 1 | EMPTY | 未识别到内容，未插入。 |
| 2 | CANCELLED | 听写已取消。 |
| 3 | TARGET_UNAVAILABLE | 目标应用未运行（只有 Orca 会被启动）、Orca 无法启动或没有会话，或跳转失败。 |
| 4 | RECOGNIZER_ERROR | 语音识别失败。 |
| 5 | PERMISSION | 缺少 macOS 权限（语音识别、辅助功能）。 |
| 6 | NOTHING_TO_UNDO | 没有可撤销的片段。 |

STATUS `code`：0 清除，1 缺少语音识别权限，2 缺少辅助功能权限，3 Orca CLI 不可用，4 `zh-CN` 识别器不可用。

NOTES_STATE `notice`：0 无，1 已开始，2 已停止，3 Voice Notes 未能及时启动，4 开始失败，5 已开始但使用蓝牙麦克风（风险 `bluetooth_mic`），6 已开始但有其他风险，7 未安装 Voice Notes，8 已开始但开启了语音突显（风险 `voice_isolation`），9 Voice Notes 中未允许控制（“允许 AI 控制录制”关闭），10 停止失败。

## 音频

- 采集 16 kHz、16 位、单声道。IMA-ADPCM，每采样 4 位，低半字节在前，标准 89 项步长表。320 个采样 → 每帧 160 字节，每秒 50 帧（空中约 8.4 kB/s）。
- 共享黄金向量 [`tests/vectors/adpcm_golden.txt`](../../tests/vectors/adpcm_golden.txt) 必须在固件编码器与配套程序解码器中得到完全一致的结果。

## 限制

- 目标列表最多 **24** 项。TARGET_ITEM 标签最多 **71** 字节，TARGET_STATE 标签最多 **127** 字节。配套程序保留名称、用"…"缩短标题来适配（标签不取尾部）。
- 设备在 5 秒内未收到 TARGET_END（列表 1 的请求可能启动 Orca，为 25 秒）或跳转后 5 秒内未收到 TARGET_STATE 时关闭选择页。等待 RESULT 超过 30 秒则放弃（插入前可能要等 Orca 启动，最长 20 秒）。

## 行为规则

- 插入从不按回车，只有 SUBMIT 才会。
- 每个片段都以标点结尾，避免多次插入时句子连在一起：中文补"。"，英文补". "（带一个空格）。长停顿拆开的语句也按同样方式连接。
- UNDO 按最近一个片段的字数（字素簇）删除，仅一次，删除位置是该片段当初插入的地方（即使目标已改变）。UNDO、成功的 SUBMIT 或新听写开始之后，在下一个片段出现前没有可撤销内容。
- **目标。** 配套程序保存目标（一个 Orca 会话，或除 Orca 外的某个支持的应用，其当前会话即该应用正在显示的会话），重启后仍然有效。只要 Mac 前台是支持的应用（每 2 秒检查一次，每次插入和提交前再检查一次），目标就变为该应用的当前会话（Orca：Orca 当前工作树中活动标签页的活动终端）。前台是其他应用时目标不变。尚无目标，或目标 Orca 会话已不存在时，静默改用 Orca 的当前会话。
- **送达。** 插入、提交和撤销前先把目的地切到前台。Orca 会话：执行 `orca terminal switch` 并激活 Orca，再用 `orca terminal send`（文字、回车或退格）发送，不经过剪贴板，换行变为空格。其他应用：激活应用后用 ⌘V 粘贴（并恢复剪贴板）或按回车 / 删除键。
- **启动。** 只会启动 Orca（`orca open`，最长 20 秒）：目标在 Orca 中时的插入或提交，以及 TARGETS_REQ 1。其他应用绝不启动；其子列表为空，插入、提交和撤销回复 TARGET_UNAVAILABLE。
- **选择页。** 根列表固定为上述顺序的四个支持的应用（第 `i` 行 = `app` `i`），每行都有 bit1，目标所在应用带 bit0。在根列表第 `i` 行按 OK 发送 TARGETS_REQ `i + 1`。列表 1 最多 24 个跨工作树的 Orca 会话：当前会话在最前（bit0），其次是当前工作树的其余会话，再其次是其他工作树；标签为 `"<工作树> · <标题>"`。列表 2–4 只有一行 `"当前会话 · <窗口标题>"`（当前会话，bit0），应用未运行时为空。设备打开列表时光标位于 bit0 行。
- **跳转。** 对子列表行发送 TARGET_SELECT 会把它设为目标并切到前台（Orca：`orca terminal switch` 并激活 Orca；其他应用：激活）。回复 TARGET_STATE，跳转失败时其状态为失败原因。设备收到该 TARGET_STATE 后关闭选择页。对根列表或越界的 TARGET_SELECT 不改变目标，同样回复 TARGET_STATE。
- **TARGET_STATE** 在 HELLO_ACK 之后、DICT_START 时、每次插入和提交之后、跳转之后发送，并在目标或其显示内容变化时发送（连接中且未听写时每 2 秒刷新一次）。
- 未插入时 RESULT 文本为空。对未知听写的 DICT_STOP 回复 RESULT `EMPTY`。
- STATUS 每次只携带一个代码，优先级：1、4、2（目标在 Orca 以外的应用中时）、3（目标在 Orca 中且 CLI 或启动失败时）。
- 配套程序以无响应写发送 PARTIAL，其余帧均以有响应写发送。
- （重新）连接时设备发送 HELLO，配套程序回复 HELLO_ACK 和 TARGET_STATE。目标保存在配套程序的 `~/.config/vibe-voice/target.json` 中，设备和配套程序重启后仍然有效。
- 听写过程中断开连接时，双方都放弃本次听写且不插入。
- **提醒。** Orca CLI 没有事件流，因此连接期间配套程序在独立线程上每 2 秒执行一次 `orca terminal list`（结果同时刷新 Orca 缓存），并从标题读取每个 agent 会话的状态。Claude Code：标题前缀为转圈字形（`◐ ◑ ◒ ◓`、盲文 `U+2800–U+28FF`、`✶ ✻ ✽ ✢` 等）表示工作中，`✳` 表示等待。其他 agent 使用 Orca 自己写入的标题（其 agent 表）：`"<Agent>"` 为工作中，`"<Agent> ready"` 或 `"<Agent> - action required"` 为等待，适用于 Codex、Cursor Agent、OpenCode、Pi、OMP、Droid、Hermes、Devin 和 ZCode；Gemini CLI 为 `✦` 工作中、`◇` 等待。Codex 工作时 Orca 保留 Codex 自己的标题，因此只有标题此前能识别为工作中时 Codex 才会提醒。其他标题和没有 agent 的终端状态未知，从不提醒。
- 带 agent 的会话每次从工作中变为等待时都发出 ALERT，与 Orca 自己的通知一致，无论用户是否正在看该会话；首次看到（配套程序启动或重新连接）或会话消失时不提醒。每个会话只有一条提醒；只有会话重新开始工作或关闭时才发送 ALERT_CLEAR（切到前台不会清除）；设备上打开或忽略时由设备删除。HELLO_ACK 之后重发待处理的提醒；断开连接时双方都清空。
- 消息取自该会话渲染后的屏幕，在状态变化时读取一次（`orca terminal read --screen`，在 Orca 监视线程上，最长 3 秒）：优先用最后一个不是工具调用的 “⏺” 回复块（即 Orca 在 macOS 通知中显示的内容），回复已滚出屏幕时改用 Claude Code 的 “※ recap:” 段落；续行拼接，去掉表格边框，过长时保留开头并以“…”结尾；状态行、提示框及其下方的状态栏忽略。两者都没有时用列表预览中可读的结尾，再不行则为“等待你的回复”。
- ALERT_OPEN 的跳转与在 Orca 行上 TARGET_SELECT 相同：该会话成为目标，执行 `orca terminal switch` 并把 Orca 切到前台，回复 TARGET_STATE（会话已不存在时带失败状态；未知 `id` 回复 ALERT_CLEAR 和 TARGET_STATE）。
- **Voice Notes 录音。** Mac 应用 Voice Notes（`com.teemo.voice-notes`）中的会议录音，由 Mac 的麦克风采集。它与听写互不影响：听写中也接受 NOTES_TOGGLE 且绝不结束听写；Voice Notes 也绝不改变目标、撤销或任何听写帧。配套程序通过 Voice Notes 的 Unix socket `<app data>/mcp.sock`（`{"op":"status"|"start"|"stop"}`）通信，且总在独立的工作线程上进行，因此 Voice Notes 启动或加载模型时音频和 PARTIAL 帧照常流动。
- 收到 NOTES_TOGGLE 后，配套程序立即回复 NOTES_STATE 启动中（3）或停止中（4），随后回复带结果和 notice 的 NOTES_STATE。开始录音时如 Voice Notes 未运行，则在后台启动它（`open -g -b`，最多等 20 秒直到 socket 应答）；开录风险不会阻止录音，而以 notice 5、6 或 8 告知。启动中或停止中再次切换只会重发当前 NOTES_STATE。
- NOTES_STATE 在 HELLO_ACK 之后（TARGET_STATE 和 STATUS 之后）、每次切换之后发送；配套程序每 2 秒（连接期间，听写中也是）查询一次状态，状态变化或设备计时偏差超过 2 秒时也会发送，因此在 Mac 上开始、暂停或停止的录音也会显示。断开连接后设备清除该状态。
