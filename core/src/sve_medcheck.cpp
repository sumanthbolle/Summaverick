#include "sve_medcheck.h"

#include <algorithm>
#include <cstring>
#include <set>

namespace sve {
namespace {

enum Cls { SEP, DIGIT, ALPHA };

Cls classify(unsigned char c) {
    if (c >= '0' && c <= '9') return DIGIT;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80) return ALPHA;
    return SEP;
}

// Telugu digits U+0C66..U+0C6F are E0 B1 A6..AF in UTF-8.
std::string map_telugu_digits(const std::string &s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0xE0 && i + 2 < s.size() && (unsigned char)s[i + 1] == 0xB1) {
            unsigned char d = (unsigned char)s[i + 2];
            if (d >= 0xA6 && d <= 0xAF) { o += (char)('0' + (d - 0xA6)); i += 2; continue; }
        }
        o += s[i];
    }
    return o;
}

const char *map_word(const std::string &t) {
    static const struct { const char *w, *d; } kMap[] = {
        {"one", "1"}, {"two", "2"}, {"three", "3"}, {"four", "4"}, {"five", "5"}, {"six", "6"},
        {"seven", "7"}, {"eight", "8"}, {"nine", "9"}, {"ten", "10"}, {"eleven", "11"}, {"twelve", "12"},
        {"\xE0\xB0\x92\xE0\xB0\x95\xE0\xB0\x9F\xE0\xB0\xBF", "1"},                  // ఒకటి
        {"\xE0\xB0\x92\xE0\xB0\x95", "1"},                                          // ఒక
        {"\xE0\xB0\xB0\xE0\xB1\x86\xE0\xB0\x82\xE0\xB0\xA1\xE0\xB1\x81", "2"},      // రెండు
        {"\xE0\xB0\xAE\xE0\xB1\x82\xE0\xB0\xA1\xE0\xB1\x81", "3"},                  // మూడు
        {"\xE0\xB0\xA8\xE0\xB0\xBE\xE0\xB0\xB2\xE0\xB1\x81\xE0\xB0\x97\xE0\xB1\x81", "4"},  // నాలుగు
        {"\xE0\xB0\x90\xE0\xB0\xA6\xE0\xB1\x81", "5"},                              // ఐదు
    };
    for (auto &e : kMap)
        if (t == e.w) return e.d;
    return nullptr;
}

std::vector<std::string> tokens(const std::string &raw, bool split = true) {
    std::string s = map_telugu_digits(raw);
    std::vector<std::string> out;
    std::string cur;
    Cls curc = SEP;
    auto flush = [&] {
        if (cur.empty()) return;
        if (const char *d = map_word(cur)) cur = d;
        out.push_back(cur);
        cur.clear();
    };
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        Cls k = classify(c);
        bool next_digit = i + 1 < s.size() && classify((unsigned char)s[i + 1]) == DIGIT;
        if (k == SEP && next_digit && curc == DIGIT && !cur.empty() && c == '.') {
            cur += '.';  // decimal point
            continue;
        }
        if (k == SEP && curc == DIGIT && !cur.empty()) {
            // fraction, range or schedule, with optional spaces: "1 / 2" -> "1/2"
            size_t j = i;
            while (j < s.size() && (s[j] == ' ' || s[j] == '\t' || s[j] == '\n' || s[j] == '\r')) ++j;
            if (j < s.size() && (s[j] == '/' || s[j] == '-' || s[j] == ',')) {
                size_t j2 = j + 1;
                while (j2 < s.size() && (s[j2] == ' ' || s[j2] == '\t' || s[j2] == '\n' || s[j2] == '\r')) ++j2;
                if (j2 < s.size() && classify((unsigned char)s[j2]) == DIGIT) {
                    cur += s[j];
                    i = j2 - 1;
                    continue;
                }
            }
        }
        if (k == SEP && c == '.' && next_digit) {  // leading-dot decimal: ".5" -> "0.5"
            flush();
            cur = "0.";
            curc = DIGIT;
            continue;
        }
        if (k == SEP) { flush(); curc = SEP; continue; }
        if (k != curc && split) flush();
        curc = k;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        cur += (char)c;
    }
    flush();
    return out;
}

bool contains_run(const std::vector<std::string> &hay, const std::vector<std::string> &needle) {
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        size_t j = 0;
        while (j < needle.size() && hay[i + j] == needle[j]) ++j;
        if (j == needle.size()) return true;
    }
    return false;
}

bool is_filler(const std::string &t) {
    static const char *kFiller[] = {"tab", "tabs", "tablet", "tablets", "cap", "caps",
                                    "capsule", "capsules", "syp", "syrup", "inj", "injection"};
    for (auto *f : kFiller) if (t == f) return true;
    return false;
}

bool blank(const std::string &v) {
    for (unsigned char c : v) if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return false;
    return true;
}

// Name fallback: all non-filler name tokens in order within a short window.
bool name_window_match(const std::vector<std::string> &q, const std::vector<std::string> &nf) {
    if (nf.empty()) return false;
    size_t win = nf.size() + 2;
    for (size_t i = 0; i < q.size(); ++i) {
        if (q[i] != nf[0]) continue;
        size_t k = 1, end = std::min(q.size(), i + win);
        for (size_t j = i + 1; j < end && k < nf.size(); ++j)
            if (q[j] == nf[k]) ++k;
        if (k == nf.size()) return true;
    }
    return false;
}

bool name_ok(const std::string &value, const std::string &quote) {
    std::vector<std::string> v = tokens(value, false), q = tokens(quote, false);
    if (v.empty()) return false;
    if (contains_run(q, v)) return true;
    std::vector<std::string> nf;
    for (auto &t : v) if (!is_filler(t)) nf.push_back(t);
    return name_window_match(q, nf);
}

std::string unit_key(const std::string &t) {
    static const char *kPlural[] = {"tablets", "drops", "capsules", "caps", "tabs"};
    for (auto *p : kPlural)
        if (t == p) return t.substr(0, t.size() - 1);
    return t;
}

bool is_number(const std::string &t) { return !t.empty() && t[0] >= '0' && t[0] <= '9'; }

// True when the quote has more than one distinct number directly before the dose's unit.
bool ambiguous_dose(const std::vector<std::string> &v, const std::vector<std::string> &q) {
    if (v.empty() || is_number(v.back())) return false;
    std::string unit = unit_key(v.back());
    std::set<std::string> nums;
    for (size_t i = 1; i < q.size(); ++i)
        if (is_number(q[i - 1]) && unit_key(q[i]) == unit) nums.insert(q[i - 1]);
    return nums.size() > 1;
}

bool field_ok(const std::string &value, const std::vector<std::string> &q, bool is_dose) {
    if (blank(value)) return true;
    std::vector<std::string> v = tokens(value);
    if (v.empty()) return false;  // non-empty but nothing verifiable
    if (!contains_run(q, v)) return false;
    return !(is_dose && ambiguous_dose(v, q));
}

}  // namespace

std::string normalize_for_match(const std::string &s) {
    std::string o;
    for (auto &t : tokens(s)) { if (!o.empty()) o += ' '; o += t; }
    return o;
}

MedCheck check_medicine(const MedFields &f, const std::string &quote) {
    std::vector<std::string> q = tokens(quote);
    MedCheck r;
    struct { const char *n; const std::string *v; } fs[] = {
        {"name", &f.name}, {"dose", &f.dose}, {"when_text", &f.when_text},
        {"frequency", &f.frequency}, {"duration", &f.duration}};
    bool name_bad = false, dose_bad = false;
    for (auto &e : fs) {
        bool is_name = std::strcmp(e.n, "name") == 0;
        if (is_name ? (blank(*e.v) || name_ok(*e.v, quote)) : field_ok(*e.v, q, !std::strcmp(e.n, "dose"))) continue;
        r.fields.push_back(e.n);
        if (!std::strcmp(e.n, "name")) name_bad = true;
        if (!std::strcmp(e.n, "dose")) dose_bad = true;
    }
    bool both = !blank(f.name) && !blank(f.dose);
    if (both && name_bad && dose_bad) r.status = "drop";
    else r.status = r.fields.empty() ? "ok" : "check";
    return r;
}

}  // namespace sve
