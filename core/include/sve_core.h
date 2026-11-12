/*
 * Summaverick Everyday — portable core (C API)
 *
 * This is the part of the product that moves between hosts unchanged:
 *   - iOS app today (compiled into the app target, linked against llama.xcframework)
 *   - Android app later (JNI over the same header)
 *   - the Summaverick Everyday chip device later (Linux/ARM, same code + whisper backend,
 *     fronted by a small web UI served over the device's own Wi‑Fi hotspot)
 *
 * Responsibilities:
 *   1. Notes store   — SQLite with full‑text search (recall / "what did she say about…")
 *   2. Summariser    — llama.cpp running a small Qwen GGUF, producing three outputs per recording:
 *                      short summary, detailed notes, revision questions
 *   3. ASR (optional) — whisper.cpp backend, compiled only when SVE_WITH_WHISPER is defined.
 *                      On iOS the host supplies transcripts from Apple's on‑device recogniser,
 *                      on the device this backend does the job.
 *
 * All strings are UTF‑8. Every char* returned by the library must be released with sve_free_string().
 * Functions return 0 on success and a negative value on failure unless stated otherwise.
 * One sve_ctx must only be used from one thread at a time. Several contexts may open the same database file
 * and be used from different threads at once (SQLite WAL; a writer waits up to 5 s for another's write).
 */
#ifndef SVE_CORE_H
#define SVE_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SVE_VERSION "0.1.0"

typedef struct sve_ctx sve_ctx;

/* Output kinds for a note. Stored per note; each can be generated independently. */
typedef enum {
    SVE_OUT_SHORT_SUMMARY = 0,
    SVE_OUT_DETAILED_NOTES = 1,
    SVE_OUT_REVISION_QUESTIONS = 2
} sve_output_kind;

/* Progress callback for long generations. phase: "chunk", "merge", "final".
 * done/total are chunk counters. Return non‑zero to cancel. */
typedef int (*sve_progress_cb)(const char *phase, int done, int total, void *user);
/* A new piece of streamed text (UTF-8, whole characters). Return non-zero to cancel. */
typedef int (*sve_text_cb)(const char *piece, void *user);

/* Optional log sink (defaults to stderr). */
typedef void (*sve_log_cb)(int level, const char *msg, void *user);
void sve_set_log(sve_log_cb cb, void *user);

/* ---- lifecycle ---------------------------------------------------------- */

const char *sve_version(void);

/* Opens (or creates) the notes database. db_path may be ":memory:" for tests. Loads no model, so a context
 * that never calls sve_load_llm is a store-only context: a host can keep one for quick reads and writes while
 * another context on the same file runs long model calls. Each call reads SQLite afresh (no per-context
 * cache), so either context sees the other's committed rows. Opening a file that is already up to date
 * writes nothing. */
sve_ctx *sve_init(const char *db_path);
void sve_free(sve_ctx *ctx);

/* ---- language model ---------------------------------------------------- */

/* Loads a GGUF model. n_ctx 0 => 4096. n_threads 0 => auto. n_gpu_layers: 99 on iOS (Metal), 0 on the device. */
int  sve_load_llm(sve_ctx *ctx, const char *gguf_path, int n_ctx, int n_threads, int n_gpu_layers);
/* Pair writing (sve_section_outputs_pair) on (1, the default) or off (0). Applies from the next sve_load_llm: on, the
 * model loads with two sequences in one unified KV memory; off, with one, and sve_section_outputs_pair always writes
 * the two kinds by two single calls. Turning it off also stops pairs at once with a model already loaded. */
void sve_set_pair_enabled(sve_ctx *ctx, int on);
void sve_unload_llm(sve_ctx *ctx);
/* CPU threads for the next model calls (the heat ladder eases them off): n_decode for generation, n_batch for
 * prefill. 0 => the load-time rule (decode up to 4 cores; batch up to 8, never fewer than decode). The audio
 * encoder keeps its threads until sve_set_audio_threads. 0, or -1 with no model. */
int  sve_set_threads(sve_ctx *ctx, int n_decode, int n_batch);
int  sve_llm_loaded(const sve_ctx *ctx);
/* 1 when a run on the GPU failed since the model was loaded: load it again before the next run (the Metal state stays broken). */
int  sve_llm_needs_reload(const sve_ctx *ctx);

/* Generates one output kind from a transcript. Long transcripts are chunked, summarised
 * per chunk, then merged, so this works for a 90‑minute lecture on a 4096‑token context.
 * lang is a BCP‑47 code ("en", "hi", "te"); the model is asked to write in that language.
 * An output that reaches its token cap ends on its last whole sentence or bullet (also sve_generate_from_sections).
 * Returns a malloc'd UTF‑8 string, or NULL on failure/cancel. */
char *sve_generate(sve_ctx *ctx, const char *transcript, sve_output_kind kind, const char *lang,
                   sve_progress_cb cb, void *user);

/* 1 when sve_generate takes the whole transcript in one prompt (no chunking), 0 when it would have to
 * summarise it first, -1 with no model or bad input. Same budget as sve_generate (needs the model's tokenizer).
 * Notes and title come from the transcript when it fits, from the section summaries only when it does not. */
int sve_transcript_fits(sve_ctx *ctx, const char *transcript, const char *lang);

/* The text to give sve_generate and sve_transcript_fits for a note (N7 rev 1): its pieces
 * ([{"start_sec","text","conf"?}], as sve_note_pieces returns them) joined by newlines and trimmed, a piece heard
 * with conf < 0.80 starting with "[unclear] " (the notes prompts say not to state it as fact). With no such piece it
 * is the stored transcript. Pure; NULL on bad input. */
char *sve_notes_material(const char *pieces_json);

/* Free‑form question over a transcript or set of notes ("what was the homework?").
 * context_text should be the transcript (or concatenated note outputs). */
char *sve_ask(sve_ctx *ctx, const char *context_text, const char *question, const char *lang,
              sve_progress_cb cb, void *user);

/* Suggests a short title (<= 8 words) for a transcript. */
char *sve_suggest_title(sve_ctx *ctx, const char *transcript, const char *lang);

/* ---- audio understanding (audio-capable models such as Gemma 4) -------- */

/* Loads the audio projector (mmproj GGUF) that belongs to the loaded LLM. Call after sve_load_llm;
 * loading another LLM unloads it. The projector runs where the LLM runs: on the GPU when the LLM was
 * loaded with n_gpu_layers > 0, else on the CPU. Returns 0 on success, negative if unsupported on this build. */
int  sve_load_audio(sve_ctx *ctx, const char *mmproj_path);
void sve_unload_audio(sve_ctx *ctx);
int  sve_audio_loaded(const sve_ctx *ctx);
/* CPU threads of the audio encoder for the next pieces: n, or the model's prefill threads now when n is 0.
 * mtmd has no runtime setter, so the projector is loaded again (same file) only when the count differs; the
 * time it takes is logged. Call between runs, never during one. 0 ok; -1 with no model or no projector loaded;
 * -2 when the reload failed (the projector is then loaded again with its old threads, else unloaded). */
int  sve_set_audio_threads(sve_ctx *ctx, int n);

/* Transcribes a 16 kHz mono 16‑bit WAV with the loaded model, in ~28 s pieces. Handles mixed
 * languages (e.g. Telugu with English terms): each word is written in the script it was spoken in.
 * Progress phase "chunk" with done/total = pieces. Returns transcript, or NULL on failure/cancel. */
char *sve_transcribe_audio(sve_ctx *ctx, const char *wav_path, sve_progress_cb cb, void *user);

/* Same as sve_transcribe_audio, as JSON pieces: [{"start_sec":0,"text":"…"},…] (start in whole seconds). */
char *sve_transcribe_audio_pieces(sve_ctx *ctx, const char *wav_path, sve_progress_cb cb, void *user);

/* Incremental transcription of a WAV that may still be being written (the data size in its header may be
 * 0, 0xFFFFFFFF or larger than the file; the whole frames on disk are used). Plans the next ~28 s piece
 * from start_sample with the same cut rule, prompt, vocabulary line and previous-text context as
 * sve_transcribe_audio_pieces (prev_text: the previous piece's text, may be NULL), and transcribes it.
 * Returns JSON:
 *   {"status":"piece","start_sec":N,"end_sample":N,"text":"…"}  continue from end_sample (text may be "")
 *   {"status":"wait"}  not final and under 28 s + 3 s recorded after start_sample: call again later
 *   {"status":"end"}   nothing left (final: under 0.5 s left)
 * "conf" (in the piece): mean token probability, NaN when no token. A piece under 0.80 is heard once more (same
 * samples, prompt and previous text; sampled with a fixed seed) and the text with the higher conf is kept.
 * is_final: the recording has stopped; the last piece takes what is left. While not final, a file whose
 * header is missing, incomplete or not yet valid is a "wait". NULL on failure or cancel. */
char *sve_transcribe_next_piece(sve_ctx *ctx, const char *wav_path, int64_t start_sample, int is_final,
                                const char *prev_text, sve_progress_cb cb, void *user);

/* The audio model (Gemma) on exactly [start_sample, end_sample) of a finished WAV (at most 28 s): the same prompt,
 * vocabulary line, previous-text context (prev_text, may be NULL) and second hearing under 0.80 as
 * sve_transcribe_next_piece, without planning a cut. For the pieces sve_whisper_next_piece marks needs_gemma.
 * Returns {"text":"…","conf":F} (conf NaN when no token), or NULL on bad input, a span past the end of the file,
 * no audio model, failure or cancel. */
char *sve_transcribe_span(sve_ctx *ctx, const char *wav_path, int64_t start_sample, int64_t end_sample,
                          const char *prev_text, sve_progress_cb cb, void *user);

/* ---- notes store ------------------------------------------------------- */

/* Creates a note. audio_path/transcript may be NULL. Returns new id (>0) or negative error. */
int64_t sve_note_create(sve_ctx *ctx, const char *title, const char *lang, const char *audio_path,
                        const char *transcript, int64_t duration_seconds);

int sve_note_set_transcript(sve_ctx *ctx, int64_t id, const char *transcript);
int sve_note_set_output(sve_ctx *ctx, int64_t id, sve_output_kind kind, const char *text);
int sve_note_set_title(sve_ctx *ctx, int64_t id, const char *title);
int sve_note_delete(sve_ctx *ctx, int64_t id);

/* Returns the note as JSON:
 * {"id":1,"title":"…","lang":"en","created_at":1730000000,"duration_seconds":900,
 *  "audio_path":"…"|null,"transcript":"…"|null,
 *  "short_summary":"…"|null,"detailed_notes":"…"|null,"revision_questions":"…"|null} */
char *sve_note_get_json(sve_ctx *ctx, int64_t id);

/* Lists notes newest first as a JSON array of {id,title,lang,created_at,duration_seconds,has_audio,preview}.
 * query may be NULL/empty (all notes) or a full‑text query matched against title, transcript and outputs. */
char *sve_notes_list_json(sve_ctx *ctx, const char *query, int limit);

/* Retention: returns a JSON array of {"id":…, "audio_path":"…"} for notes whose audio is older
 * than max_age_days. The host deletes the files, then calls sve_note_clear_audio for each. */
char *sve_notes_audio_older_than_json(sve_ctx *ctx, int max_age_days);
int   sve_note_clear_audio(sve_ctx *ctx, int64_t id);
/* Points the note at another audio file (the recording after it was compressed). 0 ok, -1 no such note. */
int   sve_note_set_audio_path(sve_ctx *ctx, int64_t id, const char *audio_path);
int   sve_note_clear_transcript(sve_ctx *ctx, int64_t id);

/* ---- personal assistant (Gemma 4) --------------------------------------
 * JSON in/out. Rows use the store's column names; NULL columns are JSON null. Functions returning
 * char* return NULL on error. Nothing proposed by the model is saved until sve_suggestion_accept. */

/* Transcript pieces of a note. pieces_json: [{"start_sec":0,"text":"…"}]. Replaces existing pieces. */
int   sve_note_set_pieces(sve_ctx *ctx, int64_t note_id, const char *pieces_json);
/* [{"idx":0,"start_sec":0,"text":"…"}] */
char *sve_note_pieces(sve_ctx *ctx, int64_t note_id);

/* Section summaries of a note (~5 minute parts of a long recording). sections_json:
 * [{"idx":0,"first_piece":0,"last_piece":10,"start_sec":0,"end_sec":308,
 *   "summary":{"title","topic","points":[{"text","start_sec"}],"terms":[],"hints":[]}}]. Replaces all.
 * Removed with the note and by sve_note_clear_transcript. Fails for a note that does not exist. */
int   sve_note_set_sections(sve_ctx *ctx, int64_t note_id, const char *sections_json);
/* Same array; "[]" when none. */
char *sve_note_sections(sve_ctx *ctx, int64_t note_id);

/* Plans pieces ([{"start_sec":0,"text":"…"}], in order) into ~5 minute sections; returns half-open piece index
 * ranges "[[first,last),…]" ("[]" when none). The last piece ends at end_sec_of_last (live: cursor seconds;
 * after Stop: the note duration). Not final: only ranges that are closed are returned, an open tail waits. Pure. */
char *sve_plan_sections(const char *pieces_json, int64_t end_sec_of_last, int is_final);

/* Summary of one section. pieces_json: [{"start_sec":0,"text":"…"}]; lang "en"/"te". Returns the summary JSON
 * ({"title","topic","points":[{"text","start_sec"}],"terms","hints"}). Sampled first try, one greedy retry,
 * then a plain-text fallback ("fallback":true) built from the first sentences, so a model that ran never
 * gives NULL. NULL on cancel, no model or bad input. Output capped at 512 tokens (3 to 6 points). Pieces with a
 * "conf" under 0.80 are marked [unclear] in the prompt; saved words the pieces do not say are left out of the prompt,
 * and a term, hint or point that names one is dropped. */
char *sve_section_summarize(sve_ctx *ctx, const char *pieces_json, const char *lang, sve_progress_cb cb, void *user);

/* Notes built from a note's stored section summaries instead of the raw transcript. One group of summaries that
 * fits: one model call (same prompts and budget as sve_generate). Several: each group is summarised first, then the
 * combined result is written. NULL when the note has no sections (callers then use sve_generate), no model, or on
 * cancel/failure (callers check their own cancel flag first). */
char *sve_generate_from_sections(sve_ctx *ctx, int64_t note_id, sve_output_kind kind, const char *lang,
                                 sve_progress_cb cb, void *user);
/* Detailed notes (SVE_OUT_DETAILED_NOTES) or revision questions (SVE_OUT_REVISION_QUESTIONS) of ONE section of a note,
 * written from that section's pieces with its summary as a guide ("This is part i of n of the recording."), so a
 * long recording's notes are made a part at a time and a stopped run keeps the parts it finished. Sampled first,
 * one greedy retry. Output capped at 440 tokens (notes) / 300 (questions), then the same checks as sve_generate;
 * a reply with no readable question is a failure (retried, not stored). A reply cut at the cap is stored marked cut.
 * The result is stored for the section's current piece range and lang (sve_section_outputs_state) and returned;
 * a section with no speech stores and returns "". The try is counted before the model runs (a cancel takes it
 * back; tries in another lang start the count again); a failure records its reason. NULL on cancel, with no
 * model, for a note or section that is gone, other kinds, or when the store refuses the result (the note was
 * deleted or its sections re-planned meanwhile): nothing is stored then. */
char *sve_section_output(sve_ctx *ctx, int64_t note_id, int idx, sve_output_kind kind, const char *lang,
                         sve_progress_cb cb, void *user);
/* The detailed notes and the revision questions of ONE section written together: the part's shared prompt (system,
 * its place, its summary as a guide and its material) is read once into one sequence, copied to a second, each
 * kind's instruction is appended to its own, and both are sampled independently, one decode per token round, until
 * each ends or reaches its cap (440 / 300). Each reply gets sve_section_output's checks and is stored as soon as it
 * ends; one that fails them gets sve_section_output's greedy retry alone afterwards. Both tries are counted before
 * the model runs, per kind and lang, as sve_section_output counts them. When the context has one sequence, the part
 * must be reduced first, it is silence, or the prefix, both instructions and both caps do not fit the context, the
 * two kinds are written by two sve_section_output calls instead. Returns how many kinds were stored (0-2), -2 on
 * cancel (outputs already stored stay; the tries of the others are taken back), -1 with no model, for a note or
 * section that is gone, or on error. */
int sve_section_outputs_pair(sve_ctx *ctx, int64_t note_id, int idx, const char *lang, sve_progress_cb cb, void *user);
/* A part written while the recording is still going (the note does not exist yet): as sve_section_output, from the
 * section's own pieces_json ([{"start_sec","text","conf"?}]) and its summary_json (as sve_section_summarize returns
 * it), with "This is part <idx + 1> of the recording." (no total). Stores and counts nothing: the host keeps the reply
 * and puts it in the note with sve_section_output_put. *cut (optional) is 1 when the reply reached its cap. "" for a
 * part with no speech. NULL on cancel, with no model, for other kinds or bad input, or when both tries failed. */
char *sve_section_output_text(sve_ctx *ctx, const char *pieces_json, const char *summary_json, int idx,
                              sve_output_kind kind, const char *lang, int *cut, sve_progress_cb cb, void *user);
/* sve_section_outputs_pair for a part written while the recording is going (material as sve_section_output_text):
 * {"notes":{"text","cut"}|null,"questions":{"text","cut"}|null,"cancelled":bool}. A kind is null when it was not
 * written (failed, or not reached before a cancel); one finished before a cancel is kept. Stores nothing. NULL with
 * no model, on bad input or error. */
char *sve_section_outputs_pair_text(sve_ctx *ctx, const char *pieces_json, const char *summary_json, int idx,
                                    const char *lang, sve_progress_cb cb, void *user);
/* Stores a part written earlier (sve_section_output_text) for the note's section idx, only when that section's range
 * is [first_piece, last_piece] now. 1 stored (done, as a stored part), 0 when the range differs (or there is no such
 * section, or the note is gone; the normal steps write it again), -1 on error or another kind. Counts no try. */
int sve_section_output_put(sve_ctx *ctx, int64_t note_id, int idx, sve_output_kind kind, const char *lang,
                           int first_piece, int last_piece, const char *text, int cut);
/* [{"idx":0,"kind":1,"done":true,"attempts":1,"last_error":null,"failures":0},…]: every section x {1 notes,
 * 2 questions}, by idx then kind. done only when the stored part's lang, first_piece and last_piece match the section
 * now. attempts (tries started, a kill included), failures (tries that failed for a real reason: not a cancel, a
 * kill or a GPU refusal) and last_error (why the last try failed, or null) count only tries in lang. "[]" when no
 * sections; NULL on error. */
char *sve_section_outputs_state(sve_ctx *ctx, int64_t note_id, const char *lang);
/* The note's detailed notes (a "## <section title>" per part, one merged "## Action items") or revision questions
 * (numbered across parts, near-duplicates dropped, at most 12, every part represented) from its stored parts in
 * lang. Pure, no model. NULL when any part is missing (or no sections, or another kind); "" when every part is empty. */
char *sve_assemble_output(sve_ctx *ctx, int64_t note_id, sve_output_kind kind, const char *lang);
/* As sve_assemble_output, while parts are still missing: in the notes a missing part gets its "## <section title>"
 * with placeholder under it (and the action items never say "None"); in the questions it adds none. NULL only when
 * no part at all is done (or no sections, or another kind). placeholder NULL = "". */
char *sve_assemble_output_with_gaps(sve_ctx *ctx, int64_t note_id, sve_output_kind kind, const char *lang,
                                    const char *placeholder);
/* Deletes the stored parts and their try counts of a note: kind 1 notes, 2 questions or -1 all kinds; idx a part or
 * -1 all parts. The sections stay. Returns the rows deleted (parts plus try rows), -1 on error. */
int   sve_section_outputs_clear(sve_ctx *ctx, int64_t note_id, int kind, int idx);
/* Title from the section titles (same cleanup as sve_suggest_title). NULL when no sections or no model. */
char *sve_title_from_sections(sve_ctx *ctx, int64_t note_id, const char *lang);
/* Chapters [{"title","start_sec"}]: consecutive sections on the same topic merged. Pure, no model; "[]" when none. */
char *sve_chapters_json(sve_ctx *ctx, int64_t note_id);

/* Runs extraction on a note's pieces (greedy, windowed for long recordings). Adds pending suggestions
 * (reminders, tasks, profile changes, vocabulary; facts are memories now, see sve_memories_from_section),
 * each quote and medicine checked in code; links
 * topics and people directly. Returns the number of suggestions added (0 if the model output was unusable),
 * negative on error or cancel. Pending suggestions from an earlier run on the same note become "superseded". */
int   sve_extract(sve_ctx *ctx, int64_t note_id, sve_progress_cb cb, void *user);
/* 1 when extraction has run on the note (a run with usable output sets the settings key
 * "extracted.<id>"; a note with suggestions or topic links from before that key also counts), 0 when
 * never, -1 on error. The key goes with the note and with sve_assistant_wipe. */
int   sve_note_extracted(sve_ctx *ctx, int64_t note_id);

/* Suggestions with status ("pending", "accepted", "rejected"; NULL/"" = all):
 * [{"id","kind":"reminder|task|profile|vocab|fact","status","payload":{…},"quote","note_id","start_sec",
 *   "medicine_id","check_status":"ok|check"|null,"check_fields":["dose",…],"created_at"}] */
char *sve_suggestions_json(sve_ctx *ctx, const char *status);
/* Accepts a pending suggestion. edited_payload_json (NULL = as proposed) overrides payload keys and is
 * recorded for learning. Creates the row: reminder -> reminder id, task -> task id, profile -> medicine id
 * (stop_medicine: the deleted medicine's id, 0 if none matched; routine: memory id), vocab/fact -> memory id.
 * -2: stale suggestion re-flagged; refresh and ask again (a reminder or profile add/update card whose dose
 * no longer matches the saved medicine, when medicine.dose was not edited and the card has no "dose" flag:
 * nothing is applied, it stays pending with "dose" in check_fields). Accepting a medicine card with a new
 * dose supersedes the other pending cards for that medicine.
 * A reminder whose final times are empty is refused (-1); times are never filled in on accept.
 * -1 on other errors (unknown id, not pending, bad JSON). */
int64_t sve_suggestion_accept(sve_ctx *ctx, int64_t id, const char *edited_payload_json);
int   sve_suggestion_reject(sve_ctx *ctx, int64_t id);

/* Profile: {"name","lang":"en|te","style","routine":{"wake","breakfast","lunch","dinner","bed":"HH:MM"}}.
 * Get returns "null" when unset. Set takes a JSON object (the person's own edit, applied directly). */
char *sve_profile_get(sve_ctx *ctx);
int   sve_profile_set(sve_ctx *ctx, const char *json);

/* [{"id","name","dose","when_text","times":["20:00"],"repeat":"daily|twice_daily|weekly|once","until_date","created_at"}] */
char *sve_medicines_json(sve_ctx *ctx);
/* Saves a medicine the person typed ({"id":0 = new, "name", "dose", "when_text"|"when", "times", "repeat",
 * "until_date"}) and adds a pending reminder suggestion for it. Returns the medicine id, negative on error.
 * "times" given (even []) is kept as given; only a missing "times" key is resolved from "when" against the
 * routine (see sve_resolve_times). A card with no times is flagged "check" on "times". */
int64_t sve_medicine_save(sve_ctx *ctx, const char *json);
/* Routine times implied by the words of "when" (e.g. "after dinner" -> the profile's dinner time), as JSON
 * ["HH:MM",…] sorted, [] if no keyword or no routine time. The one keyword table the apps use. NULL on error. */
char *sve_resolve_times(sve_ctx *ctx, const char *when);
/* Deletes a medicine and its reminders and suggestions. */
int   sve_medicine_delete(sve_ctx *ctx, int64_t id);

/* [{"id","text","times","repeat","start_date","until_date","note_id","start_sec","medicine_id","from_deleted","active","created_at"}] */
char *sve_reminders_json(sve_ctx *ctx, int active_only);
int   sve_reminder_set_active(sve_ctx *ctx, int64_t id, int active);
/* Creates a reminder the person made themselves (e.g. "Make reminder" on an Ask answer; their own edit,
 * applied directly, no suggestion). json: {"text","times":["HH:MM",…],"repeat","start_date","until_date",
 * "note_id","start_sec","medicine_id","source_text"}. Requires non-empty text and at least one time; every time must be a valid HH:MM.
 * repeat daily|twice_daily|weekly|once (missing/empty = "once"); start_date YYYY-MM-DD (missing/empty =
 * today); until_date YYYY-MM-DD or missing, not before start_date; note_id/start_sec optional (a note that
 * no longer exists is dropped with its start_sec). source_text optional: the words it was made from (e.g. the
 * Ask answer). medicine_id optional; if given it must exist and be the only saved medicine named in
 * text + source_text (see sve_medicines_named), else -1. If not given and the text names exactly one saved
 * medicine, the reminder is linked to it (dose change, stop and delete of that medicine then reach it).
 * Returns the reminder id, negative on bad input or error. */
int64_t sve_reminder_create(sve_ctx *ctx, const char *json);
/* Ids of the saved medicines named in text (whole words, after the core's medicine normalisation: Telugu
 * digits, number words, "Telma40" = "Telma 40"), as a JSON array, e.g. [3]. NULL on error. */
char *sve_medicines_named(sve_ctx *ctx, const char *text);
/* [{"id","text","who","due_date","done","note_id","start_sec","from_deleted","created_at"}] */
char *sve_tasks_json(sve_ctx *ctx, int open_only);
int   sve_task_set_done(sve_ctx *ctx, int64_t id, int done);
/* Creates a to-do the person made themselves (applied directly). json: {"text","who","due_date","note_id",
 * "start_sec"}. Requires non-empty text; due_date YYYY-MM-DD or missing; note_id/start_sec as for
 * sve_reminder_create. Returns the task id, negative on bad input or error. */
int64_t sve_task_create(sve_ctx *ctx, const char *json);

/* kind "topic" | "person" | NULL/"" = both:
 * [{"id","kind","name","summary","summary_stale","note_count","open_tasks","last_title"}] */
char *sve_topics_json(sve_ctx *ctx, const char *kind);
/* [{"id","title","created_at","short_summary"}] newest first */
char *sve_topic_notes_json(sve_ctx *ctx, int64_t topic_id);
int   sve_topic_rename(sve_ctx *ctx, int64_t topic_id, const char *name);
int   sve_topic_merge(sve_ctx *ctx, int64_t from_id, int64_t into_id);
/* {"summary":"","key_terms":[{"term":"","meaning":""}],"quiz":[{"q":"","a":""}],"lang":"en"} from the
 * topic's notes; cached until a new recording joins the topic or another lang is asked for. */
char *sve_topic_summary(sve_ctx *ctx, int64_t topic_id, const char *lang, sve_progress_cb cb, void *user);

/* kind "fact" | "preference" | "vocab" | NULL/"" = all: [{"id","kind","text","note_id","created_at"}].
   Facts come from the memory layer with negative ids; update/delete accept those ids. */
char *sve_memory_json(sve_ctx *ctx, const char *kind);
int   sve_memory_update(sve_ctx *ctx, int64_t id, const char *text);
int   sve_memory_delete(sve_ctx *ctx, int64_t id);
/* Adds a vocabulary word the person fixed (applied directly). Returns its id (existing id if known). */
int64_t sve_vocab_add(sve_ctx *ctx, const char *term);

/* ---- memories (the second brain) ---------------------------------------
 * Memory row JSON: {"id","type":"fact|decision|promise|question|person|idea","text","who","to_whom",
 * "due_date","due_text","said_date","checked":[{"kind","text"}],"confidence":"high|low",
 * "review":"new|kept|corrected|wrong","queue","health","done","importance","origin":"recording|chat|medicine|migrated",
 * "medicine_id","suggestion_id","superseded_by","conflict_with","first_seen","last_seen",
 * "sources":[{"note_id","start_sec","quote","title"}],"people":[{"id","name","role"}]}.
 * A saved medicine has one memory (origin "medicine", review "kept", health 1, text "<name> <dose>, <when>"),
 * kept in step with the medicine and deleted with it. */

/* Makes the memories of one section of a note: one greedy memory-only model call (a reply cut off keeps its
 * whole items), each candidate checked against the section's own words (sve_memory), double-checked
 * by the model where needed (a "no" drops it; only a "yes" lets it skip the review queue), stored with its
 * quote as source, linked to people and the note's topic, and to a pending reminder/task card of the same
 * note and quote. Consolidated with the memories already known (sve_consolidate.h): the same memory said
 * again only gains this quote as a source, a later version supersedes the older one ("change" in
 * sve_memory_get), a contradiction is stored with conflict_with and queues both. A name that is clearly a known
 * person links to them; a name that only may be (checked by the model, phase "person_check") becomes a new
 * person plus a question (sve_person_questions_json) unless the model says no: people are never merged silently.
 * Marks the section done; a section already done returns 0 and changes nothing.
 * Returns the number of memories stored (a repeat counts once, as the row it joined); -1 for no such note or section, bad input, or no model (for a section
 * with words); -2 on cancel (nothing stored, the section stays due); -3 when the memory model call itself fails
 * (not a cancel: nothing stored, the section stays due). A reply that comes back but is empty or unusable marks
 * the section done with 0. The notes and section summaries are not touched: the section summary prompt and its
 * output are the same as before memories existed. */
int   sve_memories_from_section(sve_ctx *ctx, int64_t note_id, int section_idx, sve_progress_cb cb, void *user);
/* Indexes of the note's sections whose memories are not made yet, e.g. "[0,2]"; "[]" when none. */
char *sve_memory_sections_due(sve_ctx *ctx, int64_t note_id);
/* Note ids with a section whose memories are not made yet, newest note first: "[12,9]". */
char *sve_notes_needing_memories(sve_ctx *ctx, int limit);
/* Memory rows. filter_json (NULL = {}): {"note_id","types":[],"reviews":[],"queue":0|1,"include_wrong",
 * "include_superseded","limit"}. Newest first; wrong and superseded rows only when asked for. */
char *sve_memories_json(sve_ctx *ctx, const char *filter_json);
/* One memory row plus "history" (the versions it replaced), or "null". With history it also has
 * "change": {"was","now","said"}, the checked words that changed from the version it replaced directly and
 * the date the new one was said (absent when no number or date changed). */
char *sve_memory_get(sve_ctx *ctx, int64_t id);
/* The person's review: state "kept" or "wrong". Both take it out of the review queue. 0 ok, -1 error. */
int   sve_memory_review(sve_ctx *ctx, int64_t id, const char *state);
/* The person's own words for a memory, applied directly: review becomes "corrected", out of the queue.
 * Medicine memories are changed by saving the medicine instead (-1). 0 ok, -1 error. */
int   sve_memory_edit(sve_ctx *ctx, int64_t id, const char *text);
/* The mind map as JSON (see sve_brainmap.h): {"center","list","branches","links"}. note_id 0 = all recordings,
 * else only that recording's memories. Current memories that are kept or corrected, or waiting in the review
 * queue and neither health-related nor a conflict (each item's "waiting": true, drawn lighter); never wrong or
 * a medicine's. NULL on error. */
char *sve_brain_map_json(sve_ctx *ctx, int64_t note_id);
/* Memories under a person or topic ("Who is this?" for a person), by importance, same rows as sve_memories_json,
 * and only those the map shows (so counts match), each with "waiting" (in the review queue, not checked yet).
 * Kept before waiting at equal importance. note_id > 0: only that recording's. */
char *sve_topic_memories_json(sve_ctx *ctx, int64_t topic_id, int64_t note_id /* 0 = all */);
/* Marks a memory as used just now (last_used), which raises its importance. 0 ok, -1 error. */
int   sve_memory_touch(sve_ctx *ctx, int64_t id);
/* Deletes a memory (not a medicine's: delete the medicine). 0 ok, -1 error. */
int   sve_memory_remove(sve_ctx *ctx, int64_t id);
int   sve_memory_set_done(sve_ctx *ctx, int64_t id, int done);
/* Memories waiting for the person (queue 1, any review but "wrong": a kept memory a later recording
 * contradicts waits again; "conflict_with" names the other row), newest recording first, each with "suggestion":
 * the linked pending suggestion object, or null. */
char *sve_review_queue_json(sve_ctx *ctx);
/* "Is this the same person?": pending questions about a new name that may be someone already known, oldest
 * first: [{"id","a":{"id","name","context"},"b":{"id","name","context"}}] (a is the known person, b the new
 * one; context is what was said about them, may be ""). NULL on error. */
char *sve_person_questions_json(sve_ctx *ctx);
/* The person's answer. same != 0 merges b into a (a keeps its name; notes, memories and other questions move
 * to a); 0 keeps them apart and the pair is not asked again. 0 ok, -1 error (no such pending question). */
int   sve_person_question_answer(sve_ctx *ctx, int64_t id, int same);

/* Deletes everything the assistant stored, in one transaction: the profile, medicines, memory (vocabulary,
 * preferences, facts), reminders, tasks, topics and their note links, person questions, suggestions, decisions, insights,
 * transcript pieces, and the settings keys the assistant created ("profile", "insight.last_date", "extracted.<id>").
 * Notes and other settings are kept; the host deletes notes and cancels scheduled notifications.
 * Returns 0, or -1 on error (nothing deleted). */
int   sve_assistant_wipe(sve_ctx *ctx);

/* Ask across all recordings: {"answer":"","sources":[{"note_id":0,"start_sec":0,"title":""}],
 * "not_found":false,"medicine":false}. Medicine questions are answered in code from the profile and quoted
 * transcript pieces, never generated. Answers are in the profile language. */
char *sve_assistant_ask(sve_ctx *ctx, const char *question, sve_progress_cb cb, void *user);

/* Ask as a chat: messages_json is [{"role":"user"|"assistant","text":"…"}] with the newest (the question, a
 * "user" turn) last. An assistant turn may carry "sources" ([{"title","start_sec"}], kept as text for the model)
 * and "medicine":true (its text is then never handed to the model). The last turn may carry "quiz_asked": the quiz
 * ids already asked in this chat. Earlier turns go with the question within 1500 tokens, newest first. The answer is
 * written in the question's language (Telugu when Telugu script is at least 30% of its letters, else English);
 * lang_hint ("te"/"en", may be NULL) is used only for a question with no letters, then the profile's.
 * Returns the JSON of sve_assistant_ask plus "lang" and "kind", chosen by sve::route_question:
 * "medicine" (built in code, quoted, never streamed); "due" (reminders and to-dos of 7 days, in code);
 * "quiz" ("question", "answer" = the expected answer, "source", "quiz_id"; none left: not_found and "quiz_done");
 * "summary" (stored summaries of the notes of the period, each note a source; empty period: not_found);
 * "recordings" (retrieval over pieces and stored summaries); "general" (the model's own knowledge, no sources).
 * A question the health check flags, or the turn after one, never calls the model: the best passage word for word
 * ("medicine":true), or a fixed line saying to ask a doctor or pharmacist (kind "health" when the question itself was
 * about health; pass "kind":"health" back on that assistant turn). Health turns are never in the model's prompt.
 * Before the first piece of a model answer cb gets the phase
 * "kind:<kind>", so a stopped answer keeps its kind.
 * on_text (may be NULL) gets the visible answer as it is written, a sentence at a time, thinking and [S1] labels
 * removed; its pieces join to "answer" when the answer is the model's. No sentence that mentions a medicine is ever
 * streamed. "answer" is the final text to show. on_text or cb returning non-zero cancels (NULL). NULL also on error,
 * or with no model when the question needs one. */
char *sve_assistant_chat(sve_ctx *ctx, const char *messages_json, const char *lang_hint,
                         sve_text_cb on_text, void *user, sve_progress_cb cb, void *cbuser);
/* The kind sve_assistant_chat's code rule gives a question ("medicine", "quiz", "due", "summary", "recordings"), for the
 * host: "due", "quiz" and "medicine" need no model, so sve_assistant_chat can run them on a store-only context at once. */
char *sve_route_kind(sve_ctx *ctx, const char *question);

/* At most one insight per calendar day, from recent notes and open tasks: {"id":0,"text":""} or "null".
 * cb (may be NULL) is called during generation; returning non-zero cancels it (returns NULL, nothing stored). */
char *sve_insight_generate(sve_ctx *ctx, int64_t note_id, sve_progress_cb cb, void *user);
/* Newest undismissed {"id","text","note_id","dismissed","created_at"} or "null". */
char *sve_insight_latest(sve_ctx *ctx);
int   sve_insight_dismiss(sve_ctx *ctx, int64_t id);

/* ---- meaning search (optional second model) ----------------------------
 * EmbeddingGemma 300M, loaded next to Gemma (never unloads it) and always on the CPU, so it also runs in the
 * background. Vectors (the first 256 values, normalised) are stored per transcript piece and per current memory
 * (not wrong, superseded, queued or a medicine's), under the model's file name; they are deleted with their piece
 * or memory, with the note, by sve_note_clear_transcript and by the assistant wipe, and a memory's when its text
 * changes. With no embedder loaded the search is the keyword search alone. */

/* 0 ok, -1 bad arguments, -2 the model did not load (no embedder is loaded then). n_threads 0 => auto. */
int   sve_load_embedder(sve_ctx *ctx, const char *gguf_path, int n_threads);
void  sve_unload_embedder(sve_ctx *ctx);
int   sve_embedder_loaded(const sve_ctx *ctx);
/* Embeds up to limit pieces (notes that still have their transcript, newest notes first) and then current memories
 * that have no vector of this model. Returns how many were stored (0 when none is left; an item whose text changed
 * meanwhile is not stored and stays pending), -1 with no embedder, on error, or when no item of a non-empty batch
 * could be embedded (stop calling), -2 when cb (phase "embed", may be NULL) returned non-zero. */
int   sve_embed_pending(sve_ctx *ctx, int limit, sve_progress_cb cb, void *user);
/* Search for Ask: [{"kind":"piece","note_id","start_sec","score"} | {"kind":"memory","memory_id","score"}], best
 * first, at most limit (10 when limit <= 0). The keyword search (top 20 pieces and top 20 current memories, the
 * question's keywords) fused with the meaning search (top 20 each by cosine) by reciprocal rank fusion (k 60).
 * With no embedder: the keyword order, pieces and memories interleaved by rank. NULL on error. */
char *sve_search_json(sve_ctx *ctx, const char *query, int limit);

/* ---- device settings (small key/value store, used by the device web UI and the apps) ---- */
int   sve_setting_set(sve_ctx *ctx, const char *key, const char *value);
char *sve_setting_get(sve_ctx *ctx, const char *key); /* NULL if unset */

/* ---- speech recognition: Whisper (whisper.cpp, built with SVE_WITH_WHISPER) ---- */
/* Always declared. Without SVE_WITH_WHISPER: sve_load_asr returns -1, sve_asr_loaded 0, and
 * sve_whisper_next_piece / sve_transcribe_wav return NULL. */

/* Loads a whisper.cpp model (ggml .bin). use_gpu: 1 runs it on Metal, 0 on the CPU (the GPU is not usable in the
 * background). Flash attention is on. n_threads <= 0: up to 4. Loading again replaces the loaded model.
 * Returns 0, -1 on bad input or no whisper in this build, -2 when the model does not load. */
int   sve_load_asr(sve_ctx *ctx, const char *whisper_model_path, int use_gpu, int n_threads);
void  sve_unload_asr(sve_ctx *ctx);
/* 1 when a Whisper model is loaded, else 0 (also for NULL). */
int   sve_asr_loaded(sve_ctx *ctx);

/* The next group of Whisper pieces of a WAV that may still be being written (same WAV rules as
 * sve_transcribe_next_piece). From start_sample, pieces of 4-10 s are cut at the quietest moment, up to 28 s in all.
 * Each piece's language is detected between Telugu and English: when any piece is not clearly Telugu, nothing is
 * decoded and the group is returned with needs_gemma true and text "" (give its samples to sve_transcribe_span).
 * Otherwise each piece is decoded as Telugu (greedy, no timestamps, no context, encoder sized to the piece), the
 * texts are joined with spaces, and a word or short phrase repeated more than 3 times in a row is cut to one.
 * Returns JSON:
 *   {"status":"piece","start_sec":N,"end_sample":N,"text":"…","conf":F,"needs_gemma":B}  continue from end_sample
 *   {"status":"wait"}  not final and under 28 s + 3 s recorded after start_sample: call again later
 *   {"status":"end"}   nothing left (final: under 0.5 s left)
 * conf: mean token probability over the text tokens, NaN when none. start_sec: start_sample / 16000 (whole s).
 * NULL on failure, with no model loaded, or on cancel (cb returning non-zero). */
char *sve_whisper_next_piece(sve_ctx *ctx, const char *wav_path, int64_t start_sample, int is_final,
                             sve_progress_cb cb, void *user);

/* 16 kHz mono 16‑bit PCM WAV, whole file. lang "auto" lets whisper detect. Returns transcript. */
char *sve_transcribe_wav(sve_ctx *ctx, const char *wav_path, const char *lang,
                         sve_progress_cb cb, void *user);

/* ---- memory ------------------------------------------------------------ */
void sve_free_string(char *s);

#ifdef __cplusplus
}
#endif
#endif /* SVE_CORE_H */
