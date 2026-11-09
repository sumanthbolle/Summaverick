// Prompt templates for the Summaverick Everyday core.
// Written for small (0.6B–2B) instruction models: short, explicit, fixed output shape,
// and we always say "only use what is in the transcript" to cut hallucination.
#pragma once
#include <string>

namespace sve {

inline std::string language_name(const std::string &code) {
    if (code == "hi") return "Hindi";
    if (code == "te") return "Telugu";
    if (code == "ta") return "Tamil";
    if (code == "kn") return "Kannada";
    if (code == "ml") return "Malayalam";
    if (code == "mr") return "Marathi";
    if (code == "bn") return "Bengali";
    if (code == "gu") return "Gujarati";
    if (code == "pa") return "Punjabi";
    if (code == "or") return "Odia";
    if (code == "ur") return "Urdu";
    if (code == "zh") return "Chinese";
    if (code == "ms") return "Malay";
    if (code == "ta-SG") return "Tamil";
    return "English";
}

inline std::string system_prompt(const std::string &lang) {
    // "/no_think" switches Qwen3 to non‑thinking mode; harmless for other models.
    return "You are Summaverick, a careful note-taker. You only use information that is present in the "
           "transcript you are given. You never invent names, numbers, dates or facts. If something is "
           "unclear in the transcript, leave it out. The transcript may mix languages (for example Telugu "
           "with English terms) and may contain mis-heard words: understand all of it and correct obvious "
           "mis-hearings from context. Write in " + language_name(lang) + "; keep important terms from other "
           "languages in brackets in their original script. /no_think";
}

// Transcript text from a piece heard with conf < 0.80 starts with "[unclear] " (sve::kUnclearTag, N7 rev 1); every
// prompt that may see such text says what the tag means.
inline const char *unclear_rule() {
    return "Text marked [unclear] was hard to hear: do not state anything from it as fact; skip it or say the passage "
           "was unclear.";
}

// ---- per‑chunk extraction (used when a transcript does not fit one context) ----
inline std::string chunk_prompt(const std::string &chunk, int idx, int total) {
    return "This is part " + std::to_string(idx + 1) + " of " + std::to_string(total) +
           " of a transcript of a recording (a lecture, meeting, consultation or personal note).\n"
           "List the important points from THIS PART ONLY as short bullet points. Include every fact, "
           "definition, number, name, date, decision, instruction and task that is mentioned. "
           "Do not add anything that is not in the text. " + unclear_rule() + "\n\nTranscript part:\n\"\"\"\n" + chunk + "\n\"\"\"\n\nBullet points:";
}

// ---- final outputs ----

// Rules of the detailed notes and the revision questions (N7, owner audit of note 19: the notes inverted
// statements, turned "too many" into "two more" and "resignation" into "registration", gave the host's question
// as the guest's claim, and made an action item out of garbled speech; rev 1: "CAC (Civil Action Committee)" was
// an invented expansion, and an ad was given as a key point). The output language rules stay in system_prompt.
inline std::string faithful_rules(bool action_items) {
    std::string r = std::string("Rules:\n- ") + unclear_rule() + "\n"
                    "- Skip any other passage that is garbled; do not guess what it means.\n";
    if (action_items) r += "- Write an action item only when a speaker asks for it or commits to it.\n";
    r += "- Keep who said what: a host's or interviewer's question is not the guest's claim.\n"
         "- Never reverse a statement or drop a negation (not, never, no).\n"
         "- Keep numbers, names and terms exactly as in the transcript.\n"
         "- Never expand an abbreviation the speaker did not expand.\n"
         "- Leave out advertisements and sponsor messages.\n";
    return r;
}
inline std::string short_summary_prompt(const std::string &material, bool from_bullets) {
    return std::string("Below ") + (from_bullets ? "are bullet points extracted from" : "is") +
           " a transcript of a recording.\n"
           "Write a short summary of at most 6 sentences that a person could read to remember what happened. "
           "Mention who/what it was about, the main points, and any decisions or next steps. "
           "Use only what is in the text. " + unclear_rule() + "\n\n\"\"\"\n" + material + "\n\"\"\"\n\nShort summary:";
}

inline std::string detailed_notes_prompt(const std::string &material, bool from_bullets) {
    return std::string("Below ") + (from_bullets ? "are bullet points extracted from" : "is") +
           " a transcript of a recording.\n"
           "Turn it into organised notes using exactly this structure (keep the headings, leave a heading as "
           "'None' if nothing applies):\n\n"
           "## Topic\n(one line)\n\n"
           "## Key points\n- bullet points, grouped by sub-topic, keep every definition, number, name and date\n\n"
           "## Decisions and instructions\n- anything that was decided, prescribed, assigned or promised\n\n"
           "## Action items\n- [ ] task — who — when (only if stated; only what a speaker asked for or committed to)\n\n"
           "## Dates and numbers mentioned\n- …\n\n"
           "## Open questions\n- things left unresolved\n\n"
           "Use only what is in the text.\n" + faithful_rules(true) + "\n\"\"\"\n" + material + "\n\"\"\"\n\n## Topic";
}

inline std::string revision_questions_prompt(const std::string &material, bool from_bullets) {
    return std::string("Below ") + (from_bullets ? "are bullet points extracted from" : "is") +
           " a transcript of a lesson or discussion.\n"
           "Write 6 revision questions a student could use to test themselves on this material, from easy to hard. "
           "Every answer must come from the text; do not invent anything.\n" + faithful_rules(false) +
           "Use exactly this format, with a blank line between questions:\n\n"
           "Q1. <question about the text>\nA1. <answer taken from the text>\n\n"
           "Q2. <question about the text>\nA2. <answer taken from the text>\n\n"
           "(continue to Q6)\n\nText:\n\"\"\"\n" + material + "\n\"\"\"";
}

inline std::string ask_prompt(const std::string &context, const std::string &question) {
    return "Use the notes below to answer the question. If the notes do not contain the answer, say "
           "\"The recording does not mention that.\" Keep the answer short.\n\nNotes:\n\"\"\"\n" + context +
           "\n\"\"\"\n\nQuestion: " + question + "\nAnswer:";
}

inline std::string title_prompt(const std::string &excerpt) {
    return "Give a title of at most 8 words for a recording that starts like this. Reply with the title only.\n\n\"\"\"\n" +
           excerpt + "\n\"\"\"\n\nTitle:";
}

} // namespace sve
