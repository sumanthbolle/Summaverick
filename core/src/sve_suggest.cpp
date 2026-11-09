#include "sve_suggest.h"
#include "sve_medcheck.h"

#include <algorithm>
#include <cstring>
#include <cstdint>
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

std::string trim_copy(const std::string &s) { return trim(s); }

bool valid_hhmm(const std::string &s) {
    if (s.size() != 5 || s[2] != ':' || !digits(s, 0, 2) || !digits(s, 3, 2)) return false;
    return std::stoi(s.substr(0, 2)) < 24 && std::stoi(s.substr(3, 2)) < 60;
}

bool valid_ymd(const std::string &s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-' || !digits(s, 0, 4) || !digits(s, 5, 2) || !digits(s, 8, 2)) return false;
    int m = std::stoi(s.substr(5, 2)), d = std::stoi(s.substr(8, 2));
    return m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

namespace {

int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {  // Howard Hinnant's algorithm
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

std::string civil_from_days(int64_t z) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    const int64_t y = (int64_t)yoe + era * 400 + (m <= 2);
    char b[32];
    snprintf(b, sizeof b, "%04lld-%02u-%02u", (long long)y, m, d);
    return b;
}

int64_t ymd_days(const std::string &s) {
    return days_from_civil(std::stoi(s.substr(0, 4)), (unsigned)std::stoi(s.substr(5, 2)), (unsigned)std::stoi(s.substr(8, 2)));
}

int weekday_of(int64_t days) { return (int)(((days % 7) + 7 + 4) % 7); }  // 0 = Sunday; 1970-01-01 was a Thursday

const char *kWeekdayNames[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

// Weekdays (0 = Sunday) said in the quote: English full or short forms, Telugu "...వార" (any ending),
// or the short Telugu word on its own.
std::set<int> named_weekdays(const std::string &quote) {
    static const struct { const char *en[4]; const char *te_stem, *te_short; } kDays[] = {
        {{"sunday", "sun", nullptr, nullptr}, "ఆదివార", "ఆది"},
        {{"monday", "mon", nullptr, nullptr}, "సోమవార", "సోమ"},
        {{"tuesday", "tue", "tues", nullptr}, "మంగళవార", "మంగళ"},
        {{"wednesday", "wed", nullptr, nullptr}, "బుధవార", "బుధ"},
        {{"thursday", "thu", "thur", "thurs"}, "గురువార", "గురు"},
        {{"friday", "fri", nullptr, nullptr}, "శుక్రవార", "శుక్ర"},
        {{"saturday", "sat", nullptr, nullptr}, "శనివార", "శని"}};
    std::vector<std::string> q = norm_tokens(quote);
    auto has_tok = [&](const char *t) { return t && std::find(q.begin(), q.end(), t) != q.end(); };
    std::set<int> named;
    for (int i = 0; i < 7; ++i) {
        const auto &k = kDays[i];
        bool hit = quote.find(k.te_stem) != std::string::npos || has_tok(k.te_short);
        for (const char *e : k.en) hit = hit || has_tok(e);
        if (hit) named.insert(i);
    }
    return named;
}

bool says_any(const std::string &quote, std::initializer_list<const char *> en, std::initializer_list<const char *> te) {
    std::vector<std::string> q = norm_tokens(quote);
    for (const char *w : en) if (std::find(q.begin(), q.end(), w) != q.end()) return true;
    for (const char *w : te) if (quote.find(w) != std::string::npos) return true;
    return false;
}

}  // namespace

bool quote_supports_date(const std::string &quote) {
    static const std::set<std::string> kMonths = {"january", "february", "march", "april", "june", "july", "august",
        "september", "october", "november", "december", "jan", "feb", "mar", "apr", "jun", "jul", "aug", "sep", "sept",
        "oct", "nov", "dec"};
    static const std::set<std::string> kSpans = {"day", "days", "week", "weeks", "month", "months"};
    static const std::set<std::string> kOrd = {"st", "nd", "rd", "th"};
    std::vector<std::string> q = norm_tokens(quote);
    auto num = [](const std::string &t) { return !t.empty() && t[0] >= '0' && t[0] <= '9'; };
    for (size_t i = 0; i < q.size(); ++i) {
        const std::string &t = q[i];
        if (num(t) && (t.find('/') != std::string::npos || t.find('-') != std::string::npos)) return true;  // 20/11, 2026-11-20
        if (kMonths.count(t) || t == "today" || t == "tomorrow" || t == "tonight") return true;
        if (num(t) && i + 1 < q.size() && (kSpans.count(q[i + 1]) || kOrd.count(q[i + 1]))) return true;
    }
    if (!named_weekdays(quote).empty()) return true;
    for (const char *w : {"ఈరోజు", "రేపు", "రోజు", "వారం", "వారాలు", "నెల"})
        if (quote.find(w) != std::string::npos) return true;
    return false;
}

bool quote_has_hour(const std::string &quote, const std::string &hhmm) {
    if (!valid_hhmm(hhmm)) return false;
    static const struct { const char *w; int v; } kTe[] = {
        {"ఆరు", 6}, {"ఏడు", 7}, {"ఎనిమిది", 8}, {"తొమ్మిది", 9}, {"పది", 10}, {"పదకొండు", 11}, {"పన్నెండు", 12}};
    int h = std::stoi(hhmm.substr(0, 2));
    auto hour_ok = [&](int v) { return v == h || (v >= 1 && v <= 12 && v % 12 == h % 12); };
    std::vector<std::string> q = norm_tokens(quote);
    for (size_t i = 0; i < q.size(); ++i) {
        const std::string &t = q[i];
        if (!t.empty() && t[0] >= '0' && t[0] <= '9') {
            size_t k = 0;
            while (k < t.size() && t[k] >= '0' && t[k] <= '9' && k < 4) ++k;
            int v = std::stoi(t.substr(0, k));
            // "9:30" tokenizes as "9", "30": skip the minutes token, so it is never read as an hour and
            // the am/pm after it still counts.
            size_t j = i + 1;
            if (k == t.size() && j < q.size() && q[j].size() <= 2 && digits(q[j], 0, q[j].size()) &&
                std::stoi(q[j]) < 60) {
                ++j;
                i = j - 1;
            }
            // "9 am" / "9 a.m." / "9pm" / "9:30 am": the stated half of the day must match
            std::string next = j < q.size() ? q[j] : "", after = j + 1 < q.size() ? q[j + 1] : "";
            bool am = next == "am" || (next == "a" && after == "m");
            bool pm = next == "pm" || (next == "p" && after == "m");
            if (v >= 1 && v <= 12 && (am || pm)) {
                if ((v % 12) + (pm ? 12 : 0) == h) return true;
                continue;
            }
            if (hour_ok(v)) return true;
        }
        for (const auto &w : kTe) if (t == w.w && hour_ok(w.v)) return true;
    }
    return false;
}

std::string weekday_name(const std::string &ymd) {
    if (!valid_ymd(ymd)) return "";
    return kWeekdayNames[weekday_of(ymd_days(ymd))];
}

bool date_matches_weekday(const std::string &ymd, const std::string &quote) {
    std::set<int> named = named_weekdays(quote);
    return named.size() != 1 || weekday_name(ymd) == kWeekdayNames[*named.begin()];
}

namespace {
// Weekday check plus: the date must be the nearest upcoming named weekday from today (today itself
// only when the quote says today), unless the quote says "next"/"వచ్చే".
bool date_plausible(const std::string &ymd, const std::string &quote, const std::string &today) {
    if (!date_matches_weekday(ymd, quote)) return false;
    std::set<int> named = named_weekdays(quote);
    if (named.size() != 1 || !valid_ymd(today)) return true;
    if (says_any(quote, {"next"}, {"వచ్చే"})) return true;
    int64_t t = ymd_days(today);
    int ahead = (*named.begin() - weekday_of(t) + 7) % 7;
    if (ahead == 0 && !says_any(quote, {"today"}, {"ఈరోజు", "ఈ రోజు"})) ahead = 7;
    return ymd == civil_from_days(t + ahead);
}
}  // namespace

std::string sanitize_repeat(const std::string &s) {
    for (const char *r : {"daily", "twice_daily", "weekly", "once"}) if (s == r) return s;
    return "";
}

bool twice_daily_mismatch(const std::string &repeat, const json &times) {
    return repeat == "twice_daily" && valid_times(times).size() != 2;
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
        {"\xE0\xB0\xA8\xE0\xB0\xBF\xE0\xB0\xA6\xE0\xB1\x8D\xE0\xB0\xB0", "bed"},  // నిద్ర
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
    med_out = {{"name", f.name}, {"dose", f.dose}, {"when_text", f.when_text}, {"frequency", f.frequency},
               {"duration", f.duration}};
    return true;
}

// Valid date or null, flagged when the quote names a different weekday. nearest: also flagged unless it
// is the nearest upcoming named weekday (date_plausible). strict (medicines): also flagged unless the
// date is today or the quote names its weekday.
json date_field(const json &item, const char *key, std::vector<std::string> &fields, bool strict,
                const std::string &today, bool nearest) {
    std::string d = jstr(item, key);
    if (!valid_ymd(d)) return nullptr;
    std::string q = jstr(item, "quote");
    bool ok = quote_supports_date(q) && (nearest ? date_plausible(d, q, today) : date_matches_weekday(d, q));
    if (ok && strict && d != today) ok = named_weekdays(q).size() == 1;
    if (!ok) fields.push_back(key);
    return d;
}

bool has_med_name(const json &med) { return !med.is_null() && !jstr(med, "name").empty(); }

// True when a saved medicine with this name has a (non-empty) dose that differs from the card's.
bool dose_differs_from_saved(const json &saved, const json &med) {
    if (!saved.is_array() || !has_med_name(med) || jstr(med, "dose").empty()) return false;
    std::string n = normalize_for_match(jstr(med, "name")), d = normalize_for_match(jstr(med, "dose"));
    for (const auto &m : saved)
        if (normalize_for_match(jstr(m, "name")) == n && !jstr(m, "dose").empty() && normalize_for_match(jstr(m, "dose")) != d)
            return true;
    return false;
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

std::vector<json> build_suggestions(const Extracted &e, const json &pieces, int64_t note_id, const json &routine,
                                    const std::string &today, const std::string &lang,
                                    const std::vector<std::string> &names, const json &saved) {
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
        if (!med.is_null() && times.empty()) b.fields.push_back("times");  // nothing to schedule
        std::string text;
        if (has_med_name(med)) {
            text = reminder_text(jstr(med, "name"), jstr(med, "dose"), lang);  // notification text from checked fields
        } else {
            text = trim(jstr(it, "text"));
            if (mentions_medicine(text, names)) b.fields.push_back("text");
        }
        if (text.empty()) continue;
        if (dose_differs_from_saved(saved, med)) b.fields.push_back("dose");  // differs from the saved medicine: needs a look
        std::string rep = sanitize_repeat(jstr(it, "repeat"));
        if (med.is_null() && times.empty() && (rep.empty() || rep == "once")) {
            // A one-off with no time of day cannot be scheduled: it is a task due that day.
            json task = it;
            task["due_date"] = jstr(it, "start_date");  // never until_date
            b.rec["kind"] = "task";
            json due = date_field(task, "due_date", b.fields, false, today, true);
            out.push_back(finish(b, {{"text", text}, {"who", nullptr}, {"due_date", due}}));
            continue;
        }
        if (twice_daily_mismatch(rep, json(times))) b.fields.push_back("times");  // twice a day needs two times
        const bool strict = !med.is_null();
        json start = date_field(it, "start_date", b.fields, strict, today, true);
        json until = date_field(it, "until_date", b.fields, strict, today, false);
        out.push_back(finish(b, {{"text", text}, {"when_text", when}, {"times", times},
                                 {"repeat", str_or_null(rep)},
                                 {"start_date", start}, {"until_date", until}, {"medicine", med}}));
    }
    for (const auto &it : e.tasks) {
        std::string text = trim(jstr(it, "text"));
        if (text.empty()) continue;
        Base b = base_for("task", it, pieces, note_id);
        if (mentions_medicine(text, names)) b.fields.push_back("text");
        json due = date_field(it, "due_date", b.fields, false, today, true);
        out.push_back(finish(b, {{"text", text}, {"who", str_or_null(trim(jstr(it, "who")))}, {"due_date", due}}));
    }
    for (const auto &it : e.profile) {
        std::string change = jstr(it, "change");
        bool is_med = change == "add_medicine" || change == "update_medicine" || change == "stop_medicine";
        if (!is_med && change != "routine") continue;
        if (trim(jstr(it, "quote")).empty()) continue;  // not stated in the recording
        Base b = base_for("profile", it, pieces, note_id);
        if (is_med) {
            std::string when = trim(jstr(jobj(it, "medicine"), "when_text"));
            if (when.empty()) when = trim(jstr(it, "when_text"));
            json med;
            if (!check_med(it, when, b.fields, med) || !has_med_name(med)) continue;
            if (change != "stop_medicine") {
                if (resolve_times(when, routine).empty()) b.fields.push_back("times");
                if (dose_differs_from_saved(saved, med)) b.fields.push_back("dose");  // would change the saved dose
            }
            std::string text = profile_text(change, jstr(med, "name"), jstr(med, "dose"), lang);
            out.push_back(finish(b, {{"change", change}, {"text", text}, {"when_text", when}, {"medicine", med}}));
        } else {
            json r = clean_routine(jobj(it, "routine"));
            if (r.empty()) continue;  // a routine change must say which time
            std::string text = trim(jstr(it, "detail"));
            if (text.empty())
                for (auto kv = r.begin(); kv != r.end(); ++kv)
                    text += (text.empty() ? "" : ", ") + kv.key() + " " + kv.value().get<std::string>();
            if (mentions_medicine(text, names)) b.fields.push_back("text");
            for (auto kv = r.begin(); kv != r.end(); ++kv)  // each new time must be said in the quote
                if (!quote_has_hour(jstr(it, "quote"), kv.value().get<std::string>())) b.fields.push_back("routine");
            out.push_back(finish(b, {{"change", change}, {"text", text}, {"detail", text}, {"routine", r},
                                     {"medicine", nullptr}}));
        }
    }
    for (const auto &it : e.facts) {
        std::string text = trim(jstr(it, "text"));
        if (text.empty()) continue;
        Base b = base_for("fact", it, pieces, note_id);
        if (mentions_medicine(text, names)) b.fields.push_back("text");
        out.push_back(finish(b, {{"memory_kind", "fact"}, {"text", text}}));
    }
    for (const auto &v : e.vocab) {
        std::string t = v.is_string() ? trim(v.get<std::string>()) : "";
        if (t.empty() || t.size() > 60 || std::count(t.begin(), t.end(), ' ') > 2) continue;  // terms, not sentences
        out.push_back({{"kind", "vocab"}, {"payload", {{"text", t}}}, {"quote", nullptr}, {"note_id", note_id},
                       {"start_sec", nullptr}, {"check_status", nullptr}, {"check_fields", json::array()}});
    }
    return out;
}

std::string reminder_text(const std::string &name, const std::string &dose, const std::string &lang) {
    std::string nd = dose.empty() ? name : name + " " + dose;
    return lang == "te" ? nd + " వేసుకోండి" : "Take " + nd;
}

std::string profile_text(const std::string &change, const std::string &name, const std::string &dose,
                         const std::string &lang) {
    const bool te = lang == "te";
    std::string nd = dose.empty() || change == "stop_medicine" ? name : name + " " + dose;
    if (change == "stop_medicine") return (te ? "మందు ఆపండి: " : "Stop medicine: ") + nd;
    if (change == "update_medicine") return (te ? "మందు మార్చండి: " : "Change medicine: ") + nd;
    return (te ? "మందు చేర్చండి: " : "Add medicine: ") + nd;
}

json clean_routine(const json &routine) {
    json out = json::object();
    for (const char *k : {"wake", "breakfast", "lunch", "dinner", "bed"}) {
        std::string t = jstr(routine, k);
        if (valid_hhmm(t)) out[k] = t;
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

std::string without_only_words(const std::string &text) {
    std::string s = text;
    for (const char *w : {"మాత్రమే", "మాత్రం"})
        for (size_t at = s.find(w); at != std::string::npos; at = s.find(w, at)) s.replace(at, strlen(w), " ");
    return s;
}

bool is_medicine_question(const std::string &raw_question, const std::vector<std::string> &names) {
    if (!names_in_question(raw_question, names).empty()) return true;
    const std::string question = without_only_words(raw_question);
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

json hits_naming(const json &hits, const std::vector<std::string> &names) {
    json out = json::array();
    if (!hits.is_array()) return out;
    for (const auto &h : hits)
        if (!names_in_question(jstr(h, "text"), names).empty()) out.push_back(h);
    return out;
}

std::string medicine_list_answer(const json &medicines, const std::string &lang) {
    std::string list = medicine_answer(medicines, json::array(), lang);
    size_t nl = list.find('\n');
    if (list.empty() || nl == std::string::npos) return "";
    const bool te = lang == "te";
    return std::string(te ? "మీరు సేవ్ చేసిన మందులు:" : "Your saved medicines:") + list.substr(nl) + "\n\n" +
           (te ? "మీ రికార్డింగ్‌లలో ఈ ప్రశ్నకు ఏ మందు పేరూ లేదు." : "Your recordings don't name a medicine for this question.");
}

bool filter_topic_summary(json &s, const std::vector<std::string> &names) {
    bool dropped = false;
    // A sentence ends at . ! ? followed by a space or the end (so "2.5 mg" stays whole), or a newline.
    std::string text = jstr(s, "summary"), kept, cur;
    auto flush = [&] {
        std::string t = trim(cur);
        cur.clear();
        if (t.empty()) return;
        if (mentions_medicine(t, names)) { dropped = true; return; }
        kept += (kept.empty() ? "" : " ") + t;
    };
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        cur += c;
        bool next_blank = i + 1 == text.size() || text[i + 1] == ' ' || text[i + 1] == '\n';
        if (c == '\n' || ((c == '.' || c == '!' || c == '?') && next_blank)) flush();
    }
    flush();
    s["summary"] = kept;
    for (const char *key : {"key_terms", "quiz"}) {
        json keep = json::array();
        for (const auto &e : jarr(s, key)) {
            std::string all;
            if (e.is_object())
                for (auto it = e.begin(); it != e.end(); ++it) if (it->is_string()) all += it->get<std::string>() + " ";
            if (mentions_medicine(all, names)) dropped = true;
            else keep.push_back(e);
        }
        s[key] = keep;
    }
    return dropped;
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
    static const std::set<std::string> kWords = {"mg", "mcg", "tablet", "tablets", "capsule", "capsules",
        "medicine", "medicines", "medication", "medications", "pill", "pills", "dose", "doses", "dosage", "syrup",
        "injection", "injections"};
    // Units that are also ordinary words ("the ML model", "a new tab") count only right after a number.
    static const std::set<std::string> kAfterNumber = {"ml", "iu", "tab", "tabs"};
    const std::vector<std::string> t = norm_tokens(text);
    for (size_t i = 0; i < t.size(); ++i) {
        if (kWords.count(t[i])) return true;
        if (kAfterNumber.count(t[i]) && i > 0 && !t[i - 1].empty() && t[i - 1][0] >= '0' && t[i - 1][0] <= '9') return true;
    }
    const std::string plain = without_only_words(text);
    for (const char *w : {"మందు", "మాత్ర", "డోస్"}) if (plain.find(w) != std::string::npos) return true;
    return !names_in_question(text, names).empty();
}

// ---------------------------------------------------------------- the person's own reminder / to-do

namespace {

// Optional string key: missing or null -> "" (ok); a string -> trimmed (ok); anything else -> false.
bool opt_text(const json &in, const char *key, std::string &out) {
    out.clear();
    auto it = in.find(key);
    if (it == in.end() || it->is_null()) return true;
    if (!it->is_string()) return false;
    out = trim(it->get<std::string>());
    return true;
}

// Optional whole-number key >= min: missing or null -> null (ok); anything else that is not one -> false.
bool opt_whole(const json &in, const char *key, int64_t min, json &out) {
    out = nullptr;
    auto it = in.find(key);
    if (it == in.end() || it->is_null()) return true;
    int64_t v = jint(in, key, INT64_MIN);
    if (v == INT64_MIN || v < min) return false;
    out = v;
    return true;
}

// note_id and start_sec of the source moment. start_sec without a note means nothing, so it is dropped.
bool source_fields(const json &in, json &note, json &sec) {
    if (!opt_whole(in, "note_id", 1, note) || !opt_whole(in, "start_sec", 0, sec)) return false;
    if (note.is_null()) sec = nullptr;
    return true;
}

}  // namespace

json direct_reminder_row(const json &in, const std::string &today) {
    if (!in.is_object()) return nullptr;
    std::string text, rep, start, until;
    if (!opt_text(in, "text", text) || text.empty()) return nullptr;
    auto t = in.find("times");
    if (t == in.end() || !t->is_array() || t->empty()) return nullptr;
    for (const auto &v : *t)
        if (!v.is_string() || !valid_hhmm(v.get<std::string>())) return nullptr;
    std::vector<std::string> times = valid_times(*t);  // duplicates removed
    if (times.empty()) return nullptr;
    if (!opt_text(in, "repeat", rep)) return nullptr;
    if (rep.empty()) rep = "once";
    else if (sanitize_repeat(rep).empty()) return nullptr;
    else rep = sanitize_repeat(rep);
    if (!opt_text(in, "start_date", start) || !opt_text(in, "until_date", until)) return nullptr;
    if (start.empty()) start = today;
    if (!valid_ymd(start) || (!until.empty() && (!valid_ymd(until) || until < start))) return nullptr;
    json note, sec, med;
    if (!source_fields(in, note, sec) || !opt_whole(in, "medicine_id", 1, med)) return nullptr;
    return {{"text", text}, {"times", times}, {"repeat", rep}, {"start_date", start},
            {"until_date", str_or_null(until)}, {"note_id", note}, {"start_sec", sec}, {"medicine_id", med}};
}

json direct_task_row(const json &in) {
    if (!in.is_object()) return nullptr;
    std::string text, who, due;
    if (!opt_text(in, "text", text) || text.empty()) return nullptr;
    if (!opt_text(in, "who", who) || !opt_text(in, "due_date", due)) return nullptr;
    if (!due.empty() && !valid_ymd(due)) return nullptr;
    json note, sec;
    if (!source_fields(in, note, sec)) return nullptr;
    return {{"text", text}, {"who", str_or_null(who)}, {"due_date", str_or_null(due)}, {"note_id", note}, {"start_sec", sec}};
}

std::vector<int64_t> medicines_named(const std::string &text, const json &medicines) {
    std::vector<int64_t> out;
    if (!medicines.is_array()) return out;
    for (const auto &m : medicines) {
        std::string name = jstr(m, "name");
        int64_t id = jint(m, "id", 0);
        if (id > 0 && !trim(name).empty() && !names_in_question(text, {name}).empty()) out.push_back(id);
    }
    return out;
}

}  // namespace sve
