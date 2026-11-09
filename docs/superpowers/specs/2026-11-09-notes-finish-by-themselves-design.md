# Notes that finish by themselves — design

Owner, 2026-11-09: "once recording has stopped regardless of where the user is this should work without any issues,
if there are real concerns it should not fail instead should have a pause and continue option but that too not
acceptable always. transcribing must be much faster. then summary and notes whenever they are ready it should
notify user and should not abruptly fail."
Owner's choice after the E2B test: keep Gemma 4 E4B for transcription (option A); get the speed from doing the work
during the recording and from making each piece cheaper, never from a less accurate model.

## What the logs showed (owner's iPhone 16e, iOS 27, 2026-11-09)

1. A regenerate in the background was ended by iOS 42 s after the last progress move. The heartbeat waited 30 s
   and only fired from model callbacks; iOS marks a continued-processing task stalled after about 30 s. Fixed
   (afc52f7): a timer moves the progress every 10 s while the work is alive. A 14-minute run then survived.
2. A Regenerate tapped while another note was running did nothing and said nothing.
3. The continued-processing task asked for by that tap was kept after its work ended: it sat at 100% for a minute,
   was reused for the next recording's Stop, its progress went from 100% back to 0%, and iOS ended it a minute later.
4. During an 11-minute recording, live transcription waited behind another note's notes run (the model does one run
   at a time). After Stop, 70% of the transcription was still to do.
5. Tapping a notification ended the app ("Call must be made on main thread"). Fixed (2cd4380).
6. E2B vs E4B on 24 pieces (scratchpad asr_ab/report.html): E2B 2.3× faster but changed or lost numbers in 3 of 24
   pieces. Rejected for transcription.

## Goals

1. **Never "failed" for an interruption.** After Stop, whatever iOS does (expiry, heat, app killed, locked), the
   work is kept to the last finished piece, section or output and continues by itself at the next chance: the
   automatic BGProcessingTask, the next continued-processing task, or the moment the app is opened. A real failure
   (no speech in the recording, the audio file gone) is the only "failed". An interruption shows at most one quiet
   notice, "continues by itself", never "tap to try again".
2. **A notification as each part is ready**, with the app not in front: "Transcript ready: <title>", then
   "Summary ready: <title>", then "Notes ready: <title>" (detailed notes and questions). Same identifier per note, so
   each replaces the one before; one per stage, never per percent.
3. **Faster, same accuracy (E4B).**
   - While recording, live transcription comes before every other model run (another note's notes, memories,
     backfill). Other work waits for a gap or for the recording to end.
   - After Stop the order is: the remaining transcript, then the missing sections, then the title and summary,
     then the detailed notes, then the questions. Each is notified as it lands (goal 2).
   - Each piece costs less: profile where a 28 s piece's time goes on the phone's CPU (audio encoder, prefill of
     the prompt and previous-piece context, decoding) and cut it where the transcript does not get worse.
     Candidates: audio encoder quantisation, thread count per stage, previous-piece context length, output token cap.
     A change is kept only if, on the asr_ab pieces, its number/amount/name differences against today's E4B are
     no worse than E4B's own Mac-vs-phone run-to-run differences.
4. **One background task per run.** The continued-processing task ends the moment its work is done (progress at
   100% only then). A new Stop or a new tap asks for a new task. Progress never goes backwards inside a task.
5. **A person's tap is never lost.** Regenerate, Try again and Generate notes tapped while another note runs are
   queued, shown on the note as "Waiting: another note is being written", and run next. A regenerate cut off by iOS
   is kept (which outputs, which note) and resumes like any other cut-off work; the old text stays until the new
   text is complete.

## Non-goals and anti-goals

- No smaller model for transcription (E2B measured and rejected). No cloud. No background GPU (not on iPhone).
- No silent-audio or other workarounds of iOS background rules.
- Heat: the serious/critical rules stay (rest at serious, pause at critical); this is the phone's own safety
  signal. Only the order of work changes.
- Never "Try again" for something the app can retry by itself.

## Honest limits (to tell the owner, and to design for)

- E4B on the CPU transcribes at about 1.1–1.25× real time locked. An 81-minute recording therefore has a backlog of
  roughly 10–20 minutes of transcription at Stop, plus sections and the three outputs. Goal 3's piece-cost work is
  the only lever on that with the same model; its size is not known until profiled.
- iOS decides when the automatic BGProcessingTask runs (seen today: started at once on the charger, ended after one
  minute). Without the app opened, a cut-off note may wait until iOS grants time.

## Done when (owner's iPhone)

- An 80-minute recording, locked after Stop and left alone: transcript, summary and notes notifications arrive in
  order; no "failed" anywhere; if iOS cut it off, it finished by itself later.
- A Regenerate tapped while another note runs shows "Waiting", then runs and notifies.
- A recording made while another note is being written: live transcription keeps its usual lag (logs).
- The profiled per-piece time and the accepted speed changes are written down with their accuracy check.

## Left to the builder

Exact copy beyond the strings above (en + te), the persistence format for queued and cut-off regenerates, and how
the profile is taken (core timing logs are fine).
