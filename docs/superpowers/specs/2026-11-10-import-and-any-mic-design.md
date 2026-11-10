# Import recordings and record from any mic — design

Status: draft for the owner's approval (2026-11-10).

## Why

The owner's selling point is a cheap pocket mic that records all day, while Summaverick on the phone does the
transcript, notes and memory, all on the device. Today the app only records from the phone's own mic, using the
red Record button. Students also already have recordings in WhatsApp, Voice Memos and Files from class.

## Goals

1. **Import** one or many recordings into Summaverick, which then become notes the same way a recording does
   (transcript, summary, notes, questions, memories, 1/2/3-of-3 notifications).
2. **Record from any mic** connected to the phone (Bluetooth, USB-C, wired), with the mic in use shown.
3. Leave room for the owner's own mic syncing files by itself later.

## Non-goals (for now)

- Our own mic's firmware and Bluetooth sync (later; the import path below is what it will feed).
- Video files' pictures (only their sound is used).
- Cloud links (Google Drive web links, YouTube). Only files that are on the phone or shared to it.

## Anti-goals

- Never uploading anything. Import copies the file into the app; nothing leaves the phone.
- Never losing the original because a conversion failed: the copy stays until its note exists.
- Import never blocks recording: a Record tap always wins, and imports wait.

## How people get there

1. **Deep dive → "Import"** (a button at the top beside the search): it opens the Files picker, allows choosing
   several files, and covers iCloud Drive, On My iPhone, USB-C drives and SD-card readers.
2. **Share menu**: in WhatsApp, Voice Memos, Files, Telegram or a mic's own app, tap Share → Summaverick.
   A small share extension copies the file into the app's shared folder and says "Added to Summaverick".
   The app makes the notes the next time it runs (it opens, or overnight on the charger).
3. **Open in**: tapping an audio file in Files → Summaverick.

After choosing, a short sheet lists the files with their length and date. It offers a language
(the notes language by default) and one button, "Make notes". The sheet tells the person: "Notes take
about a minute for every 3 minutes of sound while the app is open."

## Behaviour

- **Formats:** m4a, mp3, wav, aac, caf, aiff, and the sound of mp4/mov. WhatsApp voice notes (.opus, in an Ogg
  file): check on the phone first whether iOS opens them. If it doesn't, add a small Opus decoder (libopus, BSD
  licence) to the core. Any other file gets: "I can't open this kind of file yet."
- **Conversion:** to the app's own format (16 kHz mono 16-bit WAV, as `AudioRecorder` writes), streamed in
  chunks so a 3-hour file never sits in memory. Afterwards it is compressed to m4a as recordings are now.
- **Date:** the note's time is the file's own recording date when the file has one (its creation date, or the
  Voice Memos name date), otherwise the time of import. The note shows "Imported" with the original name.
- **Queue:** imports go into the same queue as recordings, oldest first, one note at a time.
  - Notifications go per note, with "3 of 12 imported" across a batch.
  - When the phone is hot or the battery is low, the heat ladder applies as it does today.
- **Duplicates:** a file already imported (same length and same first and last 64 KB) asks
  "You imported this on <date>. Import again?"
- **Long files:** these use the long-recording path already built (sections, chapters).
- **Space:** before copying, check the free space (file size × 3 for the WAV and m4a); if short, say how much
  to free.

## Any mic

- The Record screen shows the mic in use: "iPhone mic", "AirPods Pro", "USB-C mic"… When there is more than
  one, tapping it opens a picker (`AVAudioSession.availableInputs` / `setPreferredInput`).
- Bluetooth mics record at the quality Bluetooth allows (HFP), which is fine for speech. USB-C and wired mics
  give full quality.
- If the mic disconnects mid-recording, the app switches to the iPhone mic and keeps recording. The note marks
  the moment ("Mic changed at 12:30").
- The choice is remembered per mic name.

## Later: our own mic

The pocket mic will hand finished files to the same import queue, through a Bluetooth sync that keeps each file
on the mic until the phone confirms it. Nothing in this design blocks that: the queue takes a file plus its
recorded date.

## Open questions for the owner

1. Is "Import" in Deep dive right, or should it go on the Record button (long-press → "Import a recording")?
2. When a batch is big (say 10 class recordings), is "notes while the app is open, the rest overnight on the
   charger" right?

## For the builder

- Files: a new `Audio/AudioImporter.swift` (pure decision parts tested with swiftc checks: formats, the
  duplicate key, the date choice, the space check), a share extension target, `RecordView` mic picker, Deep dive
  button, and pipeline entry `process(audio:duration:recordedAt:origin:)`.
- Every new string in English and Telugu, in the voice guide's tone.
