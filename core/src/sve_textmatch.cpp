#include <set>
#include "sve_textmatch.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "sve_medcheck.h"

namespace sve {
namespace {

// ---- nfc_clean ------------------------------------------------------------

bool decode_utf8(const std::string &s, size_t i, uint32_t &cp, size_t &len) {
    unsigned char c = (unsigned char)s[i];
    size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (n == 0 || i + n > s.size()) return false;
    uint32_t v = n == 1 ? c : c & (0xFF >> (n + 1));
    for (size_t k = 1; k < n; ++k) {
        unsigned char d = (unsigned char)s[i + k];
        if ((d & 0xC0) != 0x80) return false;
        v = (v << 6) | (d & 0x3F);
    }
    cp = v;
    len = n;
    return true;
}

void append_utf8(std::string &o, uint32_t cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
    } else {
        o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F));
        o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
    }
}

int telugu_ccc(uint32_t cp) {
    switch (cp) {
        case 0x0C3C: return 7;
        case 0x0C4D: return 9;
        case 0x0C55: return 84;
        case 0x0C56: return 91;
        default: return 0;
    }
}

// ---- number words ---------------------------------------------------------

enum NumKind { N_NONE, N_SMALL, N_HUNDRED, N_THOUSAND, N_LAKH };
struct Num { NumKind kind = N_NONE; int v = 0; };

struct Word { const char *w; NumKind kind; int v; };
const Word kNumWords[] = {
    {"zero", N_SMALL, 0}, {"six", N_SMALL, 6}, {"seven", N_SMALL, 7}, {"eight", N_SMALL, 8}, {"nine", N_SMALL, 9},
    {"thirteen", N_SMALL, 13}, {"fourteen", N_SMALL, 14}, {"fifteen", N_SMALL, 15}, {"sixteen", N_SMALL, 16},
    {"seventeen", N_SMALL, 17}, {"eighteen", N_SMALL, 18}, {"nineteen", N_SMALL, 19},
    {"twenty", N_SMALL, 20}, {"thirty", N_SMALL, 30}, {"forty", N_SMALL, 40}, {"fifty", N_SMALL, 50},
    {"sixty", N_SMALL, 60}, {"seventy", N_SMALL, 70}, {"eighty", N_SMALL, 80}, {"ninety", N_SMALL, 90},
    {"hundred", N_HUNDRED, 100}, {"thousand", N_THOUSAND, 1000}, {"lakh", N_LAKH, 100000}, {"lakhs", N_LAKH, 100000},
    // Telugu (med_tokens already turns ఒక, ఒకటి, రెండు, మూడు, నాలుగు, ఐదు into digits)
    {"సున్న", N_SMALL, 0}, {"సున్నా", N_SMALL, 0}, {"అయిదు", N_SMALL, 5},
    {"ఆరు", N_SMALL, 6}, {"ఏడు", N_SMALL, 7}, {"ఎనిమిది", N_SMALL, 8}, {"తొమ్మిది", N_SMALL, 9},
    {"పది", N_SMALL, 10}, {"పదకొండు", N_SMALL, 11}, {"పన్నెండు", N_SMALL, 12}, {"పదమూడు", N_SMALL, 13},
    {"పద్నాలుగు", N_SMALL, 14}, {"పధ్నాలుగు", N_SMALL, 14}, {"పదిహేను", N_SMALL, 15}, {"పదహారు", N_SMALL, 16},
    {"పదిహేడు", N_SMALL, 17}, {"పద్దెనిమిది", N_SMALL, 18}, {"పద్ధెనిమిది", N_SMALL, 18},
    {"పంతొమ్మిది", N_SMALL, 19}, {"పందొమ్మిది", N_SMALL, 19},
    {"ఇరవై", N_SMALL, 20}, {"ముప్పై", N_SMALL, 30}, {"నలభై", N_SMALL, 40}, {"యాభై", N_SMALL, 50},
    {"ఏభై", N_SMALL, 50}, {"అరవై", N_SMALL, 60}, {"డెబ్బై", N_SMALL, 70}, {"డెబ్భై", N_SMALL, 70},
    {"ఎనభై", N_SMALL, 80}, {"తొంభై", N_SMALL, 90},
    {"వంద", N_HUNDRED, 100}, {"నూట", N_HUNDRED, 100}, {"నూరు", N_HUNDRED, 100}, {"వందలు", N_HUNDRED, 100}, {"వందల", N_HUNDRED, 100},
    {"వెయ్యి", N_THOUSAND, 1000}, {"వేయి", N_THOUSAND, 1000}, {"వేలు", N_THOUSAND, 1000}, {"వేల", N_THOUSAND, 1000},
    {"లక్ష", N_LAKH, 100000}, {"లక్షలు", N_LAKH, 100000}, {"లక్షల", N_LAKH, 100000},
};

bool all_digits(const std::string &t) {
    if (t.empty()) return false;
    for (char c : t) if (c < '0' || c > '9') return false;
    return true;
}

Num num_of(const std::string &t) {
    Num n;
    if (all_digits(t)) {
        if (t.size() <= 2) { n.kind = N_SMALL; n.v = std::atoi(t.c_str()); }
        return n;  // three digits or more (Dolo 650) stand alone
    }
    if (t == "ten") { n.kind = N_SMALL; n.v = 10; return n; }
    if (t == "eleven") { n.kind = N_SMALL; n.v = 11; return n; }
    if (t == "twelve") { n.kind = N_SMALL; n.v = 12; return n; }
    for (auto &w : kNumWords)
        if (t == w.w) { n.kind = w.kind; n.v = w.v; return n; }
    return n;
}

// Builds one number from consecutive number tokens: "రెండు వందల యాభై" -> 250.
struct NumRun {
    long total = 0, cur = 0, last_big = 0;
    int last_small = -1, count = 0;
    std::string first;
    bool after_hundred = false;

    bool add(const std::string &tok, const Num &n) {
        if (n.kind == N_SMALL) {
            if (count > 0) {
                bool units_after_tens = last_small >= 20 && last_small % 10 == 0 && n.v >= 1 && n.v <= 9;
                if (last_small >= 0 && !units_after_tens) return false;
                if (n.v == 0) return false;
            }
            cur += n.v;
            last_small = n.v;
        } else if (n.kind == N_HUNDRED) {
            if (cur >= 100 || (cur == 0 && count > 0)) return false;
            cur = (cur ? cur : 1) * 100;
            last_small = -1;
            after_hundred = true;
        } else {  // thousand or lakh
            long m = n.v;
            if (last_big && m >= last_big) return false;
            if (cur == 0 && count > 0) return false;
            total += (cur ? cur : 1) * m;
            cur = 0;
            last_big = m;
            last_small = -1;
            after_hundred = false;
        }
        if (count++ == 0) first = tok;
        return true;
    }
    bool open() const { return count > 0; }
    bool and_allowed() const { return count > 0 && (after_hundred || last_big); }
    std::string emit() {
        std::string r = count == 1 && all_digits(first) ? first : std::to_string(total + cur);
        *this = NumRun();
        return r;
    }
};

// ---- words with a fixed canonical form ------------------------------------

struct Map { const char *w, *c; };

const Map kWeekdays[] = {
    {"monday", "wd1"}, {"mon", "wd1"}, {"tuesday", "wd2"}, {"tue", "wd2"}, {"tues", "wd2"},
    {"wednesday", "wd3"}, {"wed", "wd3"}, {"thursday", "wd4"}, {"thu", "wd4"}, {"thur", "wd4"}, {"thurs", "wd4"},
    {"friday", "wd5"}, {"fri", "wd5"}, {"saturday", "wd6"}, {"sat", "wd6"}, {"sunday", "wd7"}, {"sun", "wd7"},
    {"సోమవారం", "wd1"}, {"మంగళవారం", "wd2"}, {"బుధవారం", "wd3"}, {"గురువారం", "wd4"},
    {"శుక్రవారం", "wd5"}, {"శనివారం", "wd6"}, {"ఆదివారం", "wd7"},
};
const Map kMonths[] = {
    {"january", "mo1"}, {"jan", "mo1"}, {"february", "mo2"}, {"feb", "mo2"}, {"march", "mo3"}, {"april", "mo4"},
    {"apr", "mo4"}, {"may", "mo5"}, {"june", "mo6"}, {"jun", "mo6"}, {"july", "mo7"}, {"jul", "mo7"},
    {"august", "mo8"}, {"aug", "mo8"}, {"september", "mo9"}, {"sep", "mo9"}, {"sept", "mo9"},
    {"october", "mo10"}, {"oct", "mo10"}, {"november", "mo11"}, {"nov", "mo11"}, {"december", "mo12"}, {"dec", "mo12"},
    {"జనవరి", "mo1"}, {"ఫిబ్రవరి", "mo2"}, {"మార్చి", "mo3"}, {"ఏప్రిల్", "mo4"}, {"మే", "mo5"},
    {"జూన్", "mo6"}, {"జూలై", "mo7"}, {"జులై", "mo7"}, {"ఆగస్టు", "mo8"}, {"ఆగష్టు", "mo8"},
    {"సెప్టెంబర్", "mo9"}, {"సెప్టెంబరు", "mo9"}, {"అక్టోబర్", "mo10"}, {"అక్టోబరు", "mo10"},
    {"నవంబర్", "mo11"}, {"నవంబరు", "mo11"}, {"డిసెంబర్", "mo12"}, {"డిసెంబరు", "mo12"},
};
const Map kRelative1[] = {
    {"ఈరోజు", "rd0"}, {"నేడు", "rd0"}, {"today", "rd0"}, {"రేపు", "rd1"}, {"tomorrow", "rd1"},
    {"ఎల్లుండి", "rd2"}, {"నిన్న", "rd-1"}, {"yesterday", "rd-1"}, {"మొన్న", "rd-2"},
};
struct Phrase { std::vector<const char *> w; const char *c; };
const Phrase kPhrases[] = {
    {{"day", "after", "tomorrow"}, "rd2"},
    {{"ఈ", "రోజు"}, "rd0"},
    {{"వచ్చే", "వారం"}, "rw1"}, {{"next", "week"}, "rw1"},
    {{"ఈ", "వారం"}, "rw0"}, {{"this", "week"}, "rw0"},
    {{"వచ్చే", "నెల"}, "rm1"}, {{"next", "month"}, "rm1"},
};

template <size_t N>
const char *lookup(const Map (&m)[N], const std::string &t) {
    for (auto &e : m) if (t == e.w) return e.c;
    return nullptr;
}

// English words that are also ordinary words; they map only next to a date cue.
bool ambiguous_english(const std::string &t) {
    static const char *k[] = {"may", "mar", "march", "sat", "sun", "mon", "wed", "sep", "dec"};
    for (auto *w : k) if (t == w) return true;
    return false;
}

bool is_date_cue(const std::string &t, bool before) {
    if (num_of(t).kind != N_NONE || (!t.empty() && t[0] >= '0' && t[0] <= '9')) return true;
    if (lookup(kWeekdays, t) || lookup(kMonths, t)) return true;
    return before && (t == "on" || t == "next" || t == "this" || t == "last" || t == "వచ్చే");
}

bool is_fact(const std::string &t) {
    if (all_digits(t.substr(0, 1)) && !t.empty()) return true;
    auto num_after = [&](size_t k, bool neg) {
        size_t i = k;
        if (neg && i < t.size() && t[i] == '-') ++i;
        return i < t.size() && all_digits(t.substr(i));
    };
    if (t.size() > 2) {
        std::string p = t.substr(0, 2);
        if (p == "wd" || p == "mo") return num_after(2, false);
        if (p == "rd") return num_after(2, true);
        if (p == "rw" || p == "rm") return num_after(2, false);
    }
    return false;
}

// ---- dates ----------------------------------------------------------------

int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {  // days since 1970-01-01
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

void civil_from_days(int64_t z, int64_t &y, unsigned &m, unsigned &d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
}

bool leap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

bool valid_day(int64_t y, unsigned m, unsigned d) {
    static const unsigned dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12 || d < 1) return false;
    return d <= dim[m - 1] + (m == 2 && leap(y) ? 1u : 0u);
}

bool parse_ymd(const std::string &s, int64_t &y, unsigned &m, unsigned &d) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    for (size_t i : {0, 1, 2, 3, 5, 6, 8, 9}) if (s[i] < '0' || s[i] > '9') return false;
    y = std::atoi(s.substr(0, 4).c_str());
    m = (unsigned)std::atoi(s.substr(5, 2).c_str());
    d = (unsigned)std::atoi(s.substr(8, 2).c_str());
    return valid_day(y, m, d);
}

std::string format_ymd(int64_t days) {
    int64_t y; unsigned m, d;
    civil_from_days(days, y, m, d);
    char b[16];
    std::snprintf(b, sizeof b, "%04d-%02u-%02u", (int)y, m, d);
    return b;
}

int weekday_mon1(int64_t days) { return (int)(((days % 7) + 7 + 3) % 7) + 1; }  // Monday = 1; 1970-01-01 was a Thursday

bool has_prefix(const std::string &t, const char *p) { return t.compare(0, std::strlen(p), p) == 0; }

bool is_time_marker(const std::string &t) {
    return t == "am" || t == "pm" || t == "o" || t == "గంటకు" || t == "గంటలకు" || t == "గంట" || t == "గంటల";
}

}  // namespace

std::string nfc_clean(const std::string &s) {
    std::vector<uint32_t> cps;       // code points; invalid bytes kept as 0x110000 + byte
    cps.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        uint32_t cp; size_t len;
        if (decode_utf8(s, i, cp, len)) { i += len; if (cp != 0x200C && cp != 0x200D) cps.push_back(cp); }
        else { cps.push_back(0x110000 + (unsigned char)s[i]); ++i; }
    }
    // Canonical order of each run of Telugu combining marks, then U+0C46 + U+0C56 -> U+0C48.
    for (size_t i = 0; i < cps.size();) {
        if (telugu_ccc(cps[i]) == 0) { ++i; continue; }
        size_t j = i;
        while (j < cps.size() && telugu_ccc(cps[j]) != 0) ++j;
        std::stable_sort(cps.begin() + i, cps.begin() + j, [](uint32_t a, uint32_t b) { return telugu_ccc(a) < telugu_ccc(b); });
        if (i > 0 && cps[i - 1] == 0x0C46 && cps[j - 1] == 0x0C56) {
            cps[i - 1] = 0x0C48;
            cps.erase(cps.begin() + (j - 1));
            --j;
        }
        i = j;
    }
    std::string o;
    o.reserve(s.size());
    for (uint32_t cp : cps) {
        if (cp >= 0x110000) o += (char)(cp - 0x110000);
        else append_utf8(o, cp);
    }
    return o;
}

std::vector<std::string> match_tokens(const std::string &s) {
    std::vector<std::string> t = med_tokens(nfc_clean(s)), out;
    NumRun run;
    auto flush = [&] { if (run.open()) out.push_back(run.emit()); };
    for (size_t i = 0; i < t.size(); ++i) {
        // English "one" as a pronoun ("no one", "any one"): not the number.
        if (t[i] == "1" && i > 0) {
            static const std::set<std::string> kPron = {"no", "any", "every", "some", "each", "which", "this", "that"};
            if (kPron.count(t[i - 1])) { flush(); out.push_back("one"); continue; }
        }
        Num n = num_of(t[i]);
        // A Telugu number word with a case ending ("తొమ్మిదిలో", "ఐదులోకి", "మూడుకి"): the number, and the end of it.
        if (n.kind == N_NONE) {
            static const std::vector<std::string> kEnds = {"లోకి", "లోని", "లోను", "లో", "కి", "కు", "ని", "ను", "తో", "గా"};
            static const std::vector<std::pair<std::string, int>> kSmall = {
                {"ఒకటి", 1}, {"ఒక", 1}, {"రెండు", 2}, {"మూడు", 3}, {"నాలుగు", 4}, {"ఐదు", 5}};
            bool taken = false;
            for (const auto &e : kEnds) {
                if (taken) break;
                if (t[i].size() <= e.size() || t[i].compare(t[i].size() - e.size(), e.size(), e) != 0) continue;
                const std::string stem = t[i].substr(0, t[i].size() - e.size());
                Num sn = num_of(stem);
                for (const auto &k : kSmall) if (sn.kind == N_NONE && stem == k.first) { sn.kind = N_SMALL; sn.v = k.second; }
                if (sn.kind == N_NONE) continue;
                if (!run.add(stem, sn)) { flush(); run.add(stem, sn); }
                flush();
                taken = true;
            }
            if (taken) continue;
        }
        if (n.kind != N_NONE) {
            if (run.add(t[i], n)) continue;
            flush();
            if (run.add(t[i], n)) continue;
        }
        if (t[i] == "and" && run.and_allowed() && i + 1 < t.size() && num_of(t[i + 1]).kind != N_NONE) continue;
        flush();
        bool done = false;
        for (auto &p : kPhrases) {
            if (i + p.w.size() > t.size()) continue;
            size_t k = 0;
            while (k < p.w.size() && t[i + k] == p.w[k]) ++k;
            if (k == p.w.size()) { out.push_back(p.c); i += p.w.size() - 1; done = true; break; }
        }
        if (done) continue;
        bool plain = ambiguous_english(t[i]) &&
                     !((i > 0 && is_date_cue(t[i - 1], true)) || (i + 1 < t.size() && is_date_cue(t[i + 1], false)));
        if (plain) out.push_back(t[i]);
        else if (const char *c = lookup(kWeekdays, t[i])) out.push_back(c);
        else if (const char *c2 = lookup(kMonths, t[i])) out.push_back(c2);
        else if (const char *c3 = lookup(kRelative1, t[i])) out.push_back(c3);
        else out.push_back(t[i]);
    }
    flush();
    return out;
}

std::vector<std::string> canonical_facts(const std::string &text) {
    std::vector<std::string> out;
    for (auto &t : match_tokens(text)) if (is_fact(t)) out.push_back(t);
    return out;
}

const std::vector<std::string> kTeluguCaseEndings = {
    "కి", "కు", "ని", "ను", "తో", "గారు", "గారికి", "గారితో", "గారిని", "గారి", "లో", "ది", "దే", "కే", "నుంచి", "తోటి",
};

bool name_in(const std::string &name, const std::string &text) {
    std::string n = nfc_clean(name), x = nfc_clean(text);
    if (name_ok(n, x)) return true;
    std::vector<std::string> nv = med_tokens(n, false), q = med_tokens(x, false);
    if (nv.empty()) return false;
    const std::string last = nv.back();
    nv.pop_back();
    for (size_t i = nv.size(); i < q.size(); ++i) {
        const std::string &w = q[i];
        if (w.size() <= last.size() || !has_prefix(w, last.c_str())) continue;
        const std::string rest = w.substr(last.size());
        if (std::find(kTeluguCaseEndings.begin(), kTeluguCaseEndings.end(), rest) == kTeluguCaseEndings.end()) continue;
        if (std::equal(nv.begin(), nv.end(), q.begin() + (i - nv.size()))) return true;
    }
    return false;
}

std::string resolve_spoken_date(const std::string &words, const std::string &said_ymd) {
    int64_t y; unsigned m, d;
    if (!parse_ymd(said_ymd, y, m, d)) return "";
    const int64_t said = days_from_civil(y, m, d);
    const std::vector<std::string> t = match_tokens(words);
    std::vector<int64_t> found;  // every date the words name; more than one distinct date is no answer
    std::vector<bool> used(t.size(), false);

    auto num_at = [&](size_t i) -> long { return i < t.size() && all_digits(t[i]) && t[i].size() <= 4 ? std::atol(t[i].c_str()) : -1; };

    for (size_t i = 0; i < t.size(); ++i) {
        const std::string &w = t[i];
        if (has_prefix(w, "rd") && w.size() > 2 && w != "rd") {
            found.push_back(said + std::atol(w.c_str() + 2));
        } else if (has_prefix(w, "wd") && w.size() == 3) {
            int wd = w[2] - '0';
            bool next = i > 0 && (t[i - 1] == "next" || t[i - 1] == "వచ్చే");
            if (next) {
                int64_t monday = said - (weekday_mon1(said) - 1);
                found.push_back(monday + 7 + (wd - 1));
            } else {
                int diff = (wd - weekday_mon1(said) + 7) % 7;
                found.push_back(said + (diff == 0 ? 7 : diff));
            }
        } else if (num_at(i) >= 0 && i + 1 < t.size() && !used[i]) {
            const std::string &u = t[i + 1];
            long n = num_at(i);
            bool in_en = i > 0 && t[i - 1] == "in";
            if ((in_en && (u == "day" || u == "days")) || u == "రోజుల్లో" || u == "రోజులలో") found.push_back(said + n);
            else if ((in_en && (u == "week" || u == "weeks")) || u == "వారాల్లో" || u == "వారాలలో") found.push_back(said + 7 * n);
        }
    }
    for (size_t i = 0; i < t.size(); ++i) {
        if (!has_prefix(t[i], "mo") || t[i].size() < 3 || !all_digits(t[i].substr(2))) continue;
        unsigned mon = (unsigned)std::atoi(t[i].c_str() + 2);
        long day = -1;
        if (i > 0 && num_at(i - 1) >= 0 && t[i - 1].size() <= 2) day = num_at(i - 1);
        else if (i + 1 < t.size() && num_at(i + 1) >= 0 && t[i + 1].size() <= 2 &&
                 !(i + 2 < t.size() && is_time_marker(t[i + 2])))
            day = num_at(i + 1);
        if (day < 0) continue;
        int64_t yy = y;
        if (!valid_day(yy, mon, (unsigned)day)) { found.push_back(INT64_MIN); continue; }  // not a real date: no answer
        if (days_from_civil(yy, mon, (unsigned)day) < said) ++yy;
        if (!valid_day(yy, mon, (unsigned)day)) { found.push_back(INT64_MIN); continue; }
        found.push_back(days_from_civil(yy, mon, (unsigned)day));
    }
    if (found.empty()) return "";
    for (int64_t f : found) if (f == INT64_MIN || f != found[0]) return "";
    return format_ymd(found[0]);
}

}  // namespace sve
