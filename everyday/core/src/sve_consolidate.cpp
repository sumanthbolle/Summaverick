#include "sve_skills.h"
#include "sve_consolidate.h"
#include "sve_extract.h"

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <map>
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

// One reading of a name: its words in the shared Latin form and its honorifics.
struct NameForm {
    std::vector<std::string> words;
    std::set<std::string> honorifics;
};

struct NameParts {
    NameForm full;              // the words as written (honorific words dropped)
    NameForm stripped;          // the last Telugu word without its case ending; equals full when it has none
    bool has_strip = false;
    bool ambiguous = false;     // the ending (ని, ను, ది, దే, కే) may be part of the name: రజని is not రజ + ని
    bool honorific_only = false;  // nothing but honorifics ("Sir", "సార్")
    std::set<std::string> only;   // those honorifics, canonical (సార్ and Sir are both "sir")
};

// Endings that also end ordinary names; a match that needs one removed is never "same". కి is clear only after a
// stem ending in ి ీ ె ే (రవికి, వాణికి); after anything else it may be the name's own (వాసుకి, జానకి).
bool ambiguous_ending(const std::string &e, const std::string &stem) {
    if (e == "ని" || e == "ను" || e == "ది" || e == "దే" || e == "కే") return true;
    if (e == "కి") {
        uint32_t last = 0;
        for (size_t i = 0; i < stem.size();) next_cp(stem, i, last);
        return !(last == 0x0C3F || last == 0x0C40 || last == 0x0C46 || last == 0x0C47);
    }
    return false;
}

std::vector<std::string> latin_words(const std::vector<std::string> &raw) {
    std::vector<std::string> out;
    for (const auto &t : raw) {
        std::string k = transliterate_te(t);
        if (!k.empty()) out.push_back(k);
    }
    return out;
}

NameParts name_parts(const std::string &name) {
    NameParts p;
    std::vector<std::string> raw = med_tokens(nfc_clean(name), false), kept;
    std::set<std::string> hon;
    for (const auto &t : raw) {
        if (const char *h = honorific(t)) hon.insert(h);
        else kept.push_back(t);
    }
    if (kept.empty()) {  // all honorific ("Sir"): the words are the name, but it names nobody in particular
        kept = raw;
        p.only = hon;
        hon.clear();
        p.honorific_only = !raw.empty();
    }
    p.full = {latin_words(kept), hon};
    p.stripped = p.full;
    if (!kept.empty() && has_telugu(kept.back())) {
        // The last word's case ending (longest first); a short name that only looks like one keeps it.
        static const std::vector<std::string> endings = [] {
            std::vector<std::string> e = kTeluguCaseEndings;
            std::stable_sort(e.begin(), e.end(), [](const std::string &a, const std::string &b) { return a.size() > b.size(); });
            return e;
        }();
        const std::string &w = kept.back();
        for (const auto &e : endings) {
            if (w.size() <= e.size() || w.compare(w.size() - e.size(), e.size(), e) != 0) continue;
            const std::string stem = w.substr(0, w.size() - e.size());
            if (telugu_syllables(stem) < 2) continue;
            std::vector<std::string> sk = kept;
            sk.back() = stem;
            p.stripped = {latin_words(sk), hon};
            if (const char *h = honorific(e)) p.stripped.honorifics.insert(h);
            p.has_strip = true;
            p.ambiguous = ambiguous_ending(e, stem);
            break;
        }
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
    // The nasal fold can make a new double ("Ramna" -> "ranna"): collapse once more.
    s.swap(o);
    o.clear();
    for (char c : s)
        if (o.empty() || o.back() != c || !letter(c)) o += c;
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

std::string person_key(const std::string &name) {
    const NameParts p = name_parts(name);
    return join(p.has_strip && !p.ambiguous ? p.stripped.words : p.full.words);
}

PersonMatch match_person(const std::string &a, const std::string &b) {
    const NameParts pa = name_parts(a), pb = name_parts(b);
    if (pa.full.words.empty() || pb.full.words.empty()) return PersonMatch::different;
    // Every reading of each name: as written, and without a case ending (flagged when that ending is ambiguous).
    struct Reading { const NameForm *f; bool risky; };
    std::vector<Reading> ra = {{&pa.full, false}}, rb = {{&pb.full, false}};
    if (pa.has_strip) ra.push_back({&pa.stripped, pa.ambiguous});
    if (pb.has_strip) rb.push_back({&pb.stripped, pb.ambiguous});
    // A name of honorifics only ("Sir") may be anyone: at most a question.
    const bool cap = pa.honorific_only || pb.honorific_only;
    bool maybe = false;
    if (pa.honorific_only && pb.honorific_only)
        for (const auto &h : pa.only) if (pb.only.count(h)) maybe = true;
    if (raw_norm(a) == raw_norm(b)) {
        if (!cap) return PersonMatch::same;
        maybe = true;
    }
    for (const auto &x : ra)
        for (const auto &y : rb) {
            const std::string kx = join(x.f->words), ky = join(y.f->words);
            if (kx.empty() || ky.empty()) continue;
            if (kx == ky && x.f->honorifics == y.f->honorifics) {
                if (!cap && !x.risky && !y.risky) return PersonMatch::same;
                maybe = true;
            }
            if (subset(x.f->words, y.f->words) || subset(y.f->words, x.f->words)) maybe = true;
            else if (edit_distance(kx, ky) <= 1 && std::min(letters(kx), letters(ky)) >= 4) maybe = true;
        }
    if (maybe) return PersonMatch::maybe;
    return PersonMatch::different;
}

const std::string kSamePersonSystem = skills::text(skills::Skill::same_person);

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
                       const json &conf, int64_t now, const std::vector<std::pair<int64_t, std::string>> &links) {
    json row = st.memory_item(older);
    if (!row.is_object()) return false;
    std::optional<double> c;
    if (conf.is_number()) c = conf.get<double>();
    if (!st.add_memory_source(older, note_id, start_sec, quote, c)) return false;
    for (const auto &l : links)  // the same link twice is not an error (link_memory_entity ignores it)
        if (l.first > 0 && !st.link_memory_entity(older, l.first, l.second)) return false;
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

int64_t known_person(Store &st, int64_t note_id, const std::string &raw) {
    const std::string name = trim_copy(raw);
    if (!eligible_name(name) || !looks_like_person_name(name)) return 0;
    for (const auto &t : st.topics("person"))
        if (match_person(name, jstr(t, "name")) == PersonMatch::same) {
            const int64_t id = jint(t, "id", 0);
            return id > 0 && st.link_topic(note_id, id) ? id : -1;
        }
    return 0;
}

int64_t resolve_person(Store &st, int64_t note_id, const std::string &raw, const std::string &self_lower,
                       const std::string &context, const PersonAnswers &answers) {
    const std::string name = trim_copy(raw);
    if (!eligible_name(name) || !looks_like_person_name(name) || (!self_lower.empty() && ascii_lower(name) == self_lower))
        return 0;  // a company, an organisation or a description is not a person
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

// ---------------------------------------------------------------- relations

bool names_me(const std::string &who, const std::string &self_lower) {
    static const std::set<std::string> kMe = {"i", "me", "myself", "we", "us", "నేను", "నా", "మేము", "మనం", "నన్ను"};
    const std::string w = ascii_lower(nfc_clean(trim_copy(who)));
    if (w.empty()) return false;
    return kMe.count(w) > 0 || (!self_lower.empty() && w == self_lower);
}

namespace {

std::string self_lower_of(Store &st) {
    auto p = st.setting_get("profile");
    json j = p ? json::parse(*p, nullptr, false) : json();
    return j.is_object() ? ascii_lower(trim_copy(jstr(j, "name"))) : "";
}

// One side of a relation: a known person, the owner, or nobody named.
struct Party {
    std::string kind = "person";  // person | me
    int64_t topic = 0;
    std::string name;
    bool known() const { return kind == "me" || topic > 0; }
    json topic_j() const { return topic > 0 ? json(topic) : json(nullptr); }
};

Party party_of(const json &m, const char *role, const std::string &self_lower) {
    Party p;
    for (const auto &e : jarr(m, "people"))
        if (jstr(e, "role") == role && jint(e, "id", 0) > 0) {
            p.topic = jint(e, "id", 0);
            p.name = jstr(e, "name");
            return p;
        }
    const std::string said = trim_copy(jstr(m, role));
    if (names_me(said, self_lower)) { p.kind = "me"; p.name = said; }
    return p;
}

std::string relation_key(const json &r) {
    auto n = [&](const char *k) { return std::to_string(jint(r, k, 0)); };
    return jstr(r, "relation") + "|" + jstr(r, "from_kind") + "|" + n("from_topic") + "|" + jstr(r, "to_kind") + "|" +
           n("to_topic") + "|" + n("memory_id");
}

}  // namespace

json derive_relations(int64_t note_id, const json &memories, const json &note_people, const std::string &self_lower) {
    json out = json::array();
    std::set<std::string> seen;
    auto add = [&](json r) {
        r["note_id"] = note_id;
        if (seen.insert(relation_key(r)).second) out.push_back(std::move(r));
    };
    // Everyone the recording names: its people first, in the order linked, then the memories' people.
    std::vector<std::pair<int64_t, std::string>> involved;
    std::set<int64_t> have;
    auto involve = [&](int64_t tid, const std::string &name) {
        if (tid > 0 && have.insert(tid).second) involved.push_back({tid, name});
    };
    if (note_people.is_array())
        for (const auto &p : note_people) involve(jint(p, "id", 0), jstr(p, "name"));

    json lines = json::array();
    if (memories.is_array())
        for (const auto &m : memories) {
            if (!m.is_object() || jstr(m, "review") == "wrong" || jstr(m, "origin") == "medicine") continue;
            if (m.contains("superseded_by") && !m["superseded_by"].is_null()) continue;
            for (const auto &e : jarr(m, "people")) involve(jint(e, "id", 0), jstr(e, "name"));
            const std::string type = jstr(m, "type");
            const bool done = m.contains("done") && ((m["done"].is_boolean() && m["done"].get<bool>()) ||
                                                     (m["done"].is_number_integer() && m["done"].get<int64_t>() != 0));
            if (type != "decision" && !(type == "question" && !done) && type != "promise") continue;
            lines.push_back(m);
        }
    for (const auto &p : involved)
        add({{"relation", "involved_in"}, {"from_kind", "person"}, {"from_topic", p.first}, {"from_name", p.second},
             {"to_kind", "note"}, {"to_topic", nullptr}, {"to_text", ""}, {"memory_id", nullptr}, {"quote", nullptr},
             {"start_sec", nullptr}});

    for (const auto &m : lines) {
        const std::string type = jstr(m, "type"), text = jstr(m, "text");
        const int64_t mid = jint(m, "id", 0);
        json quote = nullptr, start = nullptr;
        for (const auto &s : jarr(m, "sources"))
            if (jint(s, "note_id", -1) == note_id) { quote = jstr(s, "quote"); start = jint(s, "start_sec", 0); break; }
        auto base = [&](const char *relation) {
            return json{{"relation", relation}, {"memory_id", mid}, {"quote", quote}, {"start_sec", start}};
        };
        const Party who = party_of(m, "who", self_lower), to = party_of(m, "to_whom", self_lower);
        if (type == "decision" || type == "question") {
            json r = base(type == "decision" ? "decided" : "asked");
            r["from_kind"] = who.kind; r["from_topic"] = who.topic_j(); r["from_name"] = who.name;
            r["to_kind"] = type; r["to_topic"] = nullptr; r["to_text"] = text;
            add(r);
            for (const auto &t : jarr(m, "topics")) {
                if (jint(t, "id", 0) <= 0) continue;
                json a = base("about");
                a["from_kind"] = type; a["from_topic"] = nullptr; a["from_name"] = text;
                a["to_kind"] = "topic"; a["to_topic"] = jint(t, "id", 0); a["to_text"] = jstr(t, "name");
                add(a);
            }
        }
        // Someone said it to someone: asked (a question) or told.
        if (who.known() && to.known() && !(who.kind == to.kind && who.topic == to.topic)) {
            json r = base(type == "question" ? "asked" : "told");
            r["from_kind"] = who.kind; r["from_topic"] = who.topic_j(); r["from_name"] = who.name;
            r["to_kind"] = to.kind; r["to_topic"] = to.topic_j(); r["to_text"] = to.name;
            add(r);
        }
    }
    return out;
}

int refresh_relations(Store &st, int64_t note_id) {
    const json mems = st.memory_items_with_topics({{"note_id", note_id}});
    const json rows = derive_relations(note_id, mems, st.note_topics(note_id, "person"), self_lower_of(st));
    std::set<std::string> decided;
    for (const auto &r : st.relations({{"note_id", note_id}}))
        if (jstr(r, "status") != "pending") decided.insert(relation_key(r));
    if (st.delete_relations(note_id, "pending") < 0) return -1;
    int n = 0;
    for (const auto &r : rows) {
        if (decided.count(relation_key(r))) continue;
        if (st.add_relation(r) <= 0) return -1;
        ++n;
    }
    return n;
}

json relation_reviews(Store &st) {
    json out = json::array();
    for (const auto &n : st.relation_review_notes()) {
        const int64_t note_id = jint(n, "note_id", 0);
        json people = json::array(), decisions = json::array(), questions = json::array(), between = json::array();
        // The checking agent's flags on the memories these lines come from (sve_verify_note's report): why the line
        // needs a look (meaning | who | consistency) and where the recording says it.
        std::map<int64_t, json> flags;
        if (auto rep = st.setting_get("verify." + std::to_string(note_id))) {
            const json j = json::parse(*rep, nullptr, false);
            if (j.is_object() && j.contains("relation_flags") && j["relation_flags"].is_array())
                for (const auto &f : j["relation_flags"]) flags[jint(f, "memory_id", 0)] = f;
        }
        for (const auto &r : st.relations({{"note_id", note_id}, {"status", "pending"}})) {
            const std::string rel = jstr(r, "relation"), to_kind = jstr(r, "to_kind");
            if (rel == "involved_in") {
                if (jstr(r, "from_kind") == "person" && jint(r, "from_topic", 0) > 0)
                    people.push_back({{"id", r["id"]}, {"topic_id", r["from_topic"]}, {"name", r["from_name"]}});
            } else if ((rel == "decided" && to_kind == "decision") || (rel == "asked" && to_kind == "question")) {
                const std::string fk = jstr(r, "from_kind");
                json line = {{"id", r["id"]}, {"memory_id", r["memory_id"]}, {"text", r["to_text"]},
                             {"who", fk == "me" ? "" : jstr(r, "from_name")},
                             {"who_kind", fk == "me" ? "me" : (jint(r, "from_topic", 0) > 0 ? "person" : "nobody")},
                             {"quote", r["quote"]}, {"start_sec", r["start_sec"]}};
                auto fl = flags.find(jint(r, "memory_id", 0));
                if (fl != flags.end()) {
                    line["check"] = jstr(fl->second, "check");
                    line["check_sec"] = fl->second.contains("start_sec") ? fl->second["start_sec"] : json(nullptr);
                }
                (rel == "decided" ? decisions : questions).push_back(line);
            } else if (rel == "told" || rel == "asked") {
                between.push_back({{"id", r["id"]}, {"relation", rel},
                                   {"from", jstr(r, "from_kind") == "me" ? "me" : jstr(r, "from_name")},
                                   {"to", to_kind == "me" ? "me" : jstr(r, "to_text")}});
            }
        }
        if (people.empty() && decisions.empty() && questions.empty()) continue;
        out.push_back({{"note_id", note_id}, {"title", n["title"]}, {"created_at", n["created_at"]}, {"people", people},
                       {"decisions", decisions}, {"questions", questions}, {"between", between}});
    }
    return out;
}

namespace {

bool accept_body(Store &st, int64_t note_id, const json &edits) {
    const std::string self = self_lower_of(st);
    std::map<int64_t, json> edit;
    if (edits.is_array())
        for (const auto &e : edits)
            if (e.is_object() && jint(e, "id", 0) > 0) edit[jint(e, "id", 0)] = e;
    auto removed = [&](int64_t id) {
        auto it = edit.find(id);
        return it != edit.end() && it->second.value("removed", false) == true;
    };
    auto field = [&](int64_t id, const char *k, std::string &out) {
        auto it = edit.find(id);
        if (it == edit.end() || !it->second.contains(k) || !it->second[k].is_string()) return false;
        out = trim_copy(it->second[k].get<std::string>());
        return true;
    };
    json rows = st.relations({{"note_id", note_id}, {"status", "pending"}});
    if (rows.empty()) return false;
    std::set<int64_t> gone_people, gone_memories;

    // People: removed, or renamed (resolved like a spoken name; this note's links move to them).
    for (const auto &r : rows) {
        const int64_t id = jint(r, "id", 0), tid = jint(r, "from_topic", 0);
        if (jstr(r, "relation") != "involved_in" || tid <= 0) continue;
        if (removed(id)) { gone_people.insert(tid); continue; }
        std::string name;
        if (!field(id, "name", name) || name.empty() || name == jstr(r, "from_name")) continue;
        int64_t into = 0;
        if (!names_me(name, self)) {
            into = resolve_person(st, note_id, name, self, "", {});
            if (into < 0) return false;
            if (into == 0) continue;  // not a name (a pronoun): kept as it was
        }
        if (!st.move_note_person(note_id, tid, into, jstr(r, "from_name"), name)) return false;
    }

    // Decision and question lines: removed, who changed, words corrected.
    rows = st.relations({{"note_id", note_id}, {"status", "pending"}});
    for (const auto &r : rows) {
        const int64_t id = jint(r, "id", 0), mid = jint(r, "memory_id", 0);
        const std::string rel = jstr(r, "relation"), to_kind = jstr(r, "to_kind");
        if (mid <= 0 || !((rel == "decided" && to_kind == "decision") || (rel == "asked" && to_kind == "question"))) continue;
        if (removed(id)) { gone_memories.insert(mid); continue; }
        std::string who;
        const std::string now_who = jstr(r, "from_kind") == "me" ? "me" : jstr(r, "from_name");
        if (field(id, "who", who) && who != now_who) {
            Party p;
            p.name = who;
            if (names_me(who, self)) p.kind = "me";
            else if (!who.empty()) {
                p.topic = resolve_person(st, note_id, who, self, jstr(r, "to_text"), {});
                if (p.topic < 0) return false;
                if (p.topic == 0) p.name = "";
            }
            if (!st.set_memory_who(mid, p.topic, p.name)) return false;
            for (const auto &o : rows)
                if (jint(o, "memory_id", 0) == mid && (jstr(o, "from_kind") == "person" || jstr(o, "from_kind") == "me") &&
                    !st.update_relation(jint(o, "id", 0), {{"from_kind", p.kind}, {"from_topic", p.topic_j()}, {"from_name", p.name}}))
                    return false;
        }
        std::string text;
        if (field(id, "text", text) && !text.empty() && text != jstr(r, "to_text")) {
            if (!st.update_memory_item(mid, {{"text", text}, {"review", "corrected"}, {"queue", 0}})) return false;
            for (const auto &o : rows) {
                if (jint(o, "memory_id", 0) != mid) continue;
                const std::string orel = jstr(o, "relation");
                json f = orel == "about" ? json{{"from_name", text}} : json{{"to_text", text}};
                if ((orel == "about" || jstr(o, "to_kind") == to_kind) && !st.update_relation(jint(o, "id", 0), f)) return false;
            }
        }
    }

    // Everything left is accepted, apart from what the person took out.
    const int64_t now = (int64_t)time(nullptr);
    for (const auto &r : st.relations({{"note_id", note_id}, {"status", "pending"}})) {
        const int64_t id = jint(r, "id", 0);
        const std::string rel = jstr(r, "relation"), to_kind = jstr(r, "to_kind");
        const bool line = (rel == "decided" && to_kind == "decision") || (rel == "asked" && to_kind == "question");
        json f = {{"status", "accepted"}, {"decided_at", now}};
        if (gone_memories.count(jint(r, "memory_id", 0))) f["status"] = "rejected";
        else if (gone_people.count(jint(r, "from_topic", 0)) || gone_people.count(jint(r, "to_topic", 0))) {
            // A decision or question stays, with nobody named for it; the person's other relations go.
            if (line) { f["from_kind"] = "person"; f["from_topic"] = nullptr; f["from_name"] = ""; }
            else f["status"] = "rejected";
        }
        auto it = edit.find(id);
        if (it != edit.end()) { f["edited"] = 1; f["edits"] = it->second; }
        if (!st.update_relation(id, f)) return false;
    }
    return true;
}

}  // namespace

bool accept_relations(Store &st, int64_t note_id, const json &edits) {
    if (!st.begin()) return false;
    bool ok = false;
    try {
        ok = accept_body(st, note_id, edits);
    } catch (...) {
        st.rollback();
        throw;
    }
    if (!ok) { st.rollback(); return false; }
    return st.commit();
}

bool dismiss_relations(Store &st, int64_t note_id) {
    json rows = st.relations({{"note_id", note_id}, {"status", "pending"}});
    if (rows.empty() || !st.begin()) return false;
    const int64_t now = (int64_t)time(nullptr);
    for (const auto &r : rows)
        if (!st.update_relation(jint(r, "id", 0), {{"status", "rejected"}, {"decided_at", now}})) { st.rollback(); return false; }
    return st.commit();
}

}  // namespace sve
