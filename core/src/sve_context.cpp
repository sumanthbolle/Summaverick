#include "sve_context.h"
#include "sve_english_words.h"
#include <cctype>
#include <algorithm>
#include "sve_prompts.h"
#include "sve_sections.h"  // terms_said
#include <vector>

namespace sve {

const char *const kVocabOnlyIfSaid = "spellings only: use one only if the transcript says it; never add it as a topic, term, hint or point";

namespace {

std::string str_of(const json &j, const char *k) {
    if (!j.is_object()) return "";
    auto it = j.find(k);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : "";
}

std::string timestamp(long long sec) {
    if (sec < 0) sec = 0;
    long long m = sec / 60, s = sec % 60;
    return "@" + std::to_string(m) + ":" + (s < 10 ? "0" : "") + std::to_string(s);
}

std::string medicine_line(const json &m) {
    std::string name = str_of(m, "name"), dose = str_of(m, "dose"), when = str_of(m, "when_text");
    std::string repeat = str_of(m, "repeat");
    std::string times;
    if (m.contains("times") && m["times"].is_array())
        for (const auto &t : m["times"])
            if (t.is_string()) times += (times.empty() ? "" : ", ") + t.get<std::string>();
    std::string line = "- " + name;
    if (!dose.empty()) line += " " + dose;
    std::string mid = when;
    if (!times.empty()) mid += (mid.empty() ? "" : " ") + std::string("(") + times + ")";
    if (!mid.empty()) line += " \xE2\x80\x94 " + mid;
    if (!repeat.empty()) line += " " + repeat;
    return line;
}

// Newest first for display; `rows` is oldest first, `from` is the first kept index.
std::string build_always(const json &profile, const json &meds, const std::vector<std::string> &prefs,
                         size_t pref_from, const std::vector<std::string> &vocab, size_t vocab_from) {
    std::string out;
    std::string about;
    std::string name = str_of(profile, "name"), lang = str_of(profile, "lang"), style = str_of(profile, "style");
    if (!name.empty()) about += "Name: " + name + "\n";
    if (!lang.empty()) about += "Preferred language: " + language_name(lang) + "\n";
    if (!style.empty()) about += "Speaking style: " + style + "\n";
    if (profile.is_object() && profile.contains("routine") && profile["routine"].is_object()) {
        std::string r;
        for (const char *k : {"wake", "breakfast", "lunch", "dinner", "bed", "classes"}) {
            std::string v = str_of(profile["routine"], k);
            if (!v.empty()) r += (r.empty() ? "" : ", ") + std::string(k) + " " + v;
        }
        if (!r.empty()) about += "Routine: " + r + "\n";
    }
    if (!about.empty()) out += "About the person:\n" + about;
    bool any_med = false;
    if (meds.is_array()) for (const auto &m : meds) if (!str_of(m, "name").empty()) any_med = true;
    if (any_med) {
        out += std::string(out.empty() ? "" : "\n") + "Medicines:\n";
        for (const auto &m : meds) if (!str_of(m, "name").empty()) out += medicine_line(m) + "\n";
    }
    if (pref_from < prefs.size()) {
        out += std::string(out.empty() ? "" : "\n") + "Preferences:\n";
        for (size_t i = prefs.size(); i-- > pref_from;) out += "- " + prefs[i] + "\n";
    }
    if (vocab_from < vocab.size()) {
        out += std::string(out.empty() ? "" : "\n") + "Words to spell this way (" + kVocabOnlyIfSaid + "): ";
        bool first = true;
        for (size_t i = vocab.size(); i-- > vocab_from;) {
            out += (first ? "" : ", ") + vocab[i];
            first = false;
        }
        out += "\n";
    }
    return out;
}

std::vector<std::string> texts(const json &rows) {
    std::vector<std::string> v;
    if (rows.is_array())
        for (const auto &r : rows) {
            std::string t = str_of(r, "text");
            if (!t.empty()) v.push_back(t);
        }
    return v;
}

struct Selection {
    std::vector<std::string> facts;
    json pieces = json::array();
};

// Single source of truth for what relevant_block labels and relevant_sources returns.
Selection select_relevant(Store &store, const std::string &query, int64_t from = 0, int64_t to = 0) {
    Selection sel;
    if (to == 0) sel.facts = texts(store.search_memory_items(query, 5));  // current memories: not wrong, not superseded
    json p = to == 0 ? store.search_pieces(query, 6) : store.search_pieces_between(query, from, to, 6);
    if (p.is_array()) sel.pieces = p;
    return sel;
}

} // namespace

std::string truncate_utf8(const std::string &s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    size_t n = max_bytes;
    // If the byte at n is a continuation byte, we'd split a code point: back off to its lead.
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) n--;
    return s.substr(0, n);
}

std::vector<std::string> saved_vocab(Store &store) { return texts(store.memory("vocab")); }

std::string always_block(Store &store, size_t max_bytes, bool medicines, const std::string *said) {
    json profile;
    if (auto p = store.setting_get("profile")) {
        profile = json::parse(*p, nullptr, false);
        if (profile.is_discarded()) profile = json();
    }
    json meds = medicines ? store.medicines() : json::array();
    std::vector<std::string> prefs = texts(store.memory("preference"));
    std::vector<std::string> vocab = texts(store.memory("vocab"));
    if (said) vocab = terms_said(vocab, *said);
    size_t pf = 0, vf = 0;
    std::string out = build_always(profile, meds, prefs, pf, vocab, vf);
    while (out.size() > max_bytes && vf < vocab.size()) out = build_always(profile, meds, prefs, pf, vocab, ++vf);
    while (out.size() > max_bytes && pf < prefs.size()) out = build_always(profile, meds, prefs, ++pf, vocab, vf);
    return truncate_utf8(out, max_bytes);
}

std::string context_system(Store &store, const std::string &system, bool medicines, const std::string *said) {
    std::string ab = always_block(store, 2000, medicines, said);
    if (ab.empty()) return system;
    return "Background about the person you work for (use it for names, spellings and preferences; "
           "do not repeat it unless it is asked for):\n" + ab + "\n" + system;
}

namespace {

// head + the terms (given oldest first) newest first, whole terms within max_bytes; "" when no term.
std::string vocab_line(const std::vector<std::string> &vocab, const std::string &head, size_t max_bytes) {
    std::string out;
    for (size_t i = vocab.size(); i-- > 0;) {
        std::string add = (out.empty() ? head : std::string(", ")) + vocab[i];
        if (out.size() + add.size() > max_bytes) {
            if (out.empty()) out = truncate_utf8(add, max_bytes);  // one very long term: cut it
            break;
        }
        out += add;
    }
    return out.size() > head.size() ? out : std::string();
}

}  // namespace

std::string transcription_vocab_line(Store &store, size_t max_bytes) {
    return vocab_line(texts(store.memory("vocab")), "Spell these words this way: ", max_bytes);  // oldest first
}

std::string spelling_vocab_line(Store &store, size_t max_bytes, const std::string *said) {
    // The words that are said, newest first, within the transcription line's budget (its head is shorter: the
    // budget counts the words, not the rule).
    static const std::string head = "Spell these words this way: ";
    std::vector<std::string> vocab = texts(store.memory("vocab"));
    if (said) vocab = terms_said(vocab, *said);
    const std::string line = vocab_line(vocab, head, max_bytes);
    if (line.empty()) return line;
    return "Spell these words this way (" + std::string(kVocabOnlyIfSaid) + "): " + line.substr(head.size());
}

namespace {

// UTF-8 to code points (a bad byte stands for itself).
std::u32string code_points(const std::string &s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (i + n > s.size()) n = 1;
        char32_t cp = n == 1 ? c : (c & (0xFF >> (n + 1)));
        for (size_t k = 1; k < n; k++) cp = (cp << 6) | (s[i + k] & 0x3F);
        out.push_back(cp);
        i += n;
    }
    return out;
}

size_t edit_distance(const std::u32string &a, const std::u32string &b) {
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) prev[j] = j;
    for (size_t i = 1; i <= a.size(); i++) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); j++)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

bool edge_punct(unsigned char c) { return c < 0x80 && !std::isalnum(c); }

// Lowercase ASCII, punctuation and spaces taken off; other scripts kept as they are.
std::string match_key(const std::string &s) {
    std::string k;
    for (unsigned char c : s) {
        if (c >= 0x80) k.push_back((char)c);
        else if (std::isalnum(c)) k.push_back((char)std::tolower(c));
    }
    return k;
}

size_t word_count(const std::string &s) {
    size_t n = 0;
    bool in = false;
    for (unsigned char c : s) {
        bool sp = std::isspace(c);
        if (!sp && !in) n++;
        in = !sp;
    }
    return n;
}

struct Term { std::string text, key; std::u32string cps; size_t words; };

// 0 exact, else the distance over the longer length; -1 when it is not close enough.
double closeness(const std::string &span_key, const Term &t) {
    if (span_key == t.key) return 0;
    const std::u32string a = code_points(span_key);
    const size_t len = std::max(a.size(), t.cps.size());
    if (len < 6) return -1;
    const double r = (double)edit_distance(a, t.cps) / (double)len;
    return r <= (len >= 10 ? 0.25 : 0.2) ? r : -1;
}

}  // namespace

std::string vocab_correct(const std::string &text, const std::vector<std::string> &terms,
                          const std::vector<std::string> &protected_names) {
    if (text.empty() || terms.empty()) return text;
    std::vector<std::string> keep;
    for (const auto &p : protected_names) { std::string k = match_key(p); if (!k.empty()) keep.push_back(k); }
    std::vector<Term> ts;
    for (const auto &t : terms) {
        Term x{t, match_key(t), {}, word_count(t)};
        if (x.key.empty() || !x.words) continue;
        x.cps = code_points(x.key);
        bool guarded = false;
        for (const auto &k : keep) {
            Term kt{k, k, code_points(k), 1};
            if (closeness(x.key, kt) >= 0) { guarded = true; break; }
        }
        if (!guarded) ts.push_back(x);
    }
    if (ts.empty()) return text;

    // Words with the spaces before them, as written.
    struct Word { std::string space, lead, core, trail; };
    std::vector<Word> w;
    for (size_t i = 0; i < text.size();) {
        Word x;
        while (i < text.size() && std::isspace((unsigned char)text[i])) x.space.push_back(text[i++]);
        std::string tok;
        while (i < text.size() && !std::isspace((unsigned char)text[i])) tok.push_back(text[i++]);
        size_t a = 0, b = tok.size();
        while (a < b && edge_punct((unsigned char)tok[a])) a++;
        while (b > a && edge_punct((unsigned char)tok[b - 1])) b--;
        x.lead = tok.substr(0, a);
        x.core = tok.substr(a, b - a);
        x.trail = tok.substr(b);
        w.push_back(x);
    }
    auto starts_digit = [](const std::string &s) { return !s.empty() && std::isdigit((unsigned char)s[0]); };

    struct Best { double score = -1; size_t n = 0; const Term *t = nullptr; };
    auto best_at = [&](size_t i) {
        Best b;
        std::string span;
        for (size_t n = 1; n <= 4 && i + n <= w.size(); n++) {
            const Word &last = w[i + n - 1];
            if (last.core.empty() || (n > 1 && !w[i + n - 2].trail.empty())) break;  // no span across punctuation
            if (starts_digit(last.core)) break;                                      // numbers are never a term
            span += match_key(last.core);
            if (i + n < w.size() && starts_digit(w[i + n].core)) continue;           // right before a dose
            bool all_common = true;
            for (size_t k = i; k < i + n; k++) {
                std::string low;
                for (unsigned char c : w[k].core) low.push_back((char)std::tolower(c));
                all_common = all_common && common_english_word(low);
            }
            for (const auto &t : ts) {
                if (n > std::max<size_t>(t.words, 2)) continue;  // a term's own words, or a word split in two
                const double r = closeness(span, t);
                if (r < 0) continue;
                if (n == 1 && all_common && t.words == 1) continue;                  // a common word: kept as said
                if (r > 0 && (t.key == span + "s" || span == t.key + "s")) continue;  // plural or singular
                if (b.score < 0 || r < b.score || (r == b.score && n > b.n)) { b.score = r; b.n = n; b.t = &t; }
            }
        }
        return b;
    };

    std::string out;
    for (size_t i = 0; i < w.size();) {
        Best b = best_at(i);
        // A match that takes in the word before a term ("but perplexity"): the term from the next word is as close
        // or closer, so this word stays as it was.
        if (b.t && b.n > 1 && i + 1 < w.size()) {
            Best next = best_at(i + 1);
            if (next.t && next.score <= b.score) b = Best{};
        }
        const Word &first = w[i];
        if (!b.t) {
            out += first.space + first.lead + first.core + first.trail;
            i++;
            continue;
        }
        out += first.space + first.lead + b.t->text + w[i + b.n - 1].trail;
        i += b.n;
    }
    return out;
}

Relevant relevant(Store &store, const std::string &query, size_t max_bytes, int64_t from, int64_t to) {
    Selection sel = select_relevant(store, query, from, to);
    Relevant r;
    r.sources = json::array();
    std::string pieces, facts;
    const std::string phead = "From recordings:\n", fhead = "Remembered facts:\n";
    size_t used = 0;
    int n = 0;
    for (const auto &p : sel.pieces) {
        long long sec = p.contains("start_sec") && p["start_sec"].is_number() ? p["start_sec"].get<long long>() : 0;
        std::string label = "S" + std::to_string(n + 1);
        std::string line = "[" + label + "] " + str_of(p, "title") + " " + timestamp(sec) + ": " + str_of(p, "text") + "\n";
        std::string add = (n == 0 ? phead : std::string()) + line;
        if (used + add.size() > max_bytes) {
            if (n == 0) {
                pieces = truncate_utf8(add, max_bytes);
                used = pieces.size();
                if (pieces.find("[" + label + "]") == std::string::npos) break;
                n = 1;
                r.sources.push_back({{"label", label}, {"note_id", p.value("note_id", (int64_t)0)},
                                     {"start_sec", p.value("start_sec", (int64_t)0)}, {"title", str_of(p, "title")}});
            }
            break;
        }
        pieces += add;
        used += add.size();
        n++;
        r.sources.push_back({{"label", label}, {"note_id", p.value("note_id", (int64_t)0)},
                             {"start_sec", p.value("start_sec", (int64_t)0)}, {"title", str_of(p, "title")}});
    }
    bool first = true;
    for (const auto &f : sel.facts) {
        std::string add = (first ? std::string(used ? "\n" : "") + fhead : std::string()) + "- " + f + "\n";
        if (used + add.size() > max_bytes) break;
        facts += add;
        used += add.size();
        first = false;
    }
    r.block = pieces + facts;
    return r;
}

std::string relevant_block(Store &store, const std::string &query, size_t max_bytes) {
    return relevant(store, query, max_bytes).block;
}

json relevant_sources(Store &store, const std::string &query, size_t max_bytes) {
    return relevant(store, query, max_bytes).sources;
}

} // namespace sve
