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
