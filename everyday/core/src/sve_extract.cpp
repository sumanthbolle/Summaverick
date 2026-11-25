#include <vector>
#include <cctype>
#include <set>
#include "sve_extract.h"

#include <algorithm>

#include "sve_sections.h"

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
         "into YYYY-MM-DD. A weekday name alone (\"on Saturday\") means the first such day after today.\n\n"
         "What is known about the person:\n" + context_block + "\n\n"
         "JSON shape (every key in English, exactly as written):\n"
         "{\"reminders\":[{\"text\":\"\",\"when_text\":\"\",\"times\":[\"HH:MM\"],"
         "\"repeat\":\"once|daily|twice_daily|weekly\",\"start_date\":\"YYYY-MM-DD|null\",\"until_date\":\"YYYY-MM-DD|null\","
         "\"quote\":\"\",\"start_sec\":0,"
         "\"medicine\":{\"name\":\"\",\"dose\":\"\",\"frequency\":\"\",\"duration\":\"\"}|null}],"
         "\"tasks\":[{\"text\":\"\",\"who\":\"\",\"due_text\":\"\",\"due_date\":\"YYYY-MM-DD|null\",\"quote\":\"\",\"start_sec\":0}],"
         "\"next_meeting\":{\"text\":\"\",\"when_text\":\"\",\"quote\":\"\",\"start_sec\":0}|null,"
         "\"profile\":[{\"change\":\"add_medicine|update_medicine|stop_medicine|routine\",\"detail\":\"\","
         "\"quote\":\"\",\"start_sec\":0,"
         "\"medicine\":{\"name\":\"\",\"dose\":\"\",\"when_text\":\"\",\"frequency\":\"\",\"duration\":\"\"}|null,"
         "\"routine\":{\"wake|breakfast|lunch|dinner|bed\":\"HH:MM\"}|null}],"
         "\"vocab\":[\"term\"],"
         "\"facts\":[{\"text\":\"\",\"quote\":\"\",\"start_sec\":0}],"
         "\"topics\":[\"name\"],"
         "\"people\":[\"name\"]}\n\n"
         "What goes where:\n"
         "- \"reminders\": something to do at a time of day or on a schedule, above all taking a medicine. "
         "A one-time thing on a day, with no time of day, is a task, not a reminder.\n"
         "- \"tasks\": every one-time thing someone agreed or was asked to do: the listener's own and other "
         "people's action items, as in a meeting (\"I'll send the document by Friday\", \"Priya will fix the bug\"), "
         "such as bringing, buying, calling, sending, fixing, ordering or submitting something. One task per action. "
         "\"who\" is the person who will do it, as said, or \"me\" when it is the listener (\"I\", \"నేను\"); empty when "
         "nobody is named. On a line that starts with a speaker's name (\"Priya: నేను ... చేస్తాను\"), \"I\" and \"నేను\" "
         "are that speaker. \"due_text\" is the words that say when, copied exactly (\"by Monday\", \"Wednesday కల్లా\", "
         "\"tomorrow\"), and \"due_date\" the day they mean. Read to the end: a later line can change, put on hold or "
         "cancel an earlier plan; give each task as it stands at the end. Keep a condition in \"text\" (\"Send the "
         "proposal after Anil confirms the pricing\"); when the day no longer stands, leave \"due_text\" and \"due_date\" "
         "empty. \"We could ...\" is a suggestion, not a task.\n"
         "- \"next_meeting\": when the recording says when the group meets next (\"Next meeting Friday 4 PM\", "
         "\"మళ్ళీ సోమవారం కలుద్దాం\"): \"text\" names the meeting in a few words, \"when_text\" copies the words that "
         "say when. null when no next meeting is said.\n"
         "- \"facts\": advice or information worth remembering, such as a diet instruction or a test result.\n"
         "- \"profile\": a change to the listener's own medicines or daily routine stated in the transcript. "
         "Never propose what is already in the context above.\n"
         "- \"people\": names of individual people said in the recording, not the person this assistant is for. "
         "Never a company, product, organisation or app (those are topics), and never a description such as "
         "\"one of the managers\" or \"my doctor\".\n"
         "\n"
         "Rules:\n"
         "- Write every \"text\" and \"detail\" field in the person's preferred language from the context above, even when the transcript is in another language or mixed. "
         "Keys and the fixed values (repeat, change) stay in English.\n"
         "- Only statements clearly about the listener (\"you\", \"your\") go into \"profile\". "
         "General talk about medicines never goes into \"profile\". "
         "Other people's medicines never go into \"profile\": only the listener's own. "
         "Other people's action items do go into \"tasks\", with their name in \"who\".\n"
         "- \"quote\" must be copied word-for-word from the transcript.\n"
         "- Each \"quote\" covers exactly one medicine: the shortest sentence that states it.\n"
         "- \"when_text\" is the words of the quote that say when (for example \"after dinner\"), copied exactly. "
         "\"medicine.frequency\" and \"medicine.duration\" are also copied exactly from the quote "
         "(for example \"once a day\", \"for 5 days\"), or left empty.\n"
         "- A \"routine\" change gives the new time in \"routine\", for example {\"dinner\":\"21:00\"}; "
         "medicine changes leave \"routine\" null.\n"
         "- \"start_date\" only when the recording says the day to start (\"from today\" means today's date); "
         "otherwise null.\n"
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

bool first_json_object(const std::string &text, json &out) {
    try { return first_object(text, out); } catch (...) { return false; }
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
        // "No action items were stated" is never a task (owner, note 19).
        e.tasks.erase(std::remove_if(e.tasks.begin(), e.tasks.end(), [](const json &t) {
            auto it = t.find("text");
            return it != t.end() && it->is_string() && says_no_tasks(it->get<std::string>());
        }), e.tasks.end());
        e.profile = arr_of(j, "profile", true);
        e.vocab = arr_of(j, "vocab", false);
        e.facts = arr_of(j, "facts", true);
        e.topics = arr_of(j, "topics", false);
        e.people = arr_of(j, "people", false);
        if (j.contains("next_meeting") && j["next_meeting"].is_object()) e.next_meeting = j["next_meeting"];
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

namespace sve {

namespace {
const std::set<std::string> &lead_words() {
    static const std::set<std::string> k = {"one", "a", "an", "the", "my", "our", "his", "her", "their", "your",
                                            "some", "someone", "somebody", "everyone", "everybody", "this", "that"};
    return k;
}
std::string first_word_lower(const std::string &s, size_t *count) {
    std::string first, cur;
    size_t n = 0;
    for (size_t i = 0; i <= s.size(); i++) {
        const bool end = i == s.size() || std::isspace((unsigned char)s[i]);
        if (!end) { cur.push_back((char)std::tolower((unsigned char)s[i])); continue; }
        if (cur.empty()) continue;
        if (!n) first = cur;
        n++;
        cur.clear();
    }
    if (count) *count = n;
    return first;
}
}  // namespace

bool looks_like_description(const std::string &name) {
    size_t n = 0;
    const std::string first = first_word_lower(name, &n);
    return n > 0 && (lead_words().count(first) > 0 || n > 5);
}

bool looks_like_person_name(const std::string &raw) {
    std::vector<std::string> words;
    std::string cur;
    bool caps_word = false;
    for (char ch : raw) {
        unsigned char c = (unsigned char)ch;
        if (std::isspace(c)) { if (!cur.empty()) words.push_back(cur); cur.clear(); continue; }
        if (std::isdigit(c)) return false;
        cur.push_back(ch);
    }
    if (!cur.empty()) words.push_back(cur);
    if (words.empty() || words.size() > 4) return false;
    if (words.size() == 1) {  // an acronym ("HCSC", "TCS"): an organisation, not a person
        const std::string &w = words[0];
        int upper = 0, letters = 0;
        for (unsigned char c : w) if (c < 0x80 && std::isalpha(c)) { letters++; if (std::isupper(c)) upper++; }
        caps_word = letters >= 3 && upper == letters;
        if (caps_word) return false;
    }
    auto low = [](std::string w) {
        std::string o;
        for (unsigned char c : w) if (c >= 0x80 || std::isalnum(c)) o.push_back((char)std::tolower(c));
        return o;
    };
    static const std::set<std::string> kOrg = {
        "technologies", "technology", "tech", "inc", "ltd", "llc", "llp", "corp", "corporation", "company", "co",
        "group", "bank", "university", "college", "school", "ministry", "department", "dept", "depot", "depo",
        "police", "services", "service", "systems", "labs", "lab", "ai", "airlines", "airways", "motors",
        "foundation", "institute", "hospital", "clinic", "government", "council", "committee", "team", "society",
        "network", "agency", "board", "office", "industries", "enterprises", "solutions", "pvt", "private", "limited",
        "manager", "managers", "director", "doctor", "teacher", "officer", "brother", "sister", "friend", "people"};
    static const std::set<std::string> kBrand = {
        "google", "microsoft", "apple", "amazon", "meta", "facebook", "openai", "perplexity", "twitter", "nvidia",
        "samsung", "tesla", "netflix", "uber", "ola", "flipkart", "infosys", "wipro", "swiggy", "zomato", "youtube",
        "whatsapp", "instagram", "chatgpt", "gemini", "claude", "anthropic", "linkedin", "paytm", "reliance", "jio",
        "airtel", "tata", "adani", "nasa", "isro", "cia", "fbi", "faa", "who", "un"};
    if (lead_words().count(low(words[0]))) return false;
    for (const auto &w : words) {
        const std::string l = low(w);
        if (kOrg.count(l) || kBrand.count(l)) return false;
    }
    return true;
}

}  // namespace sve
