// main/vv_strings.h -- every fixed Chinese UI string of the Vibe Voice Device.
//
// Font contract (checked by `python3 tools/vibe_fonts.py check`):
//   - VV_H_*  strings are drawn with the 24 px title font; they must be covered
//             by assets/fonts/vibe-voice/vv_font_title_24.c. The title font's
//             glyph list is generated from these defines, so regenerate the
//             fonts after adding or changing one.
//   - Every other string literal in main/vv_*.c|h is drawn with the 16 px body
//     font (GB2312 + ASCII + CJK punctuation + fullwidth forms).
// Keep one literal per line so the checker sees each string.
#pragma once

// ---- Titles (24 px) --------------------------------------------------------
#define VV_H_NO_LINK          "未连接"
#define VV_H_PAIRING          "配对码"
#define VV_H_LINKING          "正在连接"
#define VV_H_IDLE             "就绪"
#define VV_H_DICTATING        "听写中"
#define VV_H_WAITING          "识别中…"
#define VV_H_INSERTED         "已插入"
#define VV_H_EMPTY            "没听清"
#define VV_H_CANCELLED        "已取消"
#define VV_H_TARGET_DOWN      "目标未运行"
#define VV_H_RECOGNIZER       "识别失败"
#define VV_H_PERMISSION       "缺少权限"
#define VV_H_TIMEOUT          "识别超时"
#define VV_H_FAILED           "未完成"
#define VV_H_PICKER_ROOT      "选择目标"
#define VV_H_PICKER_ORCA      "Orca 会话"
#define VV_H_PICKER_WECHAT    "微信会话"
#define VV_H_PICKER_CHATGPT   "ChatGPT 会话"
#define VV_H_PICKER_WECOM     "企业微信会话"

// ---- Body text (16 px) -----------------------------------------------------
#define VV_T_NO_TARGET        "未选择目标"
#define VV_T_NOT_RUNNING      "未运行"
#define VV_T_NO_LINK_HINT     "在 Mac 上打开 Vibe Voice 并连接"
#define VV_T_DEVICE_NAME      "设备名"
#define VV_T_PAIRING_HINT     "在 Mac 上输入此配对码"
#define VV_T_LINKING_HINT     "等待 Mac 端响应…"
#define VV_T_IDLE_HINT        "按 OK 开始说话"
#define VV_T_LISTEN_HINT      "请开始说话…"
#define VV_T_WAITING_HINT     "正在等待最终结果"
#define VV_T_EMPTY_BODY       "没有识别到文字，未插入"
#define VV_T_CANCELLED_BODY   "本次听写已丢弃"
#define VV_T_TARGET_DOWN_BODY "目标应用未运行，未插入"
#define VV_T_RECOGNIZER_BODY  "语音识别出错，未插入"
#define VV_T_PERMISSION_BODY  "请在 Mac 上授予所需权限"
#define VV_T_TIMEOUT_BODY     "Mac 端没有回应，未插入"
#define VV_T_FAILED_BODY      "Mac 端返回未知状态"
#define VV_T_PICKER_LOADING   "加载中…"
#define VV_T_PICKER_EMPTY     "列表为空"
#define VV_T_PICKER_NOT_RUN   "应用未运行"
#define VV_T_PICKER_NO_CONV   "没有会话"
#define VV_T_PICKER_JUMPING   "正在切换…"
#define VV_T_PICKER_FAILED    "列表加载失败"

// Toasts after SUBMIT / UNDO / pairing.
#define VV_T_SUBMITTING       "正在发送…"
#define VV_T_SUBMITTED        "已发送"
#define VV_T_UNDOING          "正在撤销…"
#define VV_T_UNDONE           "已撤销"
#define VV_T_NOTHING_TO_UNDO  "没有可撤销的内容"
#define VV_T_ACTION_TARGET    "目标未运行"
#define VV_T_ACTION_PERM      "缺少 macOS 权限"
#define VV_T_ACTION_FAILED    "操作失败"
#define VV_T_PAIR_FAILED      "配对失败，请重试"
#define VV_T_LIST_FAILED      "列表加载失败"
#define VV_T_JUMPED           "已切换"

// Alert card (an Orca agent session waits for the user).
#define VV_T_ALERT_HINT       "OK 打开 · ▲ 忽略"
#define VV_T_ALERT_NEXT       "▼ 下一条"

// Voice Notes Recording: top bar ("录音 12:34") and toasts.
#define VV_T_NOTES_REC        "录音"
#define VV_T_NOTES_REC_SHORT  "录"
#define VV_T_NOTES_PAUSED     "暂停"
#define VV_T_NOTES_STARTING   "启动中…"
#define VV_T_NOTES_STOPPING   "停止中…"
#define VV_T_NOTES_STARTED    "已开始录音"
#define VV_T_NOTES_STOPPED    "录音已停止"
#define VV_T_NOTES_LAUNCH     "Voice Notes 启动失败"
#define VV_T_NOTES_FAILED     "录音失败"
#define VV_T_NOTES_RISK_BT    "注意：蓝牙麦克风"
#define VV_T_NOTES_RISK_VI    "注意：语音突显已开启"
#define VV_T_NOTES_RISK       "注意：录音环境有风险"
#define VV_T_NOTES_MISSING    "未安装 Voice Notes"
#define VV_T_NOTES_DENIED     "未允许控制录音"
#define VV_T_NOTES_STOP_FAIL  "停止录音失败"

// Companion STATUS codes (protocol section "Status codes").
#define VV_T_STATUS_SPEECH    "未授予语音识别权限"
#define VV_T_STATUS_AX        "未授予辅助功能权限"
#define VV_T_STATUS_ORCA      "Orca CLI 不可用"
#define VV_T_STATUS_ZH        "中文语音识别不可用"
#define VV_T_STATUS_VERSION   "Mac 端协议版本不兼容"
#define VV_T_STATUS_OTHER     "Mac 端报告问题"

// Control hints. ▲ = UP, ▼ = DOWN.
#define VV_T_HINT_IDLE_1      "OK 说话 · 长按 OK 选目标"
#define VV_T_HINT_IDLE_2      "▲ 撤销 · ▼ 发送"
#define VV_T_HINT_DICT        "OK 完成 · ▲ 取消"
#define VV_T_HINT_PICKER_1    "▲▼ 移动 · OK 选择"
#define VV_T_HINT_PICKER_2    "长按 OK 返回"
