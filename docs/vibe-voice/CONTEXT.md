**English** · [简体中文](CONTEXT.zh_CN.md)

# Vibe Voice Input — Domain Glossary

Voice input for vibe coding: speak into the AI Passport, and the recognized text lands in the coding tool focused on the Mac.

## Language

**Device**:
The AI Passport worn by the user. It captures speech, shows status and live text, and sends button intents. It never recognizes speech itself.
_Avoid_: board, hardware, client

**Companion**:
The macOS app that pairs with the Device, recognizes speech, and acts on the **Target**.
_Avoid_: host, server, daemon, helper

**Supported App**:
One of the apps Vibe Voice can deliver to, in this order: Orca, WeChat, ChatGPT, WeCom.

**Current Conversation**:
The conversation a Supported App has open right now. For Orca it is the **Orca Session** of the active tab in Orca's active worktree; for the other apps it is whatever chat the app shows, which cannot be chosen from outside.

**Target**:
The conversation that Inserts, Submits and Undos go to, shown on the Device. It follows Mac focus: whenever a Supported App is frontmost, the Target becomes that app's Current Conversation. While focus is elsewhere the Target stays where it was. It survives restarts; with no Target yet, it is Orca's Current Conversation (launching Orca if needed). If a targeted Orca Session closes, Orca's Current Conversation silently takes its place.
_Avoid_: destination, target window

**Orca Session**:
One live Orca-managed terminal. Text is delivered directly to it without focus or paste.

**Jump**:
Choosing a conversation in the Device picker: it becomes the Target and is brought to the front. The picker opens with the Current Conversation highlighted.
_Avoid_: pin

**Alert**:
A notice on the Device that an Orca agent session finished its turn and is waiting for the user. Opening it Jumps to that session; dismissing it drops it. One pending Alert per session; it disappears when the session starts working again.
_Avoid_: notification, message

**Voice Notes Recording**:
A meeting recording in the Mac app Voice Notes, captured by the Mac's microphone (not the Device's). Double-pressing OK starts one, or stops the one in progress; Voice Notes is launched if needed. It is not a Supported App and never receives Inserts.
_Avoid_: dictation, note

**Target Title**:
The name shown on the Device for the Target's current conversation (for example a chat partner's name), so the user sees where text will land before Submitting.

**Dictation**:
One recording from OK press to the next OK press. While active, audio streams to the Companion and **Partial Text** appears on the Device.
_Avoid_: session, recording, utterance

**Partial Text**:
The provisional recognition result during a Dictation. It may still change and is never inserted.

**Segment**:
The final text of one finished Dictation, inserted into the Target as one paste. Only the most recent Segment is remembered.
_Avoid_: chunk, phrase

**Insert**:
Placing a Segment into the Target at the cursor without pressing Enter.

**Submit**:
Pressing Enter in the Target (DOWN button), which sends the prompt in a coding agent.
_Avoid_: send, commit

**Undo**:
Removing the most recent Segment from the Target (UP button), once. During a Dictation the same button means **Cancel**.

**Cancel**:
Ending a Dictation without inserting anything.

## Relationships

- A **Device** pairs with exactly one **Companion**.
- A **Dictation** produces zero or one **Segment** (zero when cancelled or silent).
- **Undo** applies only to the latest **Segment** and only once.
- An **Insert** never Submits; only the DOWN button Submits.
- Mac focus on a Supported App and a **Jump** both set the **Target**; focus elsewhere leaves it unchanged.
- Only Orca is ever launched automatically, as the default Target (and Voice Notes, to start a Voice Notes Recording).
- A **Voice Notes Recording** and a **Dictation** are independent: either can start, run or stop while the other is in progress.

## Example dialogue

> **Dev:** "If I **Undo** after typing a few characters by hand, what gets removed?"
> **Domain expert:** "The Companion deletes as many characters as the last **Segment** had, so manual edits after an **Insert** make **Undo** inaccurate. That is accepted."

## Flagged ambiguities

- The Chinese word for "submit" was used for both inserting text and pressing Enter. Resolved: **Insert** places text; **Submit** presses Enter.
