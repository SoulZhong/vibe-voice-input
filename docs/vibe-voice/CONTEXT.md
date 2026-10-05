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

**Target**:
Where Segments go, chosen on the Device and kept until changed. Either an **App Target** or an **Orca Session**.
_Avoid_: target window, destination

**App Target**:
A Mac app (or "follow focus") that is activated before its current input receives a paste. Its current conversation is whatever the app has open; it cannot be chosen from outside.

**Orca Session**:
One live Orca-managed terminal. Text is delivered directly to it without focus or paste.

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
- A **Target** whose app is not running is reported, never launched.

## Example dialogue

> **Dev:** "If I **Undo** after typing a few characters by hand, what gets removed?"
> **Domain expert:** "The Companion deletes as many characters as the last **Segment** had, so manual edits after an **Insert** make **Undo** inaccurate. That is accepted."

## Flagged ambiguities

- The Chinese word for "submit" was used for both inserting text and pressing Enter. Resolved: **Insert** places text; **Submit** presses Enter.
