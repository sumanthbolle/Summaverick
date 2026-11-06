# core — the engine, and how it becomes the Summaverick Everyday device

`sve_core.h` is the whole contract. Anything that can call a C function can host the engine.

## Build matrix

| Host | ASR | LLM backend | Build |
|---|---|---|---|
| iOS | Apple Speech (host side) | llama.xcframework, Metal | compiled into the app target, `SVE_WITH_WHISPER` undefined |
| macOS / Linux x86 (dev) | whisper.cpp | llama.cpp CPU | `cmake -S core -B build` |
| **Device: RK3576 board, Linux** | whisper.cpp (or IndicConformer) | llama.cpp CPU now; RKLLM NPU later | same CMake, `-DGGML_NATIVE=ON` on the board |
| Android | host side (sherpa‑onnx) or whisper | llama.cpp CPU | NDK CMake, same files |

## Verified run (Linux, 2 CPU threads, Qwen3‑0.6B‑Q8_0)

Input: a 5‑sentence biology transcript. Output, unedited:

```
===== short =====
Today in biology class, we learned about cell structure: the cell membrane controls entry and exit of
materials, the nucleus contains DNA, mitochondria produce energy (the powerhouse), and homework includes
reading chapter 4 pages 61–70 and drawing a plant cell by Friday. The unit test is scheduled for November 18th.

===== detailed =====
## Topic
Today in biology class we covered cell structure.
## Key points
- The cell membrane controls what enters and leaves the cell.
- The nucleus holds the DNA.
- Mitochondria produce energy through respiration; the teacher called them the powerhouse of the cell.
## Decisions and instructions
- Homework: read chapter 4, pages 61 to 70, and draw a labelled plant cell by Friday.
## Dates and numbers mentioned
- The unit test is on 18 November.

===== revision =====
Q1. What is the function of the cell membrane?
A1. The cell membrane controls what enters and leaves the cell.
…
Q6. What homework is required for this lesson?
A6. Read chapter 4, pages 61 to 70, and draw a labelled plant cell by Friday.

ask "when is the test and what is the homework?"
→ The test is on 18 November, and the homework is to read chapter 4 from pages 61–70 by Friday.
```

A 430‑word transcript forced through a 512‑token context went 4 chunks → 3 → 2 → final and stayed grounded. On an iPhone with Metal, or on the RK3576, expect this to be several times faster than the 2‑thread x86 container used here (29 s for all three outputs).

## Mapping onto the device

The device is the same engine behind a small daemon and a web page. Nothing in `core/` changes.

```
 [button + LED] ──GPIO──┐
 [2 MEMS mics] ─ALSA──▶ recorder (arecord, 16 kHz mono WAV)  ──▶  recordings/
                                                                      │  (inotify)
                 svecore daemon (C++ or Python over the C API) ◀──────┘
                   1. sve_transcribe_wav        (whisper / IndicConformer)
                   2. sve_suggest_title
                   3. sve_generate × 3          (while charging, batch)
                   4. sve_note_set_output       → notes.sqlite (same schema as the phone)
                   5. retention: sve_notes_audio_older_than_json → unlink → sve_note_clear_audio

 hostapd + dnsmasq  →  device Wi‑Fi "Summaverick-XXXX", no internet route
 tiny HTTP server   →  https://summaverick.local  (password from sve_setting_get("web.password_hash"))
                       pages: list/search (sve_notes_list_json), note (sve_note_get_json),
                       ask (sve_ask), settings (language, retention), export .md
 USB‑C mass storage →  optional: expose notes export folder read‑only
```

Settings keys shared between the app and the device (so a backup moves between them):
`lang`, `retentionDays`, `web.password_hash`, `model.selected`.

Hardware‑enforced guardrails that belong on the carrier board, not in software: the recording LED wired to the microphone's power rail; recording starts only from the physical button; a chime on start. The daemon adds a session cap and an auto‑stop on long silence.

## Files

- `include/sve_core.h` — the C API (read this first)
- `src/sve_core.cpp` — llama.cpp wrapper, chunking/merging, whisper backend
- `src/sve_prompts.h` — every prompt; the only file to touch for output quality
- `src/sve_store.*` — SQLite schema, FTS5 search, retention queries
- `cli/sve_cli.cpp` — command‑line front end (also the device daemon's skeleton)
