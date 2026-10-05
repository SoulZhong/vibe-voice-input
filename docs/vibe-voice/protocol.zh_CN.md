[English](protocol.md) · **简体中文**

# Vibe Voice BLE 协议（v1）

**设备**（`main/` 中的 AI Passport 固件）与**配套程序**（`host/vibe-voice/` 中的 macOS 应用）之间的约定。术语见 [`CONTEXT.zh_CN.md`](CONTEXT.zh_CN.md)。

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
| `0x01` | HELLO | `ver u8`（=1）、`fw 文本` | TX CCCD 打开后发送一次。 |
| `0x10` | DICT_START | `dict u8` | 听写开始，`dict` 每次递增（回绕）。 |
| `0x11` | AUDIO | `dict u8`、`seq u16`、`pred i16`、`index u8`、`adpcm[160]` | 20 ms 音频 = 320 个采样。`pred`/`index` 为本帧**之前**的编码器状态，因此每帧可独立解码；`seq` 跳号表示丢帧。 |
| `0x12` | DICT_STOP | `dict u8` | 用户结束听写（OK）或达到 5 分钟上限。配套程序完成识别、插入并回复 RESULT。 |
| `0x13` | DICT_CANCEL | `dict u8` | 取消：丢弃、不插入，回复 RESULT 状态 `CANCELLED`。 |
| `0x20` | SUBMIT | — | 在目标中按回车，回复 ACTION_RESULT。 |
| `0x21` | UNDO | — | 从目标删除最近一个片段，回复 ACTION_RESULT。 |
| `0x30` | TARGETS_REQ | `list u8` | 请求目标列表。`0` 根列表，`1` Orca 会话。 |
| `0x31` | TARGET_SELECT | `list u8`、`index u8` | 从最近收到的列表中选择一项，回复 TARGET_STATE。 |

### 配套程序 → 设备

| 类型 | 名称 | 负载 | 含义 |
| --- | --- | --- | --- |
| `0x81` | HELLO_ACK | `ver u8` | 随后发送当前目标的 TARGET_STATE。 |
| `0x82` | STATUS | `code u8`、`文本` | 配套程序层面的问题，在设备上显示（代码见下，`0` 清除）。 |
| `0x90` | PARTIAL | `dict u8`、`文本` | 最新临时文字（尾部），替换上一条。 |
| `0x91` | RESULT | `dict u8`、`status u8`、`文本` | 听写结果；插入成功时 `文本` 为片段（尾部）。 |
| `0xA0` | ACTION_RESULT | `action u8`（`0x20`/`0x21`）、`status u8` | SUBMIT/UNDO 的结果。 |
| `0xB0` | TARGET_ITEM | `list u8`、`index u8`、`count u8`、`flags u8`、`label 文本` | 列表中的一行。`flags`：bit0 当前选中，bit1 可进入子列表，bit2 应用未运行。 |
| `0xB1` | TARGET_END | `list u8`、`count u8` | 列表结束。 |
| `0xB2` | TARGET_STATE | `status u8`、`kind u8`、`label 文本` | 当前目标。`kind`：0 跟随焦点，1 App 目标，2 Orca 会话。已知标题时 `label` 为 `"<名称> · <目标标题>"`。 |

### 状态码

RESULT / ACTION_RESULT / TARGET_STATE 的 `status`：

| 代码 | 名称 | 含义 |
| --- | --- | --- |
| 0 | OK | 已插入 / 已完成 / 目标可用。 |
| 1 | EMPTY | 未识别到内容，未插入。 |
| 2 | CANCELLED | 听写已取消。 |
| 3 | TARGET_UNAVAILABLE | 应用未运行或 Orca 会话已不存在；绝不自动启动。 |
| 4 | RECOGNIZER_ERROR | 语音识别失败。 |
| 5 | PERMISSION | 缺少 macOS 权限（语音识别、辅助功能）。 |
| 6 | NOTHING_TO_UNDO | 没有可撤销的片段。 |

STATUS `code`：0 清除，1 缺少语音识别权限，2 缺少辅助功能权限，3 Orca CLI 不可用，4 `zh-CN` 识别器不可用。

## 音频

- 采集 16 kHz、16 位、单声道。IMA-ADPCM，每采样 4 位，低半字节在前，标准 89 项步长表。320 个采样 → 每帧 160 字节，每秒 50 帧（空中约 8.4 kB/s）。
- 共享黄金向量 [`tests/vectors/adpcm_golden.txt`](../../tests/vectors/adpcm_golden.txt) 必须在固件编码器与配套程序解码器中得到完全一致的结果。

## 限制

- 目标列表最多 **24** 项。TARGET_ITEM 标签最多 **71** 字节，TARGET_STATE 标签最多 **127** 字节。配套程序保留名称、用"…"缩短标题来适配（标签不取尾部）。
- 设备在 5 秒内未收到 TARGET_END 时关闭选择页，等待 RESULT 超过 20 秒则放弃。

## 行为规则

- 插入从不按回车，只有 SUBMIT 才会。
- UNDO 按最近一个片段的字数（字素簇）删除，仅一次。UNDO、成功的 SUBMIT 或新听写开始之后，在下一个片段出现前没有可撤销内容。
- flags 含 bit1 的行用于打开子列表：设备对其发送 TARGETS_REQ 而非 TARGET_SELECT。若对该行发送 TARGET_SELECT，目标保持不变，并回复 TARGET_STATE。
- 未插入时 RESULT 文本为空。对未知听写的 DICT_STOP 回复 RESULT `EMPTY`。
- STATUS 每次只携带一个代码，优先级：1、4、2（仅 App 目标）、3（仅 Orca 会话）。
- 配套程序以无响应写发送 PARTIAL，其余帧均以有响应写发送。
- 向 App 目标插入前，配套程序先激活应用、读取其焦点窗口标题（目标标题）、用 ⌘V 粘贴并恢复剪贴板。应用未运行时绝不自动启动。
- 对 Orca 会话，配套程序使用 `orca terminal send`（文字、回车或退格）和 `orca terminal switch` 将其切到前台，不经过剪贴板。
- （重新）连接时设备发送 HELLO，配套程序回复 HELLO_ACK 和 TARGET_STATE。选中的目标保存在配套程序配置中，设备重启后仍然有效。
- 听写过程中断开连接时，双方都放弃本次听写且不插入。
