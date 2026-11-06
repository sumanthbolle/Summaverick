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
 * One sve_ctx must only be used from one thread at a time.
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

/* Optional log sink (defaults to stderr). */
typedef void (*sve_log_cb)(int level, const char *msg, void *user);
void sve_set_log(sve_log_cb cb, void *user);

/* ---- lifecycle ---------------------------------------------------------- */

const char *sve_version(void);

/* Opens (or creates) the notes database. db_path may be ":memory:" for tests. */
sve_ctx *sve_init(const char *db_path);
void sve_free(sve_ctx *ctx);

/* ---- language model ---------------------------------------------------- */

/* Loads a GGUF model. n_ctx 0 => 4096. n_threads 0 => auto. n_gpu_layers: 99 on iOS (Metal), 0 on the device. */
int  sve_load_llm(sve_ctx *ctx, const char *gguf_path, int n_ctx, int n_threads, int n_gpu_layers);
void sve_unload_llm(sve_ctx *ctx);
int  sve_llm_loaded(const sve_ctx *ctx);

/* Generates one output kind from a transcript. Long transcripts are chunked, summarised
 * per chunk, then merged, so this works for a 90‑minute lecture on a 4096‑token context.
 * lang is a BCP‑47 code ("en", "hi", "te"); the model is asked to write in that language.
 * Returns a malloc'd UTF‑8 string, or NULL on failure/cancel. */
char *sve_generate(sve_ctx *ctx, const char *transcript, sve_output_kind kind, const char *lang,
                   sve_progress_cb cb, void *user);

/* Free‑form question over a transcript or set of notes ("what was the homework?").
 * context_text should be the transcript (or concatenated note outputs). */
char *sve_ask(sve_ctx *ctx, const char *context_text, const char *question, const char *lang,
              sve_progress_cb cb, void *user);

/* Suggests a short title (<= 8 words) for a transcript. */
char *sve_suggest_title(sve_ctx *ctx, const char *transcript, const char *lang);

/* ---- audio understanding (audio-capable models such as Gemma 4) -------- */

/* Loads the audio projector (mmproj GGUF) that belongs to the loaded LLM. Call after sve_load_llm;
 * loading another LLM unloads it. Returns 0 on success, negative if unsupported on this build. */
int  sve_load_audio(sve_ctx *ctx, const char *mmproj_path);
void sve_unload_audio(sve_ctx *ctx);
int  sve_audio_loaded(const sve_ctx *ctx);

/* Transcribes a 16 kHz mono 16‑bit WAV with the loaded model, in ~28 s pieces. Handles mixed
 * languages (e.g. Telugu with English terms): each word is written in the script it was spoken in.
 * Progress phase "chunk" with done/total = pieces. Returns transcript, or NULL on failure/cancel. */
char *sve_transcribe_audio(sve_ctx *ctx, const char *wav_path, sve_progress_cb cb, void *user);

/* Same as sve_transcribe_audio, as JSON pieces: [{"start_sec":0,"text":"…"},…] (start in whole seconds). */
char *sve_transcribe_audio_pieces(sve_ctx *ctx, const char *wav_path, sve_progress_cb cb, void *user);

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
int   sve_note_clear_transcript(sve_ctx *ctx, int64_t id);

/* ---- personal assistant (Gemma 4) --------------------------------------
 * JSON in/out. Rows use the store's column names; NULL columns are JSON null. Functions returning
 * char* return NULL on error. Nothing proposed by the model is saved until sve_suggestion_accept. */

/* Transcript pieces of a note. pieces_json: [{"start_sec":0,"text":"…"}]. Replaces existing pieces. */
int   sve_note_set_pieces(sve_ctx *ctx, int64_t note_id, const char *pieces_json);
/* [{"idx":0,"start_sec":0,"text":"…"}] */
char *sve_note_pieces(sve_ctx *ctx, int64_t note_id);

/* Runs extraction on a note's pieces (greedy, windowed for long recordings). Adds pending suggestions
 * (reminders, tasks, profile changes, vocabulary, facts), each quote and medicine checked in code; links
 * topics and people directly. Returns the number of suggestions added (0 if the model output was unusable),
 * negative on error or cancel. Pending suggestions from an earlier run on the same note become "superseded". */
int   sve_extract(sve_ctx *ctx, int64_t note_id, sve_progress_cb cb, void *user);

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

/* kind "fact" | "preference" | "vocab" | NULL/"" = all: [{"id","kind","text","note_id","created_at"}] */
char *sve_memory_json(sve_ctx *ctx, const char *kind);
int   sve_memory_update(sve_ctx *ctx, int64_t id, const char *text);
int   sve_memory_delete(sve_ctx *ctx, int64_t id);
/* Adds a vocabulary word the person fixed (applied directly). Returns its id (existing id if known). */
int64_t sve_vocab_add(sve_ctx *ctx, const char *term);

/* Deletes everything the assistant stored, in one transaction: the profile, medicines, memory (vocabulary,
 * preferences, facts), reminders, tasks, topics and their note links, suggestions, decisions, insights,
 * transcript pieces, and the settings keys the assistant created ("profile", "insight.last_date").
 * Notes and other settings are kept; the host deletes notes and cancels scheduled notifications.
 * Returns 0, or -1 on error (nothing deleted). */
int   sve_assistant_wipe(sve_ctx *ctx);

/* Ask across all recordings: {"answer":"","sources":[{"note_id":0,"start_sec":0,"title":""}],
 * "not_found":false,"medicine":false}. Medicine questions are answered in code from the profile and quoted
 * transcript pieces, never generated. Answers are in the profile language. */
char *sve_assistant_ask(sve_ctx *ctx, const char *question, sve_progress_cb cb, void *user);

/* At most one insight per calendar day, from recent notes and open tasks: {"id":0,"text":""} or "null". */
char *sve_insight_generate(sve_ctx *ctx, int64_t note_id);
/* Newest undismissed {"id","text","note_id","dismissed","created_at"} or "null". */
char *sve_insight_latest(sve_ctx *ctx);
int   sve_insight_dismiss(sve_ctx *ctx, int64_t id);

/* ---- device settings (small key/value store, used by the device web UI and the apps) ---- */
int   sve_setting_set(sve_ctx *ctx, const char *key, const char *value);
char *sve_setting_get(sve_ctx *ctx, const char *key); /* NULL if unset */

/* ---- speech recognition (device build only) ---------------------------- */
#ifdef SVE_WITH_WHISPER
int   sve_load_asr(sve_ctx *ctx, const char *whisper_model_path, int n_threads);
void  sve_unload_asr(sve_ctx *ctx);
/* 16 kHz mono 16‑bit PCM WAV. lang "auto" lets whisper detect. Returns transcript. */
char *sve_transcribe_wav(sve_ctx *ctx, const char *wav_path, const char *lang,
                         sve_progress_cb cb, void *user);
#endif

/* ---- memory ------------------------------------------------------------ */
void sve_free_string(char *s);

#ifdef __cplusplus
}
#endif
#endif /* SVE_CORE_H */
