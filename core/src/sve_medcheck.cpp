#include "sve_medcheck.h"

#include <cstring>

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
        if (e.d && t == e.w) return e.d;
    return nullptr;
}

std::vector<std::string> tokens(const std::string &raw) {
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
        if (k == SEP && c == '.' && curc == DIGIT && !cur.empty() && i + 1 < s.size() &&
            classify((unsigned char)s[i + 1]) == DIGIT) {
            cur += '.';  // decimal point between digits
            continue;
        }
        if (k == SEP) { flush(); curc = SEP; continue; }
        if (k != curc) flush();
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

bool ascii_alpha_token(const std::string &t) {
    for (unsigned char c : t) if (!(c >= 'a' && c <= 'z')) return false;
    return !t.empty();
}

bool field_ok(const std::string &value, const std::vector<std::string> &q, bool is_name) {
    std::vector<std::string> v = tokens(value);
    if (v.empty()) return true;
    if (contains_run(q, v)) return true;
    if (!is_name) return false;
    bool any = false;
    for (auto &t : v) {
        if (t.size() < 4 || !ascii_alpha_token(t)) continue;
        any = true;
        if (!contains_run(q, {t})) return false;
    }
    return any;  // a name with no long ASCII token cannot pass this way
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
        if (field_ok(*e.v, q, std::strcmp(e.n, "name") == 0)) continue;
        r.fields.push_back(e.n);
        if (!std::strcmp(e.n, "name")) name_bad = true;
        if (!std::strcmp(e.n, "dose")) dose_bad = true;
    }
    bool both = !tokens(f.name).empty() && !tokens(f.dose).empty();
    if (both && name_bad && dose_bad) r.status = "drop";
    else r.status = r.fields.empty() ? "ok" : "check";
    return r;
}

}  // namespace sve
