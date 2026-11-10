#include "sve_memory.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include <set>

#include "sve_english_words.h"
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

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

std::vector<size_t> word_starts(const std::string &s) {
    std::vector<size_t> out;
    for (size_t i = 0; i < s.size(); ++i)
        if (!is_space(s[i]) && (i == 0 || is_space(s[i - 1]))) out.push_back(i);
    return out;
}

// Bytes [start, end) of `scope` holding the first occurrence of `q` (match tokens), cut at word starts.
std::pair<size_t, size_t> run_span(const std::string &scope, const Tokens &q) {
    const std::vector<size_t> starts = word_starts(scope);
    size_t end = scope.size();
    for (size_t p : starts)
        if (p > 0 && contains_run(match_tokens(scope.substr(0, p)), q)) { end = p; break; }
    size_t start = 0;
    for (size_t p : starts) {
        if (p >= end) break;
        if (contains_run(match_tokens(scope.substr(p, end - p)), q)) start = p;
    }
    while (end > start && is_space(scope[end - 1])) --end;
    return {start, end};
}

// Up to `n` bytes of `s` before `pos`, starting on a UTF-8 character boundary.
std::string bytes_before(const std::string &s, size_t pos, size_t n) {
    size_t from = pos > n ? pos - n : 0;
    while (from < pos && (static_cast<unsigned char>(s[from]) & 0xC0) == 0x80) ++from;
    return s.substr(from, pos - from);
}

// A name said in `text` as written: its words as one adjacent run, or its last word with a Telugu case
// ending and its other words directly before. Stricter than name_in (no word window).
bool name_said(const std::string &name, const std::string &text) {
    Tokens n = med_tokens(nfc_clean(name), false), q = med_tokens(nfc_clean(text), false);
    if (n.empty()) return false;
    if (contains_run(q, n)) return true;
    const std::string last = n.back();
    n.pop_back();
    for (size_t i = n.size(); i < q.size(); ++i) {
        const std::string &w = q[i];
        if (w.size() <= last.size() || w.compare(0, last.size(), last) != 0) continue;
        if (std::find(kTeluguCaseEndings.begin(), kTeluguCaseEndings.end(), w.substr(last.size())) ==
            kTeluguCaseEndings.end())
            continue;
        if (std::equal(n.begin(), n.end(), q.begin() + (i - n.size()))) return true;
    }
    return false;
}

// ---------------------------------------------------------------- names across scripts

// A name the memory relies on: as said in the quote, and as written in the memory's text.
struct Name { std::string said, written; };

std::vector<Name> name_list(const json &cand) {
    std::vector<Name> out;
    for (const auto &v : jarr(cand, "names")) {
        Name n;
        if (v.is_string()) n.said = n.written = trim_copy(v.get<std::string>());
        else if (v.is_object()) { n.said = trim_copy(jstr(v, "as_said")); n.written = trim_copy(jstr(v, "as_written")); }
        if (n.written.empty()) n.written = n.said;
        if (!n.said.empty()) out.push_back(n);
    }
    return out;
}

bool mostly_telugu(const std::string &s) {
    size_t te = 0, latin = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') ++latin;
        else if (c == 0xE0 && i + 1 < s.size() &&
                 (static_cast<unsigned char>(s[i + 1]) == 0xB0 || static_cast<unsigned char>(s[i + 1]) == 0xB1)) {
            ++te;
            i += 2;
        }
    }
    return te > latin;
}

bool has_telugu(const std::string &s) {
    for (size_t i = 0; i + 1 < s.size(); ++i)
        if (static_cast<unsigned char>(s[i]) == 0xE0 &&
            (static_cast<unsigned char>(s[i + 1]) == 0xB0 || static_cast<unsigned char>(s[i + 1]) == 0xB1))
            return true;
    return false;
}

std::string collapse(const std::string &s) {
    std::string out;
    for (char c : s) if (out.empty() || out.back() != c) out += c;
    return out;
}

// Consonant skeleton of a Latin spelling: aspirates as their plain consonant (th -> t, sh -> s, ...),
// w as v, no vowels, y or h, doubles collapsed.
std::string latin_skeleton(const std::string &word) {
    std::string w = ascii_lower(word), out;
    for (size_t i = 0; i < w.size(); ++i) {
        char c = w[i], n = i + 1 < w.size() ? w[i + 1] : 0;
        if (c < 'a' || c > 'z') continue;
        if (c == 'x') { out += "ks"; continue; }
        if (n == 'h' && std::string("tdkbgpcjs").find(c) != std::string::npos) { out += c; ++i; continue; }
        switch (c) {
            case 'a': case 'e': case 'i': case 'o': case 'u': case 'y': case 'h': continue;
            case 'w': out += 'v'; continue;
            case 'f': out += 'p'; continue;
            case 'z': out += 'j'; continue;
            case 'q': out += 'k'; continue;
            case 'c': out += 'k'; continue;
            case 'm': out += (n == 'b' || n == 'p') ? 'n' : 'm'; continue;
            default: out += c;
        }
    }
    return collapse(out);
}

// Consonant skeleton of a Telugu word, on the same letters as latin_skeleton.
std::string telugu_skeleton(const std::string &word) {
    static const char *kCons[] = {
        "k", "k", "g", "g", "n", "c", "c", "j", "j", "n",  // U+0C15 క .. U+0C1E ఞ
        "t", "t", "d", "d", "n", "t", "t", "d", "d", "n",  // ట .. న
        "",  "p", "p", "b", "b", "m", "",  "r", "r", "l",  // U+0C29, ప .. ల (య dropped)
        "l", "l", "v", "s", "s", "s", "",                  // ళ ఴ వ శ ష స హ
    };
    std::string out;
    const std::string w = nfc_clean(word);
    for (size_t i = 0; i < w.size();) {
        unsigned char b0 = static_cast<unsigned char>(w[i]);
        if (b0 < 0x80) { ++i; continue; }
        size_t len = b0 >= 0xF0 ? 4 : b0 >= 0xE0 ? 3 : 2;
        if (i + len > w.size()) break;
        unsigned cp = len == 3 ? ((b0 & 0x0F) << 12) | ((static_cast<unsigned char>(w[i + 1]) & 0x3F) << 6) |
                                     (static_cast<unsigned char>(w[i + 2]) & 0x3F)
                               : 0;
        i += len;
        if (cp >= 0x0C15 && cp <= 0x0C39) out += kCons[cp - 0x0C15];
        else if (cp == 0x0C02) out += 'n';                                    // anusvara
        else if (cp == 0x0C0B || cp == 0x0C43 || cp == 0x0C44) out += 'r';   // ఋ, ృ, ౄ
    }
    return collapse(out);
}

// True when `written` (Latin) plausibly spells `said` (Telugu quote words): their consonant skeletons
// agree, after dropping honorifics (గారు ...) and the last word's case ending. A final వు may be
// silent in English ("Rao" for రావు).
bool transliterates(const std::string &written, const std::string &said) {
    static const std::set<std::string> kHonorific = {"గారు", "గారికి", "గారితో", "గారిని", "గారి", "గారూ"};
    if (!has_telugu(said) || has_telugu(written)) return false;
    std::string lat;
    for (const auto &t : med_tokens(written, false)) lat += latin_skeleton(t);
    lat = collapse(lat);
    if (lat.empty()) return false;
    Tokens words;
    for (const auto &t : med_tokens(nfc_clean(said), false))
        if (!kHonorific.count(t)) words.push_back(t);
    if (words.empty()) return false;
    std::string head;
    for (size_t i = 0; i + 1 < words.size(); ++i) head += telugu_skeleton(words[i]);
    std::vector<std::string> lasts = {words.back()};
    for (const auto &e : kTeluguCaseEndings)
        if (words.back().size() > e.size() && words.back().compare(words.back().size() - e.size(), e.size(), e) == 0)
            lasts.push_back(words.back().substr(0, words.back().size() - e.size()));
    for (const auto &l : lasts) {
        std::string sk = collapse(head + telugu_skeleton(l));
        if (sk == lat) return true;
        if (sk.size() > 1 && sk.back() == 'v' && sk.substr(0, sk.size() - 1) == lat) return true;
    }
    return false;
}

// names as [{"as_said","as_written"}]; a plain string is both.
json names_json(const json &cand) {
    json out = json::array();
    for (const auto &n : name_list(cand)) out.push_back({{"as_said", n.said}, {"as_written", n.written}});
    return out;
}

// The Latin words of a name, lowercase.
std::set<std::string> written_words(const std::vector<Name> &names) {
    std::set<std::string> out;
    for (const auto &n : names)
        for (const auto &t : med_tokens(n.written, false)) out.insert(t);
    return out;
}

// Capitalised Latin words that may be names. Not counted: weekdays and months (the canonical-facts
// check has them), words with digits, a short list of everyday words anywhere, and a sentence's first
// word when it is a common English word. A word some name is written as always counts.
std::vector<std::string> name_words(const std::string &raw, const std::set<std::string> &written) {
    static const std::set<std::string> kCommon = {
        "i", "a", "an", "the", "we", "he", "she", "they", "it", "you", "me", "us", "him", "her", "them", "my", "our",
        "your", "his", "their", "its", "this", "that", "these", "those", "there", "here", "today", "tomorrow",
        "yesterday", "tonight", "and", "but", "or", "so", "if", "then", "also", "not", "no", "yes", "ok", "okay",
        "may", "will", "would", "should", "shall", "can", "could", "must", "need", "needs", "do", "does", "did",
        "is", "are", "was", "were", "be", "been", "has", "have", "had", "who", "what", "when", "where", "why", "how",
        "which", "whether", "next", "last", "every", "each", "all", "some", "any", "one", "after", "before", "by",
        "on", "in", "at", "for", "to", "from", "with", "about", "dr", "mr", "mrs", "ms", "sir", "madam", "rs", "inr",
        "pay", "paid", "send", "sent", "call", "bring", "buy", "meet", "ask", "asked", "check", "book", "finish",
        "submit", "return", "visit", "take", "give", "get", "make", "go", "come", "tell", "told", "remember",
        "discuss", "plan", "prepare", "review", "fix", "order", "collect", "share", "email", "write", "read",
        "start", "stop", "keep", "move", "find", "try", "learn", "talk", "see", "look", "use", "decide", "decided",
        "promise", "promised", "said", "say", "agreed", "agree", "follow", "renew", "clean", "repair", "transfer",
    };
    std::string text;  // curly apostrophes as '
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw.compare(i, 3, "\xE2\x80\x99") == 0) { text += '\''; i += 2; }
        else text += raw[i];
    }
    std::vector<std::string> out;
    std::string cur;
    bool initial = true, next_initial = true;
    auto flush = [&] {
        if (cur.empty()) return;
        std::string w = cur;
        cur.clear();
        if (w.size() >= 2 && (w.compare(w.size() - 2, 2, "'s") == 0)) w.resize(w.size() - 2);
        while (!w.empty() && w.back() == '\'') w.pop_back();
        const bool first = initial;
        initial = false;
        if (w.empty() || w[0] < 'A' || w[0] > 'Z') return;
        if (std::any_of(w.begin(), w.end(), [](char c) { return c >= '0' && c <= '9'; })) return;
        if (!canonical_facts(w).empty()) return;
        std::string lower = ascii_lower(w), bare;
        for (char c : lower) if (c != '\'') bare += c;
        // Mid-sentence, a capitalised word is a name unless it is one of these; ordinary words only at a sentence start.
        static const std::set<std::string> kAnywhere = {"i", "dr", "mr", "mrs", "ms", "sir", "madam", "rs", "inr", "ok", "okay"};
        if (!written.count(bare) && (kAnywhere.count(lower) || (first && (kCommon.count(lower) || common_english_word(lower))))) return;
        out.push_back(w.substr(0, w.find('\'')));
    };
    for (char c : text) {
        bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (letter || (c == '\'' && !cur.empty())) {
            if (cur.empty()) { initial = next_initial; next_initial = false; }
            cur += c;
            continue;
        }
        flush();
        if (c == '.' || c == '!' || c == '?' || c == ':' || c == '\n') next_initial = true;
    }
    flush();
    return out;
}

}  // namespace

bool is_pronoun(const std::string &who) {
    static const std::set<std::string> kPronouns = {
        "i", "we", "you", "he", "she", "they",
        "నేను", "మేము", "మనం", "నువ్వు", "మీరు", "అతను", "ఆమె", "ఆయన", "వాళ్ళు",
    };
    return kPronouns.count(ascii_lower(nfc_clean(trim_copy(who)))) > 0;
}

namespace {

// The ways a person may be said: as given, and the quote words of a listed name written that way
// (when the spelling fits).
std::vector<std::string> said_forms(const std::string &who, const std::vector<Name> &names) {
    std::vector<std::string> out = {who};
    for (const auto &n : names)
        if (ascii_lower(n.written) == ascii_lower(who) && n.said != who && transliterates(n.written, n.said))
            out.push_back(n.said);
    return out;
}

// A spoken person is kept only when the recording says it: in the quote, or (a name, not a pronoun)
// just before it with no other name in between. `others`: how the candidate's other names are said.
std::string keep_person(const std::string &who, const std::string &quote, const std::string &before,
                        const std::vector<Name> &names, const std::vector<std::string> &others) {
    std::string w = trim_copy(who);
    if (w.empty()) return "";
    const std::vector<std::string> forms = said_forms(w, names);
    for (const auto &f : forms) if (name_said(f, quote)) return w;
    if (is_pronoun(w)) return "";
    const std::vector<size_t> starts = word_starts(before);
    for (size_t k = starts.size(); k-- > 0;) {
        std::string form;
        for (const auto &f : forms) if (name_said(f, before.substr(starts[k]))) { form = f; break; }
        if (form.empty()) continue;
        // The last place it is said: everything after its words must name nobody else.
        size_t after = k + word_starts(form).size();
        std::string between = after < starts.size() ? before.substr(starts[after]) : "";
        const std::string wl = ascii_lower(w);
        // Whole words only: "Ram" between is someone else even when the person is "Ramesh".
        std::set<std::string> own;
        for (size_t st : word_starts(wl)) { size_t e = wl.find(' ', st); own.insert(wl.substr(st, e == std::string::npos ? std::string::npos : e - st)); }
        for (const auto &nw : name_words(between, {}))
            if (!own.count(ascii_lower(nw))) return "";
        for (const auto &o : others) {
            bool self = std::find(forms.begin(), forms.end(), o) != forms.end() || ascii_lower(o) == wl;
            if (!o.empty() && !self && name_said(o, between)) return "";
        }
        return w;
    }
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
                       {"names", names_json(c)}, {"numbers", string_list(c, "numbers")},
                       {"dates", string_list(c, "dates")}, {"times", string_list(c, "times")},
                       {"start_sec", std::max<int64_t>(0, jint(c, "start_sec", 0))}});
    }
    return out;
}

Grounded ground_memory(const json &cand, const SectionPieces &pieces, const std::vector<std::optional<double>> &piece_conf,
                       const std::string &said_ymd, const std::vector<std::string> &med_names, double low_conf_threshold) {
    Grounded g;
    const std::string type = jstr(cand, "type"), text = jstr(cand, "text"), given_quote = jstr(cand, "quote");

    // 1. Medicines have their own table and rules.
    if (mentions_medicine(text + " " + given_quote, med_names)) { g.reason = "medicine"; return g; }

    // A quote of a word or two ("okay") cannot support a memory.
    // A quote of a word or two ("okay") cannot support a memory: 4 words, or 3 in Telugu, whose words
    // carry their own case endings and person.
    if (med_tokens(nfc_clean(given_quote)).size() < (mostly_telugu(given_quote) ? 3u : 4u)) {
        g.reason = "quote_too_short";
        return g;
    }

    // 2. The quote is said in the section: inside one piece, or across one piece boundary.
    const Tokens qtok = match_tokens(given_quote);
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
    // From here on the quote is the transcript's own span of words, not the model's copy of it.
    std::string scope = pieces[first].second;
    if (count == 2) scope += " " + pieces[first + 1].second;
    const auto span = run_span(scope, qtok);
    std::string quote = trim_copy(scope.substr(span.first, span.second - span.first));
    if (quote.empty()) quote = given_quote;

    // 3. Every name, number, date and time the memory relies on is in the quote.
    json checked = json::array();
    const std::vector<Name> names = name_list(cand);
    for (const auto &n : names) {
        if (!name_said(n.said, quote)) { g.reason = "field_not_in_quote:name"; return g; }
        checked.push_back({{"kind", "name"}, {"text", n.said}, {"as_written", n.written}});
    }
    const struct { const char *key, *kind; } kFields[] = {{"numbers", "number"}, {"dates", "date"}, {"times", "time"}};
    for (const auto &f : kFields)
        for (const auto &v : jarr(cand, f.key)) {
            std::string s = v.is_string() ? trim_copy(v.get<std::string>()) : "";
            if (s.empty()) continue;
            if (!in_run(qtok, s)) { g.reason = std::string("field_not_in_quote:") + f.kind; return g; }
            checked.push_back({{"kind", f.kind}, {"text", s}});
        }

    // 4. Every number and date of the text is in the quote, as often as the text has it.
    std::map<std::string, int> have;
    for (const auto &t : canonical_facts(quote)) ++have[t];
    for (const auto &t : canonical_facts(text))
        if (--have[t] < 0) { g.reason = "text_not_in_quote"; return g; }
    // Every name the text shows (a capitalised word that is not a common word) is said in the quote,
    // or is how a listed name is written and plausibly spells the quote's words for it.
    for (const auto &w : name_words(text, written_words(names))) {
        const std::string wl = ascii_lower(w);
        // An ordinary word used as a name ("Will") must be said as a name: capitalised, whole word.
        if (common_english_word(wl) || wl == "will" || wl == "may") {
            bool cap = false;
            for (size_t at = quote.find(w); at != std::string::npos && !cap; at = quote.find(w, at + 1)) {
                auto edge = [](char c) { return !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')); };
                cap = (at == 0 || edge(quote[at - 1])) && (at + w.size() >= quote.size() || edge(quote[at + w.size()]));
            }
            if (cap) continue;
        } else if (name_said(w, quote)) continue;
        bool listed = std::any_of(names.begin(), names.end(), [&](const Name &n) {
            const Tokens ws = med_tokens(n.written, false);
            return std::find(ws.begin(), ws.end(), wl) != ws.end() && transliterates(n.written, n.said);
        });
        if (!listed) { g.reason = "text_name_not_in_quote"; return g; }
    }

    // 5. who / to_whom only when said.
    const std::string before = bytes_before(joined, offset[first] + span.first, 80);
    std::vector<std::string> said_names;
    for (const auto &n : names) said_names.push_back(n.said);
    std::vector<std::string> others_w = said_names, others_t = said_names;
    for (const auto &f : said_forms(trim_copy(jstr(cand, "to_whom")), names)) others_w.push_back(f);
    for (const auto &f : said_forms(trim_copy(jstr(cand, "who")), names)) others_t.push_back(f);
    const std::string who = keep_person(jstr(cand, "who"), quote, before, names, others_w);
    const std::string to_whom = keep_person(jstr(cand, "to_whom"), quote, before, names, others_t);

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
    // 9. Names, numbers, dates and promises get the yes/no check, and so does every memory that could
    // be shown without the person's review (only a "yes" lets it skip the queue).
    g.needs_double_check = !checked.empty() || type == "promise" || !goes_to_queue(g.row, 1, true);
    return g;
}

std::string double_check_prompt(const json &row) {
    std::string quote;
    const json src = jarr(row, "sources");
    if (!src.empty() && src[0].is_object()) quote = jstr(src[0], "quote");
    std::string claim = "Memory: \"" + jstr(row, "text") + "\"";
    if (!jstr(row, "who").empty()) claim += "\nWho: " + jstr(row, "who");
    if (!jstr(row, "to_whom").empty()) claim += "\nTo whom: " + jstr(row, "to_whom");
    if (!jstr(row, "due_text").empty()) {
        claim += "\nBy: " + jstr(row, "due_text");
        if (!jstr(row, "due_date").empty())
            claim += " (" + jstr(row, "due_date") + ", said on " + jstr(row, "said_date") + ")";
    }
    return "Quote: \"" + quote + "\"\n" + claim + "\nDoes the quote say this? Answer yes or no.";
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
    return truthy(row, "health") || jstr(row, "confidence") == "low" || double_check != 1 ||
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

bool memory_on_map(const json &m) {
    if (!m.is_object() || jint(m, "queue", 0) != 0) return false;
    if (m.contains("superseded_by") && !m["superseded_by"].is_null()) return false;
    const std::string review = jstr(m, "review");
    if (review == "wrong") return false;
    return kDirectToMapDefault || review == "kept" || review == "corrected";
}

const char *memory_map_review_sql() {
    return kDirectToMapDefault ? " AND m.review<>'wrong'" : " AND m.review IN ('kept','corrected')";
}

}  // namespace sve
