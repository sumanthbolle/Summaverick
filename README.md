# Summaverick Everyday

Your personal note-taker. Record a lecture, a meeting, a doctor's instructions or a thought; get a short summary, structured notes and revision questions, written entirely on the device. No account, no server, no subscription.

This repository has two parts, and the split is deliberate:

```
core/   portable C++ engine — notes store (SQLite + full-text search), summariser (llama.cpp + Qwen),
        optional speech backend (whisper.cpp). This is what moves to Android and to the
        Summaverick Everyday hardware device unchanged.
ios/    SwiftUI app — recording, Apple on-device speech recognition, UI. Thin by design.
```

## What works today

| Step | iPhone (this build) | Summaverick Everyday device (next iteration) |
|---|---|---|
| Record | AVAudioRecorder → 16 kHz mono WAV | mic + button, same WAV format |
| Speech → text | Apple on-device recogniser (`requiresOnDeviceRecognition = true`, never the server) | `core` whisper backend, or IndicConformer for Indian languages |
| Title, summary, notes, questions, "ask" | `core` via llama.cpp + Qwen3 on Metal | identical `core` on the RK3576 CPU/NPU |
| Storage + search | `core` SQLite/FTS5 in the app sandbox | identical `core`, same database file |
| Access control | Face ID / passcode (LocalAuthentication) | device password on the local web UI |
| Audio retention | 7 days default, notes kept | same setting, same code path |

The core was compiled and run on Linux against llama.cpp `c6a0ff1` (2026‑11‑04) and whisper.cpp master with Qwen3‑0.6B‑Q8_0: summary, notes, revision questions, question answering and the long‑transcript chunk‑and‑merge path all produce grounded output (see `core/README.md` for the sample run). The iOS layer could not be compiled here (no Xcode on Linux), so expect to fix a handful of compiler nits on first build.

## Personal assistant

With the Gemma 4 E4B model selected, the app also works as a personal assistant for students, children and older people who speak Telugu, English or a mix of the two. All of it runs on the phone.

- **Today** greets you and lists today's reminders, open to-dos and one insight a day drawn from your recordings.
- **Deep dive** groups recordings by topic and by person. Each topic has a summary, key terms and a short quiz, and every point links back to the moment in the recording it came from.
- **Ask** answers questions, typed or spoken, from your own recordings only. Each answer links to its source, and if the recordings do not contain the answer the app says so.
- **Suggestions wait for Yes.** After a recording, reminders, to-dos and facts appear as cards on Today. Nothing is saved and no reminder rings until you tap Yes. Edit lets you change a card first; No discards it.
- **Medicine checks.** A medicine suggestion is compared with the doctor's exact words and with the medicines in your profile. If the name, dose or time does not match, the card shows a yellow "Please check" banner and asks "Save anyway?" before saving. Answers about medicines quote the recording rather than rephrasing it.
- **Learning memory.** The app remembers words you save and offers a preference when you make the same correction more than once, such as moving a reminder time. Review or delete these under Profile → What Summaverick has learned.

With a Qwen model selected, notes still work, but Today and Ask show "Needs Gemma 4".

### Tests

The core tests need only `clang++` and SQLite, which come with the Xcode command-line tools:

```bash
bash core/tests/run.sh                 # unit tests: store, extraction, medicine checks, learning
bash core/tests/smoke_gemma.sh api     # C API checks; no model needed
```

The model smoke tests link against Homebrew's llama.cpp (`brew install llama.cpp`) and run Gemma 4 E4B. Point them at the model and audio projector files:

```bash
export SVE_GEMMA=$(ls ~/.cache/huggingface/hub/models--unsloth--gemma-4-E4B-it-GGUF/snapshots/*/gemma-4-E4B-it-Q4_K_M.gguf)
export SVE_MMPROJ=$(ls ~/.cache/huggingface/hub/models--ggml-org--gemma-4-E4B-it-GGUF/snapshots/*/mmproj-gemma-4-E4B-it-Q8_0.gguf)

bash core/tests/smoke_gemma.sh extract                            # suggestions, Ask, topic summary, insight
SVE_TEST_WAV=clip.wav bash core/tests/smoke_gemma.sh transcribe   # 16 kHz mono WAV to transcript
```

`extract` reads `core/tests/fixtures/doctor_visit.txt` unless `SVE_FIXTURE` names another file. Its `EXPECT-OK` and `EXPECT-MISS` lines are for a person to read; model output varies, so review it rather than treating a miss as a hard failure. After an iOS build, `python3 ios/tools/check_strings.py <derived-data-dir>` confirms that every string in the app is in the catalog with a Telugu value.

### Signing

The app needs one entitlement, Increased Memory Limit (`com.apple.developer.kernel.increased-memory-limit`), so that the 5 GB Gemma model fits in memory. It is the only extra entitlement a personal Apple team supports, so the app is built to need nothing else. Adding other capabilities in Xcode will break signing with a personal team.

## Build the iOS app (on your Mac)

Requirements: Xcode 16+, an iPhone/iPad on iOS 17+, [Homebrew](https://brew.sh).

```bash
cd ios
./get-llama-xcframework.sh        # downloads prebuilt llama.xcframework (Metal) from the latest llama.cpp release
open SummaverickEveryday.xcodeproj
```

The `.xcodeproj` is included, generated by `ios/make_xcodeproj.py` (plain Python, no XcodeGen). If you add or rename Swift files later, run `python3 make_xcodeproj.py` from `ios/` to refresh it. (`project.yml` is kept for people who prefer XcodeGen, but it is not needed.)

In Xcode: select the `SummaverickEveryday` target → Signing & Capabilities → pick your Team. Plug in your iPhone, select it as the run destination, press Run.

If the llama C API has drifted since the pinned commit and the core fails to compile, pin the framework to a release from early November 2026: `./get-llama-xcframework.sh bXXXX` (tags are listed at github.com/ggml-org/llama.cpp/releases).

### First run

1. Settings (gear) → **AI model** → Download. Qwen3 0.6B (640 MB) is the default; on a 6 GB+ device pick Qwen3 1.7B for better notes. This is the only time the app touches the network.
2. Settings → **Language**. The list only shows languages Apple can recognise fully on your phone. If Hindi or Telugu is missing, add it under iOS Settings → General → Keyboard → Dictation and relaunch; if it is still missing, that language needs the whisper/IndicConformer backend (iteration 2, see below).
3. Tap **Record**, talk for a minute, tap **Stop and make notes**. The transcript appears within seconds; the three outputs follow one by one.

Keep the app in the foreground while notes are being written. iOS gives a backgrounded app ~30 s, after which the pipeline pauses; the note keeps the transcript and you can tap **Generate notes** on it to finish.

### The first real test

Record a 15‑minute lesson or meeting in English. Then check three things: is the transcript accurate enough to be useful, do the notes contain only things that were said, and would you actually revise from the questions. That is the milestone that decides the next iteration, not the UI.

## Test the core without a phone

```bash
git clone https://github.com/ggml-org/llama.cpp   deps/llama.cpp
git clone https://github.com/ggml-org/whisper.cpp deps/whisper.cpp
cmake -S core -B core/build -DCMAKE_BUILD_TYPE=Release && cmake --build core/build --target sve -j
curl -L -o qwen.gguf https://huggingface.co/Qwen/Qwen3-0.6B-GGUF/resolve/main/Qwen3-0.6B-Q8_0.gguf

core/build/sve notes --llm qwen.gguf --kind all transcript.txt
core/build/sve ask   --llm qwen.gguf "what was the homework?" transcript.txt
core/build/sve transcribe --asr ggml-base.bin --lang hi recording.wav     # whisper backend
core/build/sve store --db notes.db add --title "Biology" transcript.txt
core/build/sve store --db notes.db list "mitochondria"
```

On a Mac the same build works (`brew install cmake`) and runs on Metal.

## Where the next iterations go

- **Indian languages on iPhone**: compile `whisper.xcframework` (whisper.cpp ships one too) and switch the pipeline to `sve_transcribe_wav` when Apple's recogniser lacks the language. For Hindi/Telugu quality, IndicConformer via sherpa‑onnx is the better backend; the C API already has the slot for it.
- **Better notes in Hindi/Telugu**: have the model extract structured fields and fill a template in the target language instead of asking a 0.6B model to write fluent prose (`core/src/sve_prompts.h` is the only file that changes).
- **Android**: JNI over `sve_core.h`, `AudioRecord` → WAV, same pipeline order.
- **The device**: `core/README.md` describes how the engine maps onto the Summaverick Everyday hardware.

## Privacy rules baked into the code

- Speech recognition always sets `requiresOnDeviceRecognition`; there is no server fallback path.
- The only network call is the model download in `ModelManager`.
- Audio is deleted after the retention window; transcripts and notes stay until deleted by the user.
- Model files are excluded from iCloud backup; the notes database lives in Application Support (backed up with the phone, encrypted at rest by iOS when the phone is locked).
- A consent reminder is shown before the first recording and can be re‑enabled in Settings.
