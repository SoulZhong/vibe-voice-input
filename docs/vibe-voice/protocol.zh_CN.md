[English](protocol.md) · **简体中文**

# Vibe Voice BLE 协议（v2）

**设备**（`main/` 中的 AI Passport 固件）与**配套程序**（`host/vibe-voice/` 中的 macOS 应用）之间的约定。术语见 [`CONTEXT.zh_CN.md`](CONTEXT.zh_CN.md)。

第 2 版用“保存且跟随焦点的目标”和“跳转”选择页取代了 v1 的可配置目标列表：TARGETS_REQ 的列表编号、TARGET_ITEM 的 flags 和 TARGET_STATE 的格式都已变化。v1 的对端会看到版本不一致。

## 传输

- BLE GATT，沿用 Nordic UART Service 布局，设备为外设。
  - 服务 `6e400001-b5a3-f393-e0a9-e50e24dcca9e`
  - RX `6e400002-…` 写 / 无响应写：配套程序 → 设备
  - TX `6e400003-…` 通知：设备 → 配套程序
- 广播名：`VibeVoice-XXXX`（MAC 末两字节，大写十六进制）。
- 安全：LE Secure Connections、绑定、MITM。设备 IO 能力为 DisplayOnly，屏幕显示 6 位配对码，由用户在 macOS 输入。RX、TX 及 TX CCCD 都要求加密链路。绑定信息存于 NVS。
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

## 音频

- 采集 16 kHz、16 位、单声道。IMA-ADPCM，每采样 4 位，低半字节在前，标准 89 项步长表。320 个采样 → 每帧 160 字节，每秒 50 帧（空中约 8.4 kB/s）。
- 共享黄金向量 [`tests/vectors/adpcm_golden.txt`](../../tests/vectors/adpcm_golden.txt) 必须在固件编码器与配套程序解码器中得到完全一致的结果。

## 限制

- 目标列表最多 **24** 项。TARGET_ITEM 标签最多 **71** 字节，TARGET_STATE 标签最多 **127** 字节。配套程序保留名称、用"…"缩短标题来适配（标签不取尾部）。
- 设备在 5 秒内未收到 TARGET_END（列表 1 的请求可能启动 Orca，为 25 秒）或跳转后 5 秒内未收到 TARGET_STATE 时关闭选择页。等待 RESULT 超过 30 秒则放弃（插入前可能要等 Orca 启动，最长 20 秒）。

## 行为规则

- 插入从不按回车，只有 SUBMIT 才会。
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
