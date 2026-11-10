#include "sve_consolidate.h"

#include <algorithm>
#include <cstdint>
#include <set>

#include "sve_medcheck.h"   // med_tokens
#include "sve_memory.h"     // is_pronoun
#include "sve_suggest.h"    // jstr, jint, jarr, trim_copy
#include "sve_textmatch.h"  // nfc_clean, match_tokens, canonical_facts, kTeluguCaseEndings

namespace sve {
namespace {

// ---------------------------------------------------------------- Telugu to Latin

bool next_cp(const std::string &s, size_t &i, uint32_t &cp) {
    unsigned char c = (unsigned char)s[i];
    size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (n == 0 || i + n > s.size()) { cp = 0x110000 + c; ++i; return true; }  // stray byte: kept as is
    uint32_t v = n == 1 ? c : c & (0xFF >> (n + 1));
    for (size_t k = 1; k < n; ++k) v = (v << 6) | ((unsigned char)s[i + k] & 0x3F);
    cp = v;
    i += n;
    return true;
}

void put_cp(std::string &o, uint32_t cp) {
    if (cp >= 0x110000) { o += (char)(cp - 0x110000); return; }
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
    } else {
        o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F));
        o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
    }
}

// U+0C15 క .. U+0C39 హ. Aspirates keep their h here; person_norm folds them for both scripts alike.
const char *kConsonants[] = {
    "k", "kh", "g", "gh", "n", "ch", "chh", "j", "jh", "n",   // క ఖ గ ఘ ఙ చ ఛ జ ఝ ఞ
    "t", "th", "d", "dh", "n", "t", "th", "d", "dh", "n",     // ట ఠ డ ఢ ణ త థ ద ధ న
    "n", "p", "ph", "b", "bh", "m", "y", "r", "r", "l",       // (U+0C29) ప ఫ బ భ మ య ర ఱ ల
    "l", "l", "v", "sh", "sh", "s", "h",                      // ళ ఴ వ శ ష స హ
};

// Independent vowels and vowel signs. Long vowels are written the way English spells them (aa, ee, oo) and
// come out short after person_norm; long e and o are plain e and o, as English names write them.
const char *independent_vowel(uint32_t cp) {
    switch (cp) {
        case 0x0C05: return "a";  case 0x0C06: return "aa"; case 0x0C07: return "i";  case 0x0C08: return "ee";
        case 0x0C09: return "u";  case 0x0C0A: return "oo"; case 0x0C0B: return "ri"; case 0x0C0C: return "lu";
        case 0x0C0E: return "e";  case 0x0C0F: return "e";  case 0x0C10: return "ai"; case 0x0C12: return "o";
        case 0x0C13: return "o";  case 0x0C14: return "au"; case 0x0C60: return "ri"; case 0x0C61: return "lu";
        default: return nullptr;
    }
}

const char *vowel_sign(uint32_t cp) {
    switch (cp) {
        case 0x0C3E: return "aa"; case 0x0C3F: return "i";  case 0x0C40: return "ee"; case 0x0C41: return "u";
        case 0x0C42: return "oo"; case 0x0C43: return "ri"; case 0x0C44: return "ri"; case 0x0C46: return "e";
        case 0x0C47: return "e";  case 0x0C48: return "ai"; case 0x0C4A: return "o";  case 0x0C4B: return "o";
        case 0x0C4C: return "au"; case 0x0C62: return "lu"; case 0x0C63: return "lu";
        default: return nullptr;
    }
}

const char *consonant(uint32_t cp) {
    if (cp >= 0x0C15 && cp <= 0x0C39) return kConsonants[cp - 0x0C15];
    if (cp == 0x0C58) return "ch";  // ౘ
    if (cp == 0x0C59) return "j";   // ౙ
    if (cp == 0x0C5A) return "r";   // ౚ
    return nullptr;
}

bool is_telugu(uint32_t cp) { return cp >= 0x0C00 && cp <= 0x0C7F; }

bool has_telugu(const std::string &s) {
    for (size_t i = 0; i < s.size();) {
        uint32_t cp;
        next_cp(s, i, cp);
        if (is_telugu(cp)) return true;
    }
    return false;
}

// Spoken syllables of a Telugu word: independent vowels, and consonants that carry a vowel (no virama after).
int telugu_syllables(const std::string &w) {
    std::vector<uint32_t> cps;
    for (size_t i = 0; i < w.size();) { uint32_t cp; next_cp(w, i, cp); cps.push_back(cp); }
    int n = 0;
    for (size_t k = 0; k < cps.size(); ++k) {
        if (independent_vowel(cps[k])) ++n;
        else if (consonant(cps[k]) && !(k + 1 < cps.size() && cps[k + 1] == 0x0C4D)) ++n;
    }
    return n;
}

bool letter(char c) { return c >= 'a' && c <= 'z'; }
bool vowel(char c) { return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u'; }

std::string ascii_lower(std::string s) {
    for (auto &c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return s;
}

// ---------------------------------------------------------------- names

// Honorifics, in one canonical form per meaning (garu and గారు are the same honorific).
const char *honorific(const std::string &t) {
    static const std::pair<const char *, const char *> k[] = {
        {"dr", "dr"}, {"mr", "mr"}, {"mrs", "mrs"}, {"ms", "ms"}, {"sir", "sir"}, {"madam", "madam"},
        {"garu", "garu"}, {"గారు", "garu"}, {"గారూ", "garu"}, {"గారి", "garu"}, {"గారికి", "garu"}, {"గారితో", "garu"},
        {"గారిని", "garu"}, {"అన్న", "anna"}, {"అన్నా", "anna"}, {"అక్క", "akka"}, {"సార్", "sir"},
        {"మేడం", "madam"}, {"మేడమ్", "madam"},
    };
    for (auto &h : k) if (t == h.first) return h.second;
    return nullptr;
}

struct NameParts {
    std::vector<std::string> words;  // name words in the shared Latin form
    std::set<std::string> honorifics;
};

NameParts name_parts(const std::string &name) {
    NameParts p;
    std::vector<std::string> raw = med_tokens(nfc_clean(name), false), kept;
    for (const auto &t : raw) {
        if (const char *h = honorific(t)) p.honorifics.insert(h);
        else kept.push_back(t);
    }
    if (kept.empty()) { kept = raw; p.honorifics.clear(); }  // all honorific ("Sir"): the words are the name
    if (!kept.empty() && has_telugu(kept.back())) {
        // The last word's case ending (longest first); a short name that only looks like one keeps it.
        static const std::vector<std::string> endings = [] {
            std::vector<std::string> e = kTeluguCaseEndings;
            std::stable_sort(e.begin(), e.end(), [](const std::string &a, const std::string &b) { return a.size() > b.size(); });
            return e;
        }();
        std::string &w = kept.back();
        for (const auto &e : endings) {
            if (w.size() <= e.size() || w.compare(w.size() - e.size(), e.size(), e) != 0) continue;
            const std::string stem = w.substr(0, w.size() - e.size());
            if (telugu_syllables(stem) < 2) continue;
            if (const char *h = honorific(e)) p.honorifics.insert(h);
            w = stem;
            break;
        }
    }
    for (const auto &t : kept) {
        std::string k = transliterate_te(t);
        if (!k.empty()) p.words.push_back(k);
    }
    return p;
}

std::string join(const std::vector<std::string> &w) {
    std::string o;
    for (const auto &x : w) o += (o.empty() ? "" : " ") + x;
    return o;
}

std::string raw_norm(const std::string &s) {
    std::string o;
    for (char c : ascii_lower(nfc_clean(s))) {
        bool sp = c == ' ' || c == '\t' || c == '\n' || c == '\r';
        if (sp) { if (!o.empty() && o.back() != ' ') o += ' '; }
        else o += c;
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}

size_t edit_distance(const std::string &a, const std::string &b) {
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); ++j)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

size_t letters(const std::string &s) {
    size_t n = 0;
    for (char c : s) if (c != ' ') ++n;
    return n;
}

bool subset(const std::vector<std::string> &a, const std::vector<std::string> &b) {
    for (const auto &w : a) if (std::find(b.begin(), b.end(), w) == b.end()) return false;
    return true;
}

bool eligible_name(const std::string &name) { return !name.empty() && name.size() <= 80 && !is_pronoun(name); }

// ---------------------------------------------------------------- memories

const std::set<std::string> &stopwords() {
    static const std::set<std::string> k = {
        "a", "an", "the", "is", "are", "was", "were", "be", "been", "to", "of", "in", "on", "at", "for", "and", "or",
        "with", "will", "would", "it", "this", "that", "he", "she", "they", "i", "we", "you", "his", "her", "their",
        "my", "our", "your", "has", "have", "had", "do", "does", "did", "by", "from", "as", "about", "also", "said",
        "ఆ", "ఈ", "అని", "కూడా", "ఉంది", "ఉన్నారు", "ఉన్నాడు", "చెప్పారు", "చెప్పాడు", "మరియు", "లో", "కి", "కు",
        "నా", "మా", "మీ", "అతను", "ఆమె", "ఆయన", "వాళ్ళు", "నేను", "మేము", "ఇది", "అది", "గారు", "కోసం", "తో",
    };
    return k;
}

std::set<std::string> content_tokens(const std::string &text) {
    std::vector<std::string> facts = canonical_facts(text);
    std::set<std::string> f(facts.begin(), facts.end()), out;
    for (const auto &t : match_tokens(text))
        if (!f.count(t) && !stopwords().count(t)) out.insert(t);
    return out;
}

double jaccard(const std::set<std::string> &a, const std::set<std::string> &b) {
    if (a.empty() && b.empty()) return 0;
    size_t inter = 0;
    for (const auto &x : a) inter += b.count(x);
    return double(inter) / double(a.size() + b.size() - inter);
}

// The checked fields of one kind, in order.
std::vector<json> fields(const json &row, const char *kind) {
    std::vector<json> out;
    for (const auto &c : jarr(row, "checked"))
        if (c.is_object() && jstr(c, "kind") == kind) out.push_back(c);
    return out;
}

std::vector<std::string> field_facts(const json &field) {
    std::vector<std::string> f = canonical_facts(jstr(field, "text"));
    return f.empty() ? std::vector<std::string>{raw_norm(jstr(field, "text"))} : f;
}

std::vector<std::string> kind_facts(const json &row, const char *kind) {
    std::vector<std::string> out;
    for (const auto &c : fields(row, kind))
        for (const auto &f : field_facts(c)) out.push_back(f);
    std::sort(out.begin(), out.end());
    return out;
}

bool facts_equal(const json &a, const json &b) {
    std::vector<std::string> ta = canonical_facts(jstr(a, "text")), tb = canonical_facts(jstr(b, "text"));
    std::sort(ta.begin(), ta.end());
    std::sort(tb.begin(), tb.end());
    return ta == tb && kind_facts(a, "number") == kind_facts(b, "number") && kind_facts(a, "date") == kind_facts(b, "date");
}

bool differing_fact(const json &a, const json &b) {
    for (const char *k : {"number", "date"}) {
        std::vector<std::string> fa = kind_facts(a, k), fb = kind_facts(b, k);
        if (!fa.empty() && !fb.empty() && fa != fb) return true;
    }
    return false;
}

std::set<int64_t> ids(const json &row, const char *key) {
    std::set<int64_t> out;
    for (const auto &x : jarr(row, key)) {
        int64_t id = jint(x, "id", 0);
        if (id > 0) out.insert(id);
    }
    return out;
}

bool intersects(const std::set<int64_t> &a, const std::set<int64_t> &b) {
    for (auto x : a) if (b.count(x)) return true;
    return false;
}

bool share_entity(const json &a, const json &b) {
    std::set<int64_t> pa = ids(a, "people"), pb = ids(b, "people");
    if (!pa.empty() || !pb.empty()) return intersects(pa, pb);
    return intersects(ids(a, "topics"), ids(b, "topics"));
}

}  // namespace

// ---------------------------------------------------------------- names

std::string person_norm(const std::string &in) {
    std::string s = ascii_lower(in), o;
    // x -> ks, w -> v, aspirates as their plain letter (sh -> s, th -> t, ...), chh -> ch.
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i], n = i + 1 < s.size() ? s[i + 1] : 0;
        if (c == 'x') { o += "ks"; continue; }
        if (c == 'w') { o += 'v'; continue; }
        if (c == 'c' && n == 'h') {
            o += "ch";
            i += 1;
            while (i + 1 < s.size() && s[i + 1] == 'h') ++i;
            continue;
        }
        if (n == 'h' && std::string("sktdgjbp").find(c) != std::string::npos) { o += c; ++i; continue; }
        o += c;
    }
    // aa -> a, ee -> i, oo -> u (English spellings of long vowels).
    s.swap(o);
    o.clear();
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i], n = i + 1 < s.size() ? s[i + 1] : 0;
        if (c == 'a' && n == 'a') { o += 'a'; ++i; }
        else if (c == 'e' && n == 'e') { o += 'i'; ++i; }
        else if (c == 'o' && n == 'o') { o += 'u'; ++i; }
        else o += c;
    }
    // ayi -> ai (సాయి is Sai), then doubled letters collapsed.
    s.swap(o);
    o.clear();
    for (size_t i = 0; i < s.size(); ++i) {
        if (s.compare(i, 3, "ayi") == 0) { o += "ai"; i += 2; continue; }
        if (!o.empty() && o.back() == s[i] && letter(s[i])) continue;
        o += s[i];
    }
    // A nasal before another consonant is n (ఆనంద్ is Anand), except m before b or p; a final -avu is -ao
    // (రావు is Rao).
    s.swap(o);
    o.clear();
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i], n = i + 1 < s.size() ? s[i + 1] : 0;
        if (c == 'm' && letter(n) && !vowel(n) && n != 'b' && n != 'p' && n != 'y' && n != 'r' && n != 'l' && n != 'v') {
            o += 'n';
            continue;
        }
        if (c == 'a' && n == 'v' && i + 2 < s.size() && s[i + 2] == 'u' && (i + 3 >= s.size() || !letter(s[i + 3]))) {
            o += "ao";
            i += 2;
            continue;
        }
        o += c;
    }
    return o;
}

std::string transliterate_te(const std::string &in) {
    const std::string s = nfc_clean(in);
    std::string o;
    bool pending = false;  // a consonant whose inherent "a" is not written yet
    for (size_t i = 0; i < s.size();) {
        uint32_t cp;
        next_cp(s, i, cp);
        if (const char *c = consonant(cp)) {
            if (pending) o += 'a';
            o += c;
            pending = true;
        } else if (const char *v = vowel_sign(cp)) {
            o += v;
            pending = false;
        } else if (cp == 0x0C4D) {  // virama
            pending = false;
        } else if (cp == 0x0C55 || cp == 0x0C56 || cp == 0x0C3C) {
            // length marks and nukta: nothing to write
        } else {
            if (pending) { o += 'a'; pending = false; }
            if (const char *iv = independent_vowel(cp)) o += iv;
            else if (cp == 0x0C00 || cp == 0x0C01 || cp == 0x0C02) o += 'm';  // ఀ ఁ ం
            else if (cp == 0x0C03) o += 'h';                                 // ః
            else if (cp >= 0x0C66 && cp <= 0x0C6F) o += (char)('0' + (cp - 0x0C66));
            else if (is_telugu(cp)) {}                                       // other signs: nothing
            else put_cp(o, cp);
        }
    }
    if (pending) o += 'a';
    return person_norm(o);
}

std::string person_key(const std::string &name) { return join(name_parts(name).words); }

PersonMatch match_person(const std::string &a, const std::string &b) {
    const NameParts pa = name_parts(a), pb = name_parts(b);
    const std::string ka = join(pa.words), kb = join(pb.words);
    if (ka.empty() || kb.empty()) return PersonMatch::different;
    if (raw_norm(a) == raw_norm(b)) return PersonMatch::same;
    if (ka == kb && pa.honorifics == pb.honorifics) return PersonMatch::same;
    if (subset(pa.words, pb.words) || subset(pb.words, pa.words)) return PersonMatch::maybe;
    if (edit_distance(ka, kb) <= 1 && std::min(letters(ka), letters(kb)) >= 4) return PersonMatch::maybe;
    return PersonMatch::different;
}

const char *kSamePersonSystem =
    "You check whether two names from someone's recordings are the same person. Answer only yes or no.";

std::string same_person_prompt(const std::string &a_name, const std::string &a_context, const std::string &b_name,
                               const std::string &b_context) {
    std::string p = "Person A: " + a_name;
    if (!a_context.empty()) p += "\nWhat was said about A: " + a_context;
    p += "\nPerson B: " + b_name;
    if (!b_context.empty()) p += "\nWhat was said about B: " + b_context;
    p += "\n\nAre A and B the same person? Answer yes or no.";
    return p;
}

// ---------------------------------------------------------------- memories

Pair classify_pair(const json &newer, const json &older, double cosine) {
    if (!newer.is_object() || !older.is_object() || jstr(newer, "type") != jstr(older, "type")) return Pair::unrelated;
    if (!share_entity(newer, older)) return Pair::unrelated;
    const double sim = jaccard(content_tokens(jstr(newer, "text")), content_tokens(jstr(older, "text")));
    if ((sim >= 0.6 || cosine >= 0.9) && facts_equal(newer, older)) return Pair::same;
    if ((sim >= 0.4 || cosine >= 0.8) && differing_fact(newer, older)) {
        const std::string dn = jstr(newer, "said_date"), d_old = jstr(older, "said_date");
        // A missing date cannot say which came later: the person decides.
        if (dn.empty() || d_old.empty() || dn == d_old) return Pair::contradiction;
        if (dn > d_old) return Pair::update;
    }
    return Pair::unrelated;
}

json change_line(const json &older, const json &newer) {
    for (const char *k : {"number", "date"}) {
        std::vector<json> fo = fields(older, k), fn = fields(newer, k);
        for (size_t i = 0; i < fo.size() && i < fn.size(); ++i) {
            if (field_facts(fo[i]) == field_facts(fn[i])) continue;
            return {{"was", jstr(fo[i], "text")}, {"now", jstr(fn[i], "text")}, {"said", jstr(newer, "said_date")}};
        }
    }
    return nullptr;
}

json with_change(json row) {
    if (!row.is_object()) return row;
    const json hist = jarr(row, "history");
    if (hist.empty()) return row;
    json c = change_line(hist[0], row);
    if (!c.is_null()) row["change"] = c;
    return row;
}

Placement place_memory(const json &newer, const json &current, double cosine) {
    Placement best;
    if (!current.is_array()) return best;
    const int64_t self = jint(newer, "id", 0);
    for (const auto &r : current) {
        const int64_t id = jint(r, "id", 0);
        if (id <= 0 || id == self || jstr(r, "origin") == "medicine" || jstr(r, "review") == "wrong") continue;
        if (r.contains("superseded_by") && !r["superseded_by"].is_null()) continue;
        Pair p = classify_pair(newer, r, cosine);
        bool flipped = false;
        if (p == Pair::unrelated && classify_pair(r, newer, cosine) == Pair::update) { p = Pair::update; flipped = true; }
        if (p == Pair::unrelated) continue;
        if (p == Pair::same) return {p, id, false};
        if (best.pair == Pair::unrelated) best = {p, id, flipped};
    }
    return best;
}

bool apply_placement(Store &st, const Placement &p, int64_t new_id) {
    if (p.older <= 0 || new_id <= 0) return p.pair == Pair::unrelated || p.pair == Pair::same;
    switch (p.pair) {
        case Pair::update:
            return p.newer_is_older ? st.update_memory_item(new_id, {{"superseded_by", p.older}})
                                    : st.update_memory_item(p.older, {{"superseded_by", new_id}});
        case Pair::contradiction:
            return st.update_memory_item(new_id, {{"conflict_with", p.older}, {"queue", 1}}) &&
                   st.update_memory_item(p.older, {{"queue", 1}});
        default:
            return true;
    }
}

bool merge_same_memory(Store &st, int64_t older, int64_t note_id, int64_t start_sec, const std::string &quote,
                       const json &conf, int64_t now) {
    json row = st.memory_item(older);
    if (!row.is_object()) return false;
    std::optional<double> c;
    if (conf.is_number()) c = conf.get<double>();
    if (!st.add_memory_source(older, note_id, start_sec, quote, c)) return false;
    return st.update_memory_item(older, {{"last_seen", std::max<int64_t>(jint(row, "last_seen", 0), now)}});
}

std::vector<std::pair<std::string, json>> person_checks(const std::vector<std::string> &names, const json &person_topics) {
    std::vector<std::pair<std::string, json>> out;
    std::set<std::string> seen;
    for (const auto &raw : names) {
        const std::string name = trim_copy(raw);
        if (!eligible_name(name) || !seen.insert(name).second) continue;
        std::vector<json> maybes;
        bool same = false;
        for (const auto &t : person_topics) {
            PersonMatch m = match_person(name, jstr(t, "name"));
            if (m == PersonMatch::same) { same = true; break; }
            if (m == PersonMatch::maybe) maybes.push_back(t);
        }
        if (same) continue;
        for (auto &t : maybes) out.push_back({name, t});
    }
    return out;
}

int64_t resolve_person(Store &st, int64_t note_id, const std::string &raw, const std::string &self_lower,
                       const std::string &context, const PersonAnswers &answers) {
    const std::string name = trim_copy(raw);
    if (!eligible_name(name) || (!self_lower.empty() && ascii_lower(name) == self_lower)) return 0;
    int64_t same = 0;
    bool exact = false;
    std::vector<json> maybes;
    for (const auto &t : st.topics("person")) {
        const std::string tn = jstr(t, "name");
        PersonMatch m = match_person(name, tn);
        if (m == PersonMatch::same) {
            // Several known people may be the same (duplicates made before this check): the one written the
            // same way first, else the first by name.
            bool ex = raw_norm(name) == raw_norm(tn);
            if (!same || (ex && !exact)) { same = jint(t, "id", 0); exact = ex; }
        } else if (m == PersonMatch::maybe) {
            maybes.push_back(t);
        }
    }
    if (same > 0) return st.link_topic(note_id, same) ? same : -1;
    const int64_t tid = st.upsert_topic("person", name);
    if (tid <= 0 || !st.link_topic(note_id, tid)) return -1;
    for (const auto &t : maybes) {
        const int64_t other = jint(t, "id", 0);
        if (other <= 0 || other == tid) continue;
        auto it = answers.find({name, other});
        if (it != answers.end() && it->second == 0) continue;  // a clear no: two people
        if (st.add_person_question(other, tid, st.person_context(other), context) <= 0) return -1;
    }
    return tid;
}

}  // namespace sve
