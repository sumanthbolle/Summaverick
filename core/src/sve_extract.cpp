#include "sve_extract.h"

namespace sve {

using nlohmann::json;

std::string extraction_prompt(const std::string &context_block,
                              const std::string &transcript_with_markers,
                              const std::string &today_iso) {
    std::string p;
    p += "You read the transcript of a recording and propose reminders, tasks, profile changes, "
         "vocabulary, facts, topics and people for the person who owns this assistant.\n"
         "Reply with ONE JSON object and nothing else: no prose, no code fences.\n\n"
         "Today's date is " + today_iso + ". Use it to resolve relative dates such as \"next Saturday\" "
         "into YYYY-MM-DD.\n\n"
         "What is known about the person:\n" + context_block + "\n\n"
         "JSON shape (every key in English, exactly as written):\n"
         "{\"reminders\":[{\"text\":\"\",\"when_text\":\"\",\"times\":[\"HH:MM\"],"
         "\"repeat\":\"once|daily|twice_daily|weekly\",\"until_date\":\"YYYY-MM-DD|null\","
         "\"quote\":\"\",\"start_sec\":0,"
         "\"medicine\":{\"name\":\"\",\"dose\":\"\",\"frequency\":\"\",\"duration\":\"\"}|null}],"
         "\"tasks\":[{\"text\":\"\",\"who\":\"\",\"due_date\":\"YYYY-MM-DD|null\",\"quote\":\"\",\"start_sec\":0}],"
         "\"profile\":[{\"change\":\"add_medicine|update_medicine|stop_medicine|routine\",\"detail\":\"\","
         "\"quote\":\"\",\"start_sec\":0,"
         "\"medicine\":{\"name\":\"\",\"dose\":\"\",\"frequency\":\"\",\"duration\":\"\"}|null}],"
         "\"vocab\":[\"term\"],"
         "\"facts\":[{\"text\":\"\",\"quote\":\"\",\"start_sec\":0}],"
         "\"topics\":[\"name\"],"
         "\"people\":[\"name\"]}\n\n"
         "Rules:\n"
         "- Write every \"text\" and \"detail\" field in the person's preferred language from the context above. "
         "Keys and the fixed values (repeat, change) stay in English.\n"
         "- Only statements clearly about the listener (\"you\", \"your\") go into \"profile\". "
         "General talk about medicines never goes into \"profile\". "
         "Other people's medicines and tasks never go into \"profile\": only the listener's own.\n"
         "- \"quote\" must be copied word-for-word from the transcript.\n"
         "- Each \"quote\" covers exactly one medicine: the shortest sentence that states it.\n"
         "- \"start_sec\" is the number in the [t=SEC] marker of the transcript line the quote is in.\n"
         "- \"medicine.dose\" is written as \"<number> <unit>\" only, for example \"5 mg\" or \"2 tablets\", "
         "with nothing else.\n"
         "- Never fill in a field that was not said. Leave it as an empty string (or null where the shape allows null).\n"
         "- Use an empty array when nothing applies.\n"
         "- At most 3 topics and at most 5 people.\n\n"
         "Transcript (each line starts with its [t=SEC] marker):\n" + transcript_with_markers + "\n"
         "JSON:\n";
    return p;
}

// End index (inclusive) of the balanced {...} starting at start, or npos.
static size_t balanced_end(const std::string &s, size_t start) {
    int depth = 0;
    bool in_str = false, esc = false;
    for (size_t i = start; i < s.size(); ++i) {
        char c = s[i];
        if (in_str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') in_str = true;
        else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) return i;
    }
    return std::string::npos;
}

// First balanced {...} that parses as a JSON object; skips stray braces in prose.
static bool first_object(const std::string &s, json &out) {
    for (size_t start = s.find('{'); start != std::string::npos; start = s.find('{', start + 1)) {
        size_t end = balanced_end(s, start);
        if (end == std::string::npos) continue;
        try {
            json j = json::parse(s.substr(start, end - start + 1));
            if (j.is_object()) { out = std::move(j); return true; }
        } catch (...) {}
    }
    return false;
}

// Array under key, keeping only objects (want_obj) or only strings.
static json arr_of(const json &o, const char *key, bool want_obj) {
    json out = json::array();
    auto it = o.find(key);
    if (it == o.end() || !it->is_array()) return out;
    for (const auto &el : *it)
        if (want_obj ? el.is_object() : el.is_string()) out.push_back(el);
    return out;
}

Extracted parse_extraction(const std::string &model_output) {
    Extracted e;
    try {
        // Fences and leading prose sit outside the chosen object.
        json j;
        if (!first_object(model_output, j)) return e;
        e.reminders = arr_of(j, "reminders", true);
        e.tasks = arr_of(j, "tasks", true);
        e.profile = arr_of(j, "profile", true);
        e.vocab = arr_of(j, "vocab", false);
        e.facts = arr_of(j, "facts", true);
        e.topics = arr_of(j, "topics", false);
        e.people = arr_of(j, "people", false);
        e.ok = true;
    } catch (...) {
        return Extracted();
    }
    return e;
}

std::string pieces_with_markers(const json &pieces) {
    std::string out;
    try {
        if (!pieces.is_array()) return out;
        for (const auto &p : pieces) {
            if (!p.is_object()) continue;
            auto t = p.find("text");
            if (t == p.end() || !t->is_string()) continue;
            long long sec = 0;
            auto st = p.find("start_sec");
            if (st != p.end() && st->is_number_integer()) sec = st->get<long long>();
            out += "[t=" + std::to_string(sec) + "] " + t->get<std::string>() + "\n";
        }
    } catch (...) {}
    return out;
}

}  // namespace sve
