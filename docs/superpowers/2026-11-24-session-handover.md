# Session handover — 2026-11-16 to 2026-11-24

Branch `feature/personal-assistant`. Everything below is committed except the signing files (see "Open decisions").
Phone: iPhone 16e, UDID 00008140-001668892E22801C, app id `summaverick.everyday` (new team S5ZM5YV87S).
Desktop tools for the next session: `core/tests/devtools/README.md`. Core tests: `bash core/tests/run.sh` (484 pass).

## What was built (newest first)

| Commit | What |
|---|---|
| (next) | Checking agent (sve_verify.h, sve_verify_note): after memories, every output checked (language, decisions/actions said, owner, noise, questions answerable, memory/action quotes); only the failing part rewritten with feedback, <= 2 retries, best kept, "Please check" banner on the note; each check/retry in activity.log (`check`, `check_end`) |
| 0c5c036 | Activity log names the note a run works on (installed 2026-11-24) |
| 8eb77c1 | Processing activity log (Application Support/Summaverick/activity.log), 5-min stall watchdog, Settings > Processing activity |
| 1c49c1d | Transcript pieces cleaned on save: loops collapsed, repeat of previous piece dropped, saved words written as speech dropped (note 18) |
| 7b992f2 | Action-item agent: finder, updater (stands/changed/cancelled + condition), coverage check (commitment lines re-asked), parent checks |
| 26491b5 | All prompts = short core + one skill (`core/src/sve_skills.h`); Ask falls back to the newest recording; compose route for "draft/write" |
| 01b13d2, 1d24ada | Meeting action items with owners/days (due_text resolved in code, clock_time), next meeting -> reminder; "Before your meeting" card on Today; recording Map tab opens on Flow (chapters -> decisions/actions/questions) |
| 858f1c0, a3080c1 | Speed record per note (speed.<id> in settings; note screen + Settings summary + CSV share); hot phone no longer rests as long as the model load |
| 4edd7fa, 50d46c8 | Add-a-reminder shortcut repeats daily/weekly; relations review card asks "Who decided/asked?" in one tap |
| d115ed6, 25e227c | Telugu number words with case endings, "no one", English names said in Telugu letters: memories no longer all dropped |
| f94ff13, d2c38eb | People in the graph are people (no companies/descriptions/unplaced words); one-time clean-ups at open |
| bb4c4c4, f48b29c | No made-up notes from < 4 words; no "###" titles; graph uses each recording's people/topics |
| 589725a | Saved words fix Shruti's mishearings (never medicines) |
| bf82bc8, 2b6015f, 30ba553 | Relations review + map links; Shortcuts (today, last note, reminder, ask); status never says "failed" for a heat wait |

## Measured on the Mac (Gemma 4 E4B Q4_K_M, the shipped build)

- Team meeting fixture (core/tests/fixtures/team_meeting.txt): 4 of 4 actions, right owners/days/lines, next meeting Fri 16:00.
- Proposal fixtures (proposal_named/unnamed/reversed/telugu.txt): Ask answers "What do I need to do?" correctly in all 4,
  drafts on "Draft a message..."; to-do keeps the condition in 3 of 4. OPEN: Telugu version leaves a duplicate
  "send Friday proposal".
- With the core instruction on the to-do finder, the meeting gave 1 of 4 in 3 of 3 runs, so the finder takes its skill
  without the core (documented in sve_skills.h). Sampling is temp 0.3: judge prompt changes over 2-3 runs.

## Phone facts (from devicectl DB/log pulls)

- Short recording, cool phone: notes ready in ~0.4x the recording (note 16: 5.2 min -> 2.1 min).
- Long or hot: note 17 (28 min) notes ready after 45 min (22.8 notes + 16.4 rests), battery 75 -> 45%.
- Note 18 (14 min, charging, hot): 11 of 32 pieces went to Gemma (Shruti empty), transcript 1.5x instead of 6-8x;
  Gemma wrote the vocab hints as speech on near-silence (fixed by 1c49c1d for new recordings; note 18 itself is bad).
- Action-item step on a short note: ~2.6 min with the app open (note 19).

## Next, in order (agreed with the owner)

1. Install 0c5c036 when the phone is back; watch the activity log during the next recording (see devtools README).
2. Set a loudness cut-off so near-silent pieces never go to Gemma: read `peak` of `piece` events in activity.log
   (`to_gemma` true) on real recordings first.
3. Fix the Telugu duplicate in the action agent (one_per_line / piece_of in sve_suggest.cpp).
4. DONE (checking agent). Mac, 5 fixtures x 3 runs (verify_eval): every first-check failure fixed in 1 retry, none
   left "Please check". Caught: wrong owner in notes (team_meeting en, 3/3), saved words written as speech in summary
   and notes (noisy_meeting, 3/3). Open: proposal_telugu flags one question per run (judge "no" on an inference
   question), costs ~4 s. Watch `check` events on the phone; cost there unmeasured yet.
5. Skills learn from the person's memory/corrections (a capped "what this person taught me" part per skill).
6. Knowledge graph as the retrieval index for Ask (people/topics/decisions/actions -> recordings and moments).
7. Long recordings: write each part's notes during recording (sections already summarised live).
8. Fold a meeting's action cards into the one review card; carry open actions into the next meeting's flow.

## Open decisions (the owner's)

- Signing: app moved to team S5ZM5YV87S / `summaverick.everyday`; old app `com.summaverick.everyday` (old notes) still
  on the phone. pbxproj, both entitlements and InfoPlist.xcstrings are uncommitted; `make_xcodeproj.py` still writes
  the old team/id and App Groups (do not run it). App Groups removed: the record control's state is dead.
- Keep 4 CPU cores while charging (option A) to roughly halve note time on the charger — asked, not answered.
- Read-aloud (Geeta te_IN / Indian-English voice): owner said pronunciation is fine; not built yet.

## Owner's working preferences seen this session

- Be proactive: fix what a check finds, verify, then report (memory: feedback-be-proactive).
- Wants agentic structure: skills per task, worker agents with a parent that checks and retries.
- Plain, outcome-first reports; tables of measured results land well.
