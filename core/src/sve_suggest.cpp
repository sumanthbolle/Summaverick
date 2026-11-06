#include "sve_suggest.h"
#include "sve_medcheck.h"

#include <algorithm>
#include <set>

namespace sve {

// ---------------------------------------------------------------- getters

std::string jstr(const json &o, const char *key) {
    if (!o.is_object()) return "";
    auto it = o.find(key);
    return (it != o.end() && it->is_string()) ? it->get<std::string>() : "";
}

int64_t jint(const json &o, const char *key, int64_t dflt) {
    if (!o.is_object()) return dflt;
    auto it = o.find(key);
    if (it == o.end()) return dflt;
    if (it->is_number_integer()) return it->get<int64_t>();
    if (it->is_number_float()) {
        double d = it->get<double>();
        if (d > -1e15 && d < 1e15 && d == (double)(int64_t)d) return (int64_t)d;
    }
    return dflt;
}

json jarr(const json &o, const char *key) {
    if (!o.is_object()) return json::array();
    auto it = o.find(key);
    return (it != o.end() && it->is_array()) ? *it : json::array();
}

json jobj(const json &o, const char *key) {
    if (!o.is_object()) return nullptr;
    auto it = o.find(key);
    return (it != o.end() && it->is_object()) ? *it : json(nullptr);
}

namespace {

std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string lower_ascii(std::string s) {
    for (auto &c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return s;
}

std::vector<std::string> norm_tokens(const std::string &s) {
    std::vector<std::string> out;
    std::string n = normalize_for_match(s), cur;
    for (char c : n) {
        if (c == ' ') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool has_run(const std::vector<std::string> &hay, const std::vector<std::string> &needle) {
    if (needle.empty() || needle.size() > hay.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i)
        if (std::equal(needle.begin(), needle.end(), hay.begin() + (long)i)) return true;
    return false;
}

bool digits(const std::string &s, size_t a, size_t n) {
    if (a + n > s.size()) return false;
    for (size_t i = a; i < a + n; ++i) if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

json str_or_null(const std::string &s) { return s.empty() ? json(nullptr) : json(s); }

}  // namespace

bool valid_hhmm(const std::string &s) {
    if (s.size() != 5 || s[2] != ':' || !digits(s, 0, 2) || !digits(s, 3, 2)) return false;
    return std::stoi(s.substr(0, 2)) < 24 && std::stoi(s.substr(3, 2)) < 60;
}

bool valid_ymd(const std::string &s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-' || !digits(s, 0, 4) || !digits(s, 5, 2) || !digits(s, 8, 2)) return false;
    int m = std::stoi(s.substr(5, 2)), d = std::stoi(s.substr(8, 2));
    return m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

std::string sanitize_repeat(const std::string &s) {
    for (const char *r : {"daily", "twice_daily", "weekly", "once"}) if (s == r) return s;
    return "";
}

std::vector<std::string> valid_times(const json &arr) {
    std::vector<std::string> out;
    if (!arr.is_array()) return out;
    for (const auto &t : arr)
        if (t.is_string() && valid_hhmm(t.get<std::string>()) &&
            std::find(out.begin(), out.end(), t.get<std::string>()) == out.end())
            out.push_back(t.get<std::string>());
    return out;
}

std::vector<std::string> resolve_times(const std::string &when_text, const json &routine) {
    static const struct { const char *word, *slot; } kWords[] = {
        {"breakfast", "breakfast"}, {"morning", "breakfast"}, {"tiffin", "breakfast"},
        {"\xE0\xB0\x89\xE0\xB0\xA6\xE0\xB0\xAF\xE0\xB0\x82", "breakfast"},                      // ఉదయం
        {"\xE0\xB0\x9F\xE0\xB0\xBF\xE0\xB0\xAB\xE0\xB0\xBF\xE0\xB0\xA8\xE0\xB1\x8D", "breakfast"},  // టిఫిన్
        {"lunch", "lunch"}, {"afternoon", "lunch"},
        {"\xE0\xB0\xAE\xE0\xB0\xA7\xE0\xB1\x8D\xE0\xB0\xAF\xE0\xB0\xBE\xE0\xB0\xB9\xE0\xB1\x8D\xE0\xB0\xA8\xE0\xB0\x82", "lunch"},  // మధ్యాహ్నం
        {"dinner", "dinner"}, {"supper", "dinner"}, {"night", "dinner"},
        {"\xE0\xB0\xB0\xE0\xB0\xBE\xE0\xB0\xA4\xE0\xB1\x8D\xE0\xB0\xB0\xE0\xB0\xBF", "dinner"},  // రాత్రి
        {"bedtime", "bed"}, {"before bed", "bed"}, {"before sleep", "bed"},
    };
    std::vector<std::string> out;
    if (!routine.is_object() || when_text.empty()) return out;
    std::string w = lower_ascii(when_text);
    for (const auto &k : kWords) {
        if (w.find(k.word) == std::string::npos) continue;
        std::string t = jstr(routine, k.slot);
        if (valid_hhmm(t) && std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------- extraction windows

std::vector<json> group_windows(const json &pieces, int budget_tokens,
                                const std::function<int(const std::string &)> &count) {
    std::vector<json> out;
    if (!pieces.is_array()) return out;
    json cur = json::array();
    int used = 0;
    for (const auto &p : pieces) {
        if (!p.is_object()) continue;
        int n = count(pieces_with_markers(json::array({p})));
        if (!cur.empty() && used + n > budget_tokens) {
            out.push_back(cur);
            cur = json::array();
            used = 0;
        }
        cur.push_back(p);
        used += n;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

Extracted merge_extracted(const std::vector<Extracted> &parts) {
    Extracted m;
    std::set<std::string> seen_vocab, seen_topics, seen_people;
    auto add_unique = [](json &dst, const json &src, std::set<std::string> &seen, size_t cap) {
        for (const auto &v : src) {
            if (!v.is_string() || dst.size() >= cap) continue;
            std::string t = trim(v.get<std::string>());
            if (t.empty() || !seen.insert(lower_ascii(t)).second) continue;
            dst.push_back(t);
        }
    };
    for (const auto &p : parts) {
        if (!p.ok) continue;
        m.ok = true;
        for (const auto &v : p.reminders) m.reminders.push_back(v);
        for (const auto &v : p.tasks) m.tasks.push_back(v);
        for (const auto &v : p.profile) m.profile.push_back(v);
        for (const auto &v : p.facts) m.facts.push_back(v);
        add_unique(m.vocab, p.vocab, seen_vocab, 50);
        add_unique(m.topics, p.topics, seen_topics, 3);
        add_unique(m.people, p.people, seen_people, 5);
    }
    return m;
}

// ---------------------------------------------------------------- checks

QuoteHit locate_quote(const json &pieces, const std::string &quote, int64_t cited_start_sec) {
    QuoteHit h;
    std::vector<std::string> q = norm_tokens(quote);
    if (q.empty() || !pieces.is_array()) return h;
    std::vector<std::pair<int64_t, std::vector<std::string>>> ps;
    for (const auto &p : pieces)
        if (p.is_object()) ps.push_back({jint(p, "start_sec", 0), norm_tokens(jstr(p, "text"))});
    for (const auto &p : ps)
        if (p.first == cited_start_sec && has_run(p.second, q)) { h.found = true; h.start_sec = p.first; return h; }
    for (const auto &p : ps)
        if (has_run(p.second, q)) { h.found = true; h.start_sec = p.first; return h; }
    for (size_t i = 0; i + 1 < ps.size(); ++i) {
        std::vector<std::string> joined = ps[i].second;
        joined.insert(joined.end(), ps[i + 1].second.begin(), ps[i + 1].second.end());
        if (has_run(joined, q)) { h.found = true; h.start_sec = ps[i].first; return h; }
    }
    return h;
}

bool dose_missing_unit(const std::string &dose) {
    std::vector<std::string> t = norm_tokens(dose);
    return !t.empty() && t.back()[0] >= '0' && t.back()[0] <= '9';
}

namespace {

struct Base {
    json rec;
    std::vector<std::string> fields;
};

bool piece_exists(const json &pieces, int64_t sec) {
    if (!pieces.is_array()) return false;
    for (const auto &p : pieces) if (p.is_object() && jint(p, "start_sec", -1) == sec) return true;
    return false;
}

Base base_for(const char *kind, const json &item, const json &pieces, int64_t note_id) {
    Base b;
    std::string quote = jstr(item, "quote");
    int64_t cited = jint(item, "start_sec", -1);
    QuoteHit h = locate_quote(pieces, quote, cited);
    json start = nullptr;
    if (h.found) start = h.start_sec;
    else if (piece_exists(pieces, cited)) start = cited;
    if (!h.found) b.fields.push_back("quote");
    b.rec = {{"kind", kind}, {"quote", quote}, {"note_id", note_id}, {"start_sec", start}};
    return b;
}

// False when the medicine must be dropped. med_out is the sanitized medicine, or null.
bool check_med(const json &item, const std::string &when_text, std::vector<std::string> &fields, json &med_out) {
    med_out = nullptr;
    json m = jobj(item, "medicine");
    if (m.is_null()) return true;
    MedFields f{trim(jstr(m, "name")), trim(jstr(m, "dose")), when_text, trim(jstr(m, "frequency")), trim(jstr(m, "duration"))};
    if (f.name.empty() && f.dose.empty() && f.frequency.empty() && f.duration.empty()) return true;
    MedCheck mc = check_medicine(f, jstr(item, "quote"));
    if (mc.status == "drop") return false;
    for (auto &x : mc.fields) fields.push_back(x);
    if (dose_missing_unit(f.dose)) fields.push_back("dose");
    med_out = {{"name", f.name}, {"dose", f.dose}, {"frequency", f.frequency}, {"duration", f.duration}};
    return true;
}

json finish(Base &b, const json &payload) {
    std::vector<std::string> u;
    for (auto &f : b.fields) if (std::find(u.begin(), u.end(), f) == u.end()) u.push_back(f);
    b.rec["payload"] = payload;
    b.rec["check_status"] = u.empty() ? "ok" : "check";
    b.rec["check_fields"] = u;
    return b.rec;
}

}  // namespace

std::vector<json> build_suggestions(const Extracted &e, const json &pieces, int64_t note_id, const json &routine) {
    std::vector<json> out;
    for (const auto &it : e.reminders) {
        Base b = base_for("reminder", it, pieces, note_id);
        std::string when = trim(jstr(it, "when_text"));
        json med;
        if (!check_med(it, when, b.fields, med)) continue;
        std::vector<std::string> model_times = valid_times(jarr(it, "times"));
        std::vector<std::string> times = resolve_times(when, routine);
        if (times.empty()) {
            times = model_times;
            if (!med.is_null() && !model_times.empty()) b.fields.push_back("times");  // not from the routine
        }
        std::string text = trim(jstr(it, "text"));
        if (text.empty() && !med.is_null() && !jstr(med, "name").empty())
            text = trim(jstr(med, "name") + " " + jstr(med, "dose"));
        if (text.empty()) continue;
        std::string until = jstr(it, "until_date");
        out.push_back(finish(b, {{"text", text}, {"when_text", when}, {"times", times},
                                 {"repeat", str_or_null(sanitize_repeat(jstr(it, "repeat")))},
                                 {"until_date", valid_ymd(until) ? json(until) : json(nullptr)}, {"medicine", med}}));
    }
    for (const auto &it : e.tasks) {
        std::string text = trim(jstr(it, "text"));
        if (text.empty()) continue;
        Base b = base_for("task", it, pieces, note_id);
        std::string due = jstr(it, "due_date");
        out.push_back(finish(b, {{"text", text}, {"who", str_or_null(trim(jstr(it, "who")))},
                                 {"due_date", valid_ymd(due) ? json(due) : json(nullptr)}}));
    }
    for (const auto &it : e.profile) {
        std::string change = jstr(it, "change");
        bool is_med = change == "add_medicine" || change == "update_medicine" || change == "stop_medicine";
        if (!is_med && change != "routine") continue;
        Base b = base_for("profile", it, pieces, note_id);
        std::string when = trim(jstr(it, "when_text"));
        json med;
        if (!check_med(it, when, b.fields, med)) continue;
        if (is_med && (med.is_null() || jstr(med, "name").empty())) continue;
        std::string detail = trim(jstr(it, "detail"));
        std::string text = !detail.empty() ? detail : is_med ? jstr(med, "name") : "";
        if (text.empty()) continue;
        out.push_back(finish(b, {{"change", change}, {"text", text}, {"detail", detail}, {"when_text", when},
                                 {"medicine", is_med ? med : json(nullptr)}}));
    }
    for (const auto &it : e.facts) {
        std::string text = trim(jstr(it, "text"));
        if (text.empty()) continue;
        Base b = base_for("fact", it, pieces, note_id);
        out.push_back(finish(b, {{"memory_kind", "fact"}, {"text", text}}));
    }
    for (const auto &v : e.vocab) {
        std::string t = v.is_string() ? trim(v.get<std::string>()) : "";
        if (t.empty() || t.size() > 60) continue;
        out.push_back({{"kind", "vocab"}, {"payload", {{"text", t}}}, {"quote", nullptr}, {"note_id", note_id},
                       {"start_sec", nullptr}, {"check_status", nullptr}, {"check_fields", json::array()}});
    }
    return out;
}

// ---------------------------------------------------------------- Ask

std::string keyword_query(const std::string &question) {
    static const std::set<std::string> kStop = {
        "what", "when", "where", "which", "who", "whom", "why", "how", "did", "do", "does", "done", "is", "are",
        "was", "were", "be", "been", "the", "an", "about", "say", "said", "tell", "me", "my", "you", "your",
        "of", "to", "in", "on", "at", "for", "and", "or", "it", "that", "this", "there", "any", "can", "could",
        "should", "would", "will", "take", "have", "has", "had", "with", "from", "by", "please", "us", "we",
        "\xE0\xB0\x8F\xE0\xB0\xAE\xE0\xB0\xBF",                                                  // ఏమి
        "\xE0\xB0\x8F\xE0\xB0\xAE\xE0\xB0\xBF\xE0\xB0\x9F\xE0\xB0\xBF",                          // ఏమిటి
        "\xE0\xB0\x8E\xE0\xB0\xAA\xE0\xB1\x8D\xE0\xB0\xAA\xE0\xB1\x81\xE0\xB0\xA1\xE0\xB1\x81",  // ఎప్పుడు
        "\xE0\xB0\x8E\xE0\xB0\xB2\xE0\xB0\xBE",                                                  // ఎలా
        "\xE0\xB0\x97\xE0\xB1\x81\xE0\xB0\xB0\xE0\xB0\xBF\xE0\xB0\x82\xE0\xB0\x9A\xE0\xB0\xBF",  // గురించి
        "\xE0\xB0\xA8\xE0\xB0\xBE",                                                              // నా
        "\xE0\xB0\xA8\xE0\xB1\x87\xE0\xB0\xA8\xE0\xB1\x81",                                      // నేను
    };
    std::string out, cur;
    auto flush = [&] {
        if (cur.size() >= 2 && !kStop.count(cur)) out += (out.empty() ? "" : " ") + cur;
        cur.clear();
    };
    for (unsigned char c : lower_ascii(question)) {
        bool word = c >= 0x80 || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (word) cur += (char)c; else flush();
    }
    flush();
    return out;
}

std::vector<std::string> names_in_question(const std::string &question, const std::vector<std::string> &names) {
    std::vector<std::string> out;
    std::vector<std::string> q = norm_tokens(question);
    for (const auto &n : names)
        if (has_run(q, norm_tokens(n)) && std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
    return out;
}

bool is_medicine_question(const std::string &question, const std::vector<std::string> &names) {
    if (!names_in_question(question, names).empty()) return true;
    static const std::set<std::string> kWords = {"tablet", "tablets", "medicine", "medicines", "medication",
        "medications", "dose", "doses", "dosage", "pill", "pills", "capsule", "capsules", "syrup"};
    for (const auto &t : norm_tokens(question)) if (kWords.count(t)) return true;
    for (const char *w : {"\xE0\xB0\xAE\xE0\xB0\x82\xE0\xB0\xA6\xE0\xB1\x81",              // మందు
                          "\xE0\xB0\xAE\xE0\xB0\xBE\xE0\xB0\xA4\xE0\xB1\x8D\xE0\xB0\xB0",  // మాత్ర
                          "\xE0\xB0\xA1\xE0\xB1\x8B\xE0\xB0\xB8\xE0\xB1\x8D"})             // డోస్
        if (question.find(w) != std::string::npos) return true;
    return false;
}

Cited apply_citations(const std::string &model_out, const json &labelled_sources) {
    Cited c;
    if (model_out.find("NOT_FOUND") != std::string::npos || !labelled_sources.is_array()) return c;
    std::string answer;
    std::vector<std::string> cited;
    size_t i = 0;
    while (i < model_out.size()) {
        size_t a = model_out.find('[', i);
        size_t b = a == std::string::npos ? a : model_out.find(']', a);
        if (b == std::string::npos) { answer += model_out.substr(i); break; }
        // A citation group holds only labels like S1, separated by commas or spaces.
        std::string inner = model_out.substr(a + 1, b - a - 1), cur;
        std::vector<std::string> labels;
        bool ok = true;
        for (size_t k = 0; k <= inner.size() && ok; ++k) {
            char ch = k < inner.size() ? inner[k] : ',';
            if (ch != ',' && ch != ' ') { cur += ch; continue; }
            if (cur.empty()) continue;
            if (cur.size() < 2 || cur[0] != 'S' || !digits(cur, 1, cur.size() - 1)) ok = false;
            else labels.push_back(cur);
            cur.clear();
        }
        if (!ok || labels.empty()) { answer += model_out.substr(i, b + 1 - i); i = b + 1; continue; }
        answer += model_out.substr(i, a - i);
        while (!answer.empty() && answer.back() == ' ') answer.pop_back();
        for (auto &l : labels) if (std::find(cited.begin(), cited.end(), l) == cited.end()) cited.push_back(l);
        i = b + 1;
    }
    for (const auto &l : cited)
        for (const auto &s : labelled_sources)
            if (jstr(s, "label") == l) {
                c.sources.push_back({{"note_id", jint(s, "note_id", 0)}, {"start_sec", jint(s, "start_sec", 0)},
                                     {"title", jstr(s, "title")}});
                break;
            }
    if (c.sources.empty()) return c;
    // Tidy spaces left before punctuation, and double spaces, by removed markers.
    std::string tidy;
    for (size_t k = 0; k < answer.size(); ++k) {
        if (answer[k] == ' ' && k + 1 < answer.size() && (answer[k + 1] == '.' || answer[k + 1] == ',')) continue;
        if (answer[k] == ' ' && !tidy.empty() && tidy.back() == ' ') continue;
        tidy += answer[k];
    }
    c.answer = trim(tidy);
    c.not_found = false;
    return c;
}

std::string not_found_text(const std::string &lang) {
    if (lang == "te")  // మీ రికార్డింగ్‌లలో ఇది నేను వినలేదు.
        return "\xE0\xB0\xAE\xE0\xB1\x80 \xE0\xB0\xB0\xE0\xB0\xBF\xE0\xB0\x95\xE0\xB0\xBE\xE0\xB0\xB0\xE0\xB1\x8D\xE0\xB0\xA1"
               "\xE0\xB0\xBF\xE0\xB0\x82\xE0\xB0\x97\xE0\xB1\x8D\xE2\x80\x8C\xE0\xB0\xB2\xE0\xB0\xB2\xE0\xB1\x8B "
               "\xE0\xB0\x87\xE0\xB0\xA6\xE0\xB0\xBF \xE0\xB0\xA8\xE0\xB1\x87\xE0\xB0\xA8\xE0\xB1\x81 "
               "\xE0\xB0\xB5\xE0\xB0\xBF\xE0\xB0\xA8\xE0\xB0\xB2\xE0\xB1\x87\xE0\xB0\xA6\xE0\xB1\x81.";
    return "I didn't hear that in your recordings.";
}

namespace {

std::string mmss(int64_t sec) {
    if (sec < 0) sec = 0;
    int64_t m = sec / 60, s = sec % 60;
    return std::to_string(m) + ":" + (s < 10 ? "0" : "") + std::to_string(s);
}

std::string repeat_words(const std::string &r, bool te) {
    if (r == "daily") return te ? "\xE0\xB0\xAA\xE0\xB1\x8D\xE0\xB0\xB0\xE0\xB0\xA4\xE0\xB0\xBF\xE0\xB0\xB0\xE0\xB1\x8B\xE0\xB0\x9C\xE0\xB1\x82" : "daily";  // ప్రతిరోజూ
    if (r == "twice_daily") return te ? "\xE0\xB0\xB0\xE0\xB1\x8B\xE0\xB0\x9C\xE0\xB1\x81\xE0\xB0\x95\xE0\xB1\x81 \xE0\xB0\xB0\xE0\xB1\x86\xE0\xB0\x82\xE0\xB0\xA1\xE0\xB1\x81\xE0\xB0\xB8\xE0\xB0\xBE\xE0\xB0\xB0\xE0\xB1\x8D\xE0\xB0\xB2\xE0\xB1\x81" : "twice a day";  // రోజుకు రెండుసార్లు
    if (r == "weekly") return te ? "\xE0\xB0\xB5\xE0\xB0\xBE\xE0\xB0\xB0\xE0\xB0\xBE\xE0\xB0\xA8\xE0\xB0\xBF\xE0\xB0\x95\xE0\xB0\xBF \xE0\xB0\x92\xE0\xB0\x95\xE0\xB0\xB8\xE0\xB0\xBE\xE0\xB0\xB0\xE0\xB0\xBF" : "weekly";  // వారానికి ఒకసారి
    if (r == "once") return te ? "\xE0\xB0\x92\xE0\xB0\x95\xE0\xB0\xB8\xE0\xB0\xBE\xE0\xB0\xB0\xE0\xB0\xBF" : "once";  // ఒకసారి
    return "";
}

}  // namespace

std::string medicine_answer(const json &medicines, const json &hits, const std::string &lang) {
    const bool te = lang == "te";
    std::string meds, quotes;
    if (medicines.is_array())
        for (const auto &m : medicines) {
            std::string name = jstr(m, "name");
            if (name.empty()) continue;
            std::string line = "- " + name, dose = jstr(m, "dose"), w = jstr(m, "when_text"), times;
            if (!dose.empty()) line += " " + dose;
            for (const auto &t : valid_times(jarr(m, "times"))) times += (times.empty() ? "" : ", ") + t;
            if (!times.empty()) w += (w.empty() ? "" : " ") + std::string("(") + times + ")";
            std::vector<std::string> parts;
            if (!w.empty()) parts.push_back(w);
            std::string rep = repeat_words(jstr(m, "repeat"), te), until = jstr(m, "until_date");
            if (!rep.empty()) parts.push_back(rep);
            if (!until.empty()) parts.push_back(te ? until + " \xE0\xB0\xB5\xE0\xB0\xB0\xE0\xB0\x95\xE0\xB1\x81" : "until " + until);  // వరకు
            for (size_t i = 0; i < parts.size(); ++i) line += (i ? ", " : " \xE2\x80\x94 ") + parts[i];
            meds += line + "\n";
        }
    if (hits.is_array())
        for (const auto &h : hits) {
            std::string text = trim(jstr(h, "text"));
            if (text.empty()) continue;
            quotes += "\xE2\x80\x9C" + text + "\xE2\x80\x9D \xE2\x80\x94 " + jstr(h, "title") + " @" + mmss(jint(h, "start_sec", 0)) + "\n";
        }
    std::string out;
    if (!meds.empty())  // మీ ప్రొఫైల్ నుండి:
        out += std::string(te ? "\xE0\xB0\xAE\xE0\xB1\x80 \xE0\xB0\xAA\xE0\xB1\x8D\xE0\xB0\xB0\xE0\xB1\x8A\xE0\xB0\xAB\xE0\xB1\x88\xE0\xB0\xB2\xE0\xB1\x8D \xE0\xB0\xA8\xE0\xB1\x81\xE0\xB0\x82\xE0\xB0\xA1\xE0\xB0\xBF:"
                                  : "From your profile:") + "\n" + meds;
    if (!quotes.empty())  // మీ రికార్డింగ్‌ల నుండి:
        out += std::string(out.empty() ? "" : "\n") +
               (te ? "\xE0\xB0\xAE\xE0\xB1\x80 \xE0\xB0\xB0\xE0\xB0\xBF\xE0\xB0\x95\xE0\xB0\xBE\xE0\xB0\xB0\xE0\xB1\x8D\xE0\xB0\xA1\xE0\xB0\xBF\xE0\xB0\x82\xE0\xB0\x97\xE0\xB1\x8D\xE2\x80\x8C\xE0\xB0\xB2 \xE0\xB0\xA8\xE0\xB1\x81\xE0\xB0\x82\xE0\xB0\xA1\xE0\xB0\xBF:"
                   : "From your recordings:") + "\n" + quotes;
    return trim(out);
}

json sanitize_topic_summary(const json &j) {
    json out = {{"summary", trim(jstr(j, "summary"))}, {"key_terms", json::array()}, {"quiz", json::array()}};
    for (const auto &k : jarr(j, "key_terms")) {
        std::string t = trim(jstr(k, "term")), m = trim(jstr(k, "meaning"));
        if (!t.empty() && !m.empty()) out["key_terms"].push_back({{"term", t}, {"meaning", m}});
    }
    for (const auto &k : jarr(j, "quiz")) {
        std::string q = trim(jstr(k, "q")), a = trim(jstr(k, "a"));
        if (!q.empty() && !a.empty()) out["quiz"].push_back({{"q", q}, {"a", a}});
    }
    return out;
}

bool mentions_medicine(const std::string &text, const std::vector<std::string> &names) {
    static const std::set<std::string> kUnits = {"mg", "ml", "mcg", "iu", "tablet", "tablets", "capsule", "capsules"};
    for (const auto &t : norm_tokens(text)) if (kUnits.count(t)) return true;
    return !names_in_question(text, names).empty();
}

}  // namespace sve
