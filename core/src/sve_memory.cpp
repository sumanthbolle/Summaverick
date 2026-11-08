#include "sve_memory.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include <set>

#include "sve_health.h"
#include "sve_medcheck.h"
#include "sve_suggest.h"
#include "sve_textmatch.h"

namespace sve {

namespace {

using Tokens = std::vector<std::string>;

std::string ascii_lower(std::string s) {
    for (auto &c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// Non-empty trimmed strings of an array (other entries ignored).
json string_list(const json &o, const char *key) {
    json out = json::array();
    for (const auto &v : jarr(o, key))
        if (v.is_string()) {
            std::string t = trim_copy(v.get<std::string>());
            if (!t.empty()) out.push_back(t);
        }
    return out;
}

bool in_run(const Tokens &hay, const std::string &words) {
    Tokens t = match_tokens(words);
    return !t.empty() && contains_run(hay, t);
}

// Byte offset in `scope` of the word where the first occurrence of `q` (match tokens) starts.
size_t run_start(const std::string &scope, const Tokens &q) {
    std::vector<size_t> starts;  // word starts
    for (size_t i = 0; i < scope.size(); ++i) {
        bool space = scope[i] == ' ' || scope[i] == '\t' || scope[i] == '\n' || scope[i] == '\r';
        bool prev_space = i == 0 || scope[i - 1] == ' ' || scope[i - 1] == '\t' || scope[i - 1] == '\n' || scope[i - 1] == '\r';
        if (!space && prev_space) starts.push_back(i);
    }
    size_t end = scope.size();
    for (size_t p : starts)
        if (p > 0 && contains_run(match_tokens(scope.substr(0, p)), q)) { end = p; break; }
    size_t start = 0;
    for (size_t p : starts) {
        if (p >= end) break;
        if (contains_run(match_tokens(scope.substr(p, end - p)), q)) start = p;
    }
    return start;
}

// Up to `n` bytes of `s` before `pos`, starting on a UTF-8 character boundary.
std::string bytes_before(const std::string &s, size_t pos, size_t n) {
    size_t from = pos > n ? pos - n : 0;
    while (from < pos && (static_cast<unsigned char>(s[from]) & 0xC0) == 0x80) ++from;
    return s.substr(from, pos - from);
}

bool is_pronoun(const std::string &who) {
    static const std::set<std::string> kPronouns = {
        "i", "we", "you", "he", "she", "they",
        "నేను", "మేము", "మనం", "నువ్వు", "మీరు", "అతను", "ఆమె", "ఆయన", "వాళ్ళు",
    };
    return kPronouns.count(ascii_lower(nfc_clean(trim_copy(who)))) > 0;
}

// A spoken person is kept only when the recording says it: a name in the quote or just before it,
// a pronoun in the quote itself.
std::string keep_person(const std::string &who, const std::string &quote, const std::string &before) {
    std::string w = trim_copy(who);
    if (w.empty()) return "";
    if (name_in(w, quote)) return w;
    if (!is_pronoun(w) && !before.empty() && name_in(w, before)) return w;
    return "";
}

bool truthy(const json &row, const char *key) {
    auto it = row.find(key);
    if (it == row.end() || it->is_null()) return false;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number()) return it->get<double>() != 0;
    return false;
}

bool starts_word(const std::string &s, const std::string &w) {
    if (s.compare(0, w.size(), w) != 0) return false;
    if (s.size() == w.size()) return true;
    char c = s[w.size()];
    return !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'));
}

}  // namespace

json parse_memory_candidates(const json &arr) {
    static const std::set<std::string> kTypes = {"fact", "decision", "promise", "question", "person", "idea"};
    json out = json::array();
    if (!arr.is_array()) return out;
    for (const auto &c : arr) {
        if (out.size() >= 8) break;
        if (!c.is_object()) continue;
        std::string type = ascii_lower(trim_copy(jstr(c, "type")));
        if (type == "open question") type = "question";
        if (!kTypes.count(type)) continue;
        std::string text = trim_copy(jstr(c, "text"));
        if (text.empty() || text.size() > 300) continue;
        auto q = c.find("quote");
        if (q == c.end() || !q->is_string()) continue;
        out.push_back({{"type", type}, {"text", text}, {"quote", q->get<std::string>()},
                       {"who", trim_copy(jstr(c, "who"))}, {"to_whom", trim_copy(jstr(c, "to_whom"))},
                       {"due_text", trim_copy(jstr(c, "due_text"))},
                       {"names", string_list(c, "names")}, {"numbers", string_list(c, "numbers")},
                       {"dates", string_list(c, "dates")}, {"times", string_list(c, "times")},
                       {"start_sec", std::max<int64_t>(0, jint(c, "start_sec", 0))}});
    }
    return out;
}

Grounded ground_memory(const json &cand, const SectionPieces &pieces, const std::vector<std::optional<double>> &piece_conf,
                       const std::string &said_ymd, const std::vector<std::string> &med_names, double low_conf_threshold) {
    Grounded g;
    const std::string type = jstr(cand, "type"), text = jstr(cand, "text"), quote = jstr(cand, "quote");

    // 1. Medicines have their own table and rules.
    if (mentions_medicine(text + " " + quote, med_names)) { g.reason = "medicine"; return g; }

    // 2. The quote is said in the section: inside one piece, or across one piece boundary.
    const Tokens qtok = match_tokens(quote);
    std::string joined;
    std::vector<size_t> offset;
    for (const auto &p : pieces) {
        if (!joined.empty() || !offset.empty()) joined += ' ';
        offset.push_back(joined.size());
        joined += p.second;
    }
    size_t first = pieces.size(), count = 0;
    if (!qtok.empty()) {
        for (size_t i = 0; i < pieces.size() && !count; ++i)
            if (contains_run(match_tokens(pieces[i].second), qtok)) { first = i; count = 1; }
        for (size_t i = 0; i + 1 < pieces.size() && !count; ++i)
            if (contains_run(match_tokens(pieces[i].second + " " + pieces[i + 1].second), qtok)) { first = i; count = 2; }
    }
    if (!count) { g.reason = "quote_not_found"; return g; }

    // 3. Every name, number, date and time the memory relies on is in the quote.
    json checked = json::array();
    const struct { const char *key, *kind; } kFields[] = {
        {"names", "name"}, {"numbers", "number"}, {"dates", "date"}, {"times", "time"}};
    for (const auto &f : kFields)
        for (const auto &v : jarr(cand, f.key)) {
            std::string s = v.is_string() ? trim_copy(v.get<std::string>()) : "";
            if (s.empty()) continue;
            bool ok = std::string(f.kind) == "name" ? name_in(s, quote) : in_run(qtok, s);
            if (!ok) { g.reason = std::string("field_not_in_quote:") + f.kind; return g; }
            checked.push_back({{"kind", f.kind}, {"text", s}});
        }

    // 4. Every number and date of the text is in the quote, as often as the text has it.
    std::map<std::string, int> have;
    for (const auto &t : canonical_facts(quote)) ++have[t];
    for (const auto &t : canonical_facts(text))
        if (--have[t] < 0) { g.reason = "text_not_in_quote"; return g; }

    // 5. who / to_whom only when said.
    std::string scope = pieces[first].second;
    if (count == 2) scope += " " + pieces[first + 1].second;
    const std::string before = bytes_before(joined, offset[first] + run_start(scope, qtok), 80);
    const std::string who = keep_person(jstr(cand, "who"), quote, before);
    const std::string to_whom = keep_person(jstr(cand, "to_whom"), quote, before);

    // 6. A deadline only in the quote's words, resolved against the day it was said.
    std::string due_text = trim_copy(jstr(cand, "due_text"));
    if (!in_run(qtok, due_text)) due_text.clear();
    const std::string due_date = due_text.empty() ? "" : resolve_spoken_date(due_text, said_ymd);

    // 7, 8. Health and transcript confidence.
    const bool health = is_health_memory(text + " " + quote, med_names);
    bool low = false;
    std::optional<double> min_conf;
    for (size_t i = first; i < first + count; ++i) {
        if (i >= piece_conf.size() || !piece_conf[i]) continue;
        double c = *piece_conf[i];
        if (c < low_conf_threshold) low = true;
        if (!min_conf || c < *min_conf) min_conf = c;
    }

    g.ok = true;
    g.row = {{"type", type}, {"text", text}, {"who", who}, {"to_whom", to_whom},
             {"due_text", due_text}, {"due_date", due_date}, {"said_date", said_ymd},
             {"checked", checked}, {"confidence", low ? "low" : "high"}, {"health", health ? 1 : 0},
             {"sources", json::array({{{"start_sec", pieces[first].first}, {"quote", quote},
                                       {"conf", min_conf ? json(*min_conf) : json(nullptr)}}})}};
    // 9. Names, numbers, dates and promises get the yes/no check.
    g.needs_double_check = !checked.empty() || type == "promise";
    return g;
}

std::string double_check_prompt(const std::string &memory_text, const std::string &quote) {
    return "Quote: \"" + quote + "\"\nMemory: \"" + memory_text +
           "\"\nDoes the quote say this? Answer yes or no.";
}

int parse_yes_no(const std::string &model_out) {
    std::string s = trim_copy(nfc_clean(model_out));
    size_t i = 0;
    while (i < s.size() && static_cast<unsigned char>(s[i]) < 0x80 &&
           !((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9')))
        ++i;  // leading spaces, quotes, asterisks
    s = ascii_lower(s.substr(i));
    if (starts_word(s, "yes")) return 1;
    if (starts_word(s, "no")) return 0;
    if (s.rfind("అవును", 0) == 0) return 1;
    if (s.rfind("కాదు", 0) == 0 || s.rfind("లేదు", 0) == 0) return 0;
    return -1;
}

bool goes_to_queue(const json &row, int double_check, bool direct_allowed) {
    auto conflict = row.find("conflict_with");
    return truthy(row, "health") || jstr(row, "confidence") == "low" || double_check == -1 ||
           (conflict != row.end() && !conflict->is_null()) || !direct_allowed;
}

double memory_importance(const json &row, int people_met_max, int practice_successes, int64_t now) {
    static const std::map<std::string, double> kBase = {
        {"promise", 3}, {"decision", 3}, {"question", 2}, {"person", 2}, {"fact", 1}, {"idea", 1}};
    auto b = kBase.find(jstr(row, "type"));
    double base = b == kBase.end() ? 0 : b->second;
    for (const auto &c : jarr(row, "checked")) {
        std::string k = c.is_object() ? jstr(c, "kind") : "";
        if (k == "number" || k == "date") { base += 1; break; }
    }
    base += std::min(2.0, 0.5 * std::max(0, people_met_max - 1));
    const std::string review = jstr(row, "review");
    if (review == "kept" || review == "corrected") base += 2;
    base += 0.5 * std::min(4, std::max(0, practice_successes));
    int64_t seen = std::max(jint(row, "last_seen", INT64_MIN), jint(row, "last_used", INT64_MIN));
    double days = seen == INT64_MIN ? 0 : std::max<double>(0, double(now - seen) / 86400.0);
    return base * std::pow(0.5, days / 60.0);
}

}  // namespace sve
