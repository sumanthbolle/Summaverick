#include "sve_chat.h"
#include "sve_context.h"  // truncate_utf8
#include "sve_health.h"
#include "sve_suggest.h"  // keyword_query, mentions_medicine

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>

namespace sve {

namespace {

// Next code point of UTF-8 text at i (advances i); U+FFFD for a broken sequence.
uint32_t next_cp(const std::string &s, size_t &i) {
    unsigned char c = (unsigned char)s[i++];
    if (c < 0x80) return c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
    if (extra < 0) return 0xFFFD;
    uint32_t cp = c & (0x3F >> extra);
    for (int k = 0; k < extra; ++k) {
        if (i >= s.size() || ((unsigned char)s[i] & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | ((unsigned char)s[i++] & 0x3F);
    }
    return cp;
}

bool is_telugu_letter(uint32_t cp) { return cp >= 0x0C00 && cp <= 0x0C7F && !(cp >= 0x0C66 && cp <= 0x0C6F); }

// A letter of any script, roughly: ASCII letters and code points from U+00C0 that are not punctuation,
// symbols, digits of Telugu, joiners or emoji.
bool is_letter(uint32_t cp) {
    if (cp < 0x80) return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
    if (cp < 0xC0 || cp == 0xD7 || cp == 0xF7 || cp == 0xFFFD) return false;
    if (cp >= 0x0C66 && cp <= 0x0C6F) return false;   // Telugu digits
    if (cp >= 0x0964 && cp <= 0x0965) return false;   // danda
    if (cp >= 0x2000 && cp <= 0x2BFF) return false;   // punctuation, symbols, arrows
    if (cp >= 0x3000 && cp <= 0x303F) return false;
    if (cp >= 0xFE00 && cp <= 0xFE0F) return false;
    if (cp >= 0x1F000) return false;                  // emoji
    return true;
}

bool has_letters(const std::string &s) {
    for (size_t i = 0; i < s.size();) if (is_letter(next_cp(s, i))) return true;
    return false;
}

std::string mmss(int64_t sec) {
    if (sec < 0) sec = 0;
    char b[32];
    snprintf(b, sizeof b, "%lld:%02lld", (long long)(sec / 60), (long long)(sec % 60));
    return b;
}

size_t words_in(const std::string &s) {
    size_t n = 0;
    bool in = false;
    for (char c : s) {
        if (c == ' ') in = false;
        else if (!in) { in = true; ++n; }
    }
    return n;
}

const char *const kOpen[] = {"<|channel>", "<think>"};
const char *const kClose[] = {"<channel|>", "</think>"};

bool is_ws(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }

// A citation group's inner text: labels like S1, separated by commas or spaces (apply_citations' rule).
bool valid_group(const std::string &g) {  // g = "[...]"
    std::string inner = g.substr(1, g.size() - 2), cur;
    int labels = 0;
    for (size_t k = 0; k <= inner.size(); ++k) {
        char ch = k < inner.size() ? inner[k] : ',';
        if (ch != ',' && ch != ' ') { cur += ch; continue; }
        if (cur.empty()) continue;
        if (cur.size() < 2 || cur[0] != 'S') return false;
        for (size_t d = 1; d < cur.size(); ++d) if (cur[d] < '0' || cur[d] > '9') return false;
        ++labels;
        cur.clear();
    }
    return labels > 0;
}

}  // namespace

std::string question_lang(const std::string &text) {
    size_t letters = 0, te = 0;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = next_cp(text, i);
        if (!is_letter(cp)) continue;
        ++letters;
        if (is_telugu_letter(cp)) ++te;
    }
    return letters > 0 && te * 10 >= letters * 3 ? "te" : "en";
}

std::string chat_lang(const std::string &question, const std::string &hint, const std::string &fallback) {
    if (has_letters(question)) return question_lang(question);
    if (hint == "te" || hint == "en") return hint;
    return fallback == "te" ? "te" : "en";
}

std::vector<ChatTurn> chat_turns(const json &messages) {
    std::vector<ChatTurn> out;
    if (!messages.is_array()) return out;
    for (const auto &m : messages) {
        if (!m.is_object()) continue;
        std::string role = jstr(m, "role"), text = trim_copy(jstr(m, "text"));
        if ((role != "user" && role != "assistant") || text.empty()) continue;
        const bool health = role == "assistant" && jstr(m, "kind") == "health";
        if (role == "assistant") {
            auto med = m.find("medicine");
            if (med != m.end() && med->is_boolean() && med->get<bool>()) {
                out.push_back({role, "(A medicine answer, quoted from the saved facts and recordings.)", true, false});
                continue;
            }
            auto src = m.find("sources");
            if (src != m.end() && src->is_array() && !src->empty()) {
                std::string list;
                for (const auto &s : *src) {
                    if (!s.is_object()) continue;
                    std::string t = jstr(s, "title");
                    list += (list.empty() ? "" : "; ") + (t.empty() ? std::string("Recording") : t) + " at " +
                            mmss(jint(s, "start_sec", 0));
                }
                if (!list.empty()) text += "\n(Sources: " + list + ")";
            }
        }
        out.push_back({role, text, false, health});
    }
    return out;
}

std::vector<ChatTurn> history_window(const std::vector<ChatTurn> &earlier, int budget_tokens,
                                     const std::function<int(const std::string &)> &count_tokens) {
    std::vector<ChatTurn> picked;  // newest first
    int used = 0;
    for (size_t k = earlier.size(); k-- > 0;) {
        int n = count_tokens(earlier[k].text);
        if (used + n <= budget_tokens) {
            picked.push_back(earlier[k]);
            used += n;
            continue;
        }
        if (picked.empty()) {  // the newest alone is too long: cut it to fit
            std::string t = earlier[k].text;
            while (!t.empty() && count_tokens(t) > budget_tokens) t = truncate_utf8(t, t.size() * 9 / 10);
            if (!t.empty()) picked.push_back({earlier[k].role, t});
        }
        break;
    }
    return std::vector<ChatTurn>(picked.rbegin(), picked.rend());
}

std::string history_block(const std::vector<ChatTurn> &window) {
    if (window.empty()) return "";
    std::string out = "Conversation so far (oldest first):\n";
    for (const auto &t : window) out += (t.role == "user" ? "Person: " : "Summaverick: ") + t.text + "\n";
    return out;
}

std::string chat_search_query(const std::string &question, const std::vector<ChatTurn> &earlier) {
    // Period words pick the notes (Route), they are not what the passage must say.
    static const std::set<std::string> kPeriod = {"today", "tonight", "yesterday", "tomorrow", "week", "weeks", "month", "months",
                                                  "last", "ఈ", "రోజు", "ఈరోజు", "నేడు", "నిన్న", "వారం", "గత", "పోయిన", "నెల",
                                                  "మాత్రం", "మాత్రమే", "మాత్రమైనా", "మాత్రాన"};
    std::string kq;
    {
        std::string cur;
        for (char c : keyword_query(question) + " ") {
            if (c != ' ') { cur += c; continue; }
            if (!cur.empty() && !kPeriod.count(cur)) kq += (kq.empty() ? "" : " ") + cur;
            cur.clear();
        }
    }
    if (words_in(kq) < 2)
        for (size_t k = earlier.size(); k-- > 0;)
            if (earlier[k].role == "user") {
                std::string prev = keyword_query(earlier[k].text);
                if (!prev.empty()) kq += (kq.empty() ? "" : " ") + prev;
                break;
            }
    return kq.empty() ? question : kq;
}

std::string chat_recordings_prompt(const std::string &history, const std::string &material, const std::string &question,
                                   const std::string &lang_name) {
    std::string p;
    if (!history.empty())
        p += history + "(The conversation is only context for the question; the facts come from the material.)\n\n";
    p += "Material from the person's recordings and memory. Each recording excerpt starts with a label such as [S1].\n\n" +
         material + "\nQuestion: " + question + "\n\n"
         "Answer in a few short sentences in " + lang_name + ", using only the material above, even when the material "
         "or the conversation is in another language. A follow-up question is read in the light of the conversation. "
         "After each statement put the label of the excerpt it comes from, for example [S1]. If the material does not "
         "answer the question, reply with exactly NOT_FOUND and nothing else.";
    return p;
}

// ---------------------------------------------------------------- ThinkStripper

std::string ThinkStripper::feed(const std::string &piece) {
    buf_ += piece;
    std::string out;
    for (;;) {
        if (open_ < 0) {
            size_t best = std::string::npos;
            int which = -1;
            for (int t = 0; t < 2; ++t) {
                size_t a = buf_.find(kOpen[t]);
                if (a < best) { best = a; which = t; }
            }
            if (which >= 0) {
                out += buf_.substr(0, best);
                buf_.erase(0, best + strlen(kOpen[which]));
                open_ = which;
                continue;
            }
            // Hold the longest end of the buffer that could still start a tag.
            size_t keep = 0;
            for (int t = 0; t < 2; ++t) {
                size_t L = strlen(kOpen[t]);
                for (size_t k = std::min(L - 1, buf_.size()); k > keep; --k)
                    if (buf_.compare(buf_.size() - k, k, kOpen[t], k) == 0) { keep = k; break; }
            }
            out += buf_.substr(0, buf_.size() - keep);
            buf_.erase(0, buf_.size() - keep);
            return out;
        }
        size_t b = buf_.find(kClose[open_]);
        if (b != std::string::npos) {
            buf_.erase(0, b + strlen(kClose[open_]));
            open_ = -1;
            continue;
        }
        size_t L = strlen(kClose[open_]) - 1;  // the end that could still start the close tag
        if (buf_.size() > L) buf_.erase(0, buf_.size() - L);
        return out;
    }
}

std::string ThinkStripper::finish() {
    std::string out = open_ < 0 ? buf_ : "";
    buf_.clear();
    open_ = -1;
    return out;
}

// ---------------------------------------------------------------- AnswerStream

AnswerStream::AnswerStream(std::vector<std::string> medicine_names, std::function<bool(const std::string &)> sink, Mode mode)
    : names_(std::move(medicine_names)), sink_(std::move(sink)), mode_(mode) {}

void AnswerStream::flush_group() {
    if (group_.empty()) return;
    if (spaces_) pending_ += ' ';
    spaces_ = 0;
    pending_ += group_;
    group_.clear();
    started_ = true;
}

void AnswerStream::cite(const std::string &visible) {
    for (char c : visible) {
        if (!group_.empty()) {
            group_ += c;
            if (c == ']') {
                if (valid_group(group_)) { group_.clear(); spaces_ = 0; }  // the marker and the spaces before it go
                else flush_group();
            } else if (!(c == 'S' || c == ',' || c == ' ' || (c >= '0' && c <= '9')) || group_.size() > 64) {
                flush_group();
            }
            continue;
        }
        if (c == ' ') { if (started_) ++spaces_; continue; }
        if (c == '[') { group_ = "["; continue; }
        if (!started_ && is_ws(c)) continue;
        if (c == '.' || c == ',') spaces_ = 0;  // no space before these
        if (spaces_) pending_ += ' ';
        spaces_ = 0;
        pending_ += c;
        started_ = true;
    }
}

void AnswerStream::release(bool final) {
    if (cancelled_ || held_) return;
    size_t cut = 0;  // release pending_[0, cut)
    if (final) {
        cut = pending_.size();
        while (cut > 0 && is_ws(pending_[cut - 1])) --cut;
    } else {
        // The last sentence end: an end mark followed by whitespace, or a line end. The whitespace stays pending,
        // so nothing trailing is ever released.
        for (size_t p = pending_.size(); p-- > 1;) {
            if (!is_ws(pending_[p])) continue;
            char prev = pending_[p - 1];
            bool end = pending_[p] == '\n' || prev == '.' || prev == '!' || prev == '?' || prev == ':' ||
                       (p >= 3 && pending_.compare(p - 3, 3, "\xE0\xA5\xA4") == 0);  // ।
            if (!end) continue;
            cut = p;
            while (cut > 0 && is_ws(pending_[cut - 1])) --cut;
            break;
        }
    }
    if (cut == 0) return;
    std::string chunk = pending_.substr(0, cut);
    if (mode_ == SkipMedicine) {
        // Sentence by sentence (each with the whitespace before it): one that mentions a medicine is left out.
        pending_.erase(0, cut);
        std::string kept;
        size_t a = 0;
        for (size_t p = 1; p <= chunk.size(); ++p) {
            bool edge = p == chunk.size();
            if (!edge && is_ws(chunk[p]) && !is_ws(chunk[p - 1])) {
                char prev = chunk[p - 1];
                edge = chunk[p] == '\n' || prev == '.' || prev == '!' || prev == '?' || prev == ':' ||
                       (p >= 3 && chunk.compare(p - 3, 3, "\xE0\xA5\xA4") == 0);
            }
            if (!edge) continue;
            std::string seg = chunk.substr(a, p - a);
            if (!health_text(seg, names_)) kept += seg;
            a = p;
        }
        if (shown_.empty()) {
            size_t st = 0;
            while (st < kept.size() && is_ws(kept[st])) ++st;
            kept.erase(0, st);
        }
        if (kept.empty()) return;
        shown_ += kept;
        if (!sink_(kept)) cancelled_ = true;
        return;
    }
    std::string upto = shown_ + chunk;
    if (upto.find("NOT_FOUND") != std::string::npos || health_text(upto, names_)) {
        held_ = true;  // answered in code instead; nothing more is shown
        pending_.clear();
        return;
    }
    pending_.erase(0, cut);
    shown_ += chunk;
    if (!sink_(chunk)) cancelled_ = true;
}

bool AnswerStream::feed(const std::string &raw) {
    if (cancelled_) return false;
    cite(think_.feed(raw));
    release(false);
    return !cancelled_;
}

bool AnswerStream::finish() {
    if (cancelled_) return false;
    cite(think_.finish());
    flush_group();  // an unclosed "[" is text (apply_citations keeps it)
    spaces_ = 0;
    release(true);
    return !cancelled_;
}

// ---------------------------------------------------------------- N10

namespace {

std::string lower_ascii_copy(std::string s) {
    for (auto &c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return s;
}

std::vector<std::string> ascii_words(const std::string &text) {
    std::vector<std::string> out;
    std::string cur;
    for (unsigned char c : lower_ascii_copy(text)) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) cur += (char)c;
        else if (!cur.empty()) { out.push_back(cur); cur.clear(); }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool has_word(const std::vector<std::string> &w, const char *x) { return std::find(w.begin(), w.end(), x) != w.end(); }
bool has_prefix(const std::vector<std::string> &w, const char *x) {
    size_t n = strlen(x);
    for (const auto &t : w) if (t.compare(0, n, x) == 0) return true;
    return false;
}
bool has_phrase(const std::vector<std::string> &w, std::initializer_list<const char *> ph) {
    std::vector<const char *> p(ph);
    for (size_t i = 0; i + p.size() <= w.size(); ++i) {
        bool ok = true;
        for (size_t k = 0; k < p.size() && ok; ++k) ok = w[i + k] == p[k];
        if (ok) return true;
    }
    return false;
}
// The Telugu word on its own: not a stem of a longer word (Telugu letters count as word bytes).
bool telugu_word(const std::string &text, const char *word) {
    const size_t n = strlen(word);
    for (size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1))
        if ((at == 0 || (unsigned char)text[at - 1] < 0x80) && (at + n == text.size() || (unsigned char)text[at + n] < 0x80)) return true;
    return false;
}

bool has_any(const std::string &text, std::initializer_list<const char *> subs) {
    for (const char *x : subs) if (text.find(x) != std::string::npos) return true;
    return false;
}

int64_t day_start(int64_t t, int add_days) {
    time_t tt = (time_t)t;
    struct tm lt;
    localtime_r(&tt, &lt);
    lt.tm_hour = lt.tm_min = lt.tm_sec = 0;
    lt.tm_mday += add_days;
    lt.tm_isdst = -1;
    return (int64_t)mktime(&lt);
}
int weekday_from_monday(int64_t t) {
    time_t tt = (time_t)t;
    struct tm lt;
    localtime_r(&tt, &lt);
    return (lt.tm_wday + 6) % 7;
}
int64_t month_start(int64_t t, int add_months) {
    time_t tt = (time_t)t;
    struct tm lt;
    localtime_r(&tt, &lt);
    lt.tm_hour = lt.tm_min = lt.tm_sec = 0;
    lt.tm_mday = 1;
    lt.tm_mon += add_months;
    lt.tm_isdst = -1;
    return (int64_t)mktime(&lt);
}

std::string ymd_of(int64_t t) {
    time_t tt = (time_t)t;
    struct tm lt;
    localtime_r(&tt, &lt);
    char b[16];
    strftime(b, sizeof b, "%Y-%m-%d", &lt);
    return b;
}

// Days since 1970-01-01 of a civil date, and back (H. Hinnant's algorithms).
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
std::string civil_from_days(int64_t z) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    char b[16];
    snprintf(b, sizeof b, "%04lld-%02u-%02u", (long long)y, m, d);
    return b;
}

std::string strip_lead(std::string line) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '*' || line[i] == '#' || line[i] == '-' ||
                               line[i] == '>' || line[i] == '_'))
        ++i;
    return line.substr(i);
}

// "Q12. rest", "Question 2: rest", "ప్ర1. rest" (or A, Answer, జ, జవాబు) -> rest; nullopt when the line is not one.
std::optional<std::string> tagged(const std::string &line, bool question) {
    static const std::vector<std::string> kQ = {"question", "q", "ప్రశ్న", "ప్ర"}, kA = {"answer", "a", "జవాబు", "సమాధానం", "జ"};
    const std::string low = lower_ascii_copy(line);
    for (const auto &tag : question ? kQ : kA) {
        if (low.compare(0, tag.size(), tag) != 0) continue;
        size_t i = tag.size(), start;
        while (i < line.size() && line[i] == ' ') ++i;
        start = i;
        while (i < line.size() && line[i] >= '0' && line[i] <= '9') ++i;
        if (i == start || i >= line.size() || (line[i] != '.' && line[i] != ')' && line[i] != ':')) continue;
        std::string rest = trim_copy(strip_lead(line.substr(i + 1)));
        while (!rest.empty() && (rest.back() == '*' || rest.back() == ' ')) rest.pop_back();
        return rest;
    }
    return std::nullopt;
}

std::string mmss_label(int64_t sec) {
    if (sec < 0) sec = 0;
    char b[32];
    snprintf(b, sizeof b, "%02lld:%02lld", (long long)(sec / 60), (long long)(sec % 60));
    return b;
}

}  // namespace

bool has_subject(const std::string &text) {
    static const std::set<std::string> kGeneric = {
        "today", "tonight", "yesterday", "tomorrow", "week", "weeks", "month", "months", "last", "next", "past", "recent",
        "key", "discussion", "discussions", "discuss", "discussed", "highlight", "highlights", "important", "point", "points",
        "summarise", "summarize", "summary", "summaries", "summarised", "summarized", "recap", "happen", "happened", "happening",
        "talk", "talked", "talking", "meeting", "meetings", "recording", "recordings", "note", "notes", "remember", "coming", "up",
        "pending", "list", "soon", "give", "show", "get", "need", "anything", "due", "remind", "reminds", "reminder", "reminders", "todo", "todos",
        "deadline", "deadlines", "గడువు",
        "ఈ", "రోజు", "ఈరోజు", "నేడు", "నిన్న", "వారం", "గత", "పోయిన", "నెల", "ఏం", "జరిగింది", "మాట్లాడాం", "చర్చలు", "చెప్పారు", "చెప్పండి",
        "రికార్డింగ్‌ల", "రికార్డింగ్ల", "రికార్డింగ్‌లు", "రికార్డింగ్లు", "త్వరలో", "ఏమేమి", "ఉంది", "ముఖ్యమైన", "ముఖ్యాంశాలు", "సారాంశం", "చేయాల్సి", "గుర్తు", "చేయండి",
    };
    std::string cur;
    const std::string kq = keyword_query(text) + " ";
    for (char c : kq) {
        if (c != ' ') { cur += c; continue; }
        if (!cur.empty() && !kGeneric.count(cur)) return true;
        cur.clear();
    }
    return false;
}

Route route_question(const std::string &text, int64_t now, const std::vector<std::string> &medicine_names) {
    Route r;
    if (is_medicine_question(text, medicine_names)) { r.kind = "medicine"; r.by_rule = true; return r; }
    const std::vector<std::string> w = ascii_words(text);
    const std::string low = lower_ascii_copy(text);
    const bool subject = has_subject(text);
    const bool starts_quiz = !w.empty() && w[0] == "quiz" && low.find_first_not_of(" \t\n") == low.find("quiz") &&
                             (w.size() == 1 || w[1] == "on" || w[1] == "about" || w[1] == "for");
    if (has_phrase(w, {"quiz", "me"}) || has_phrase(w, {"test", "me"}) || has_phrase(w, {"next", "question"}) || starts_quiz ||
        has_any(text, {"క్విజ్", "తర్వాతి ప్రశ్న", "మరో ప్రశ్న"}) ||
        (has_any(text, {"పరీక్ష"}) && (telugu_word(text, "పెట్టండి") || telugu_word(text, "పెట్టు") ||
                                       (has_any(text, {"నన్ను"}) && (telugu_word(text, "చేయండి") || telugu_word(text, "చేయి")))))) {
        r.kind = "quiz"; r.by_rule = true; return r;
    }
    bool due_word = false;
    for (size_t i = 0; i < w.size(); ++i) due_word = due_word || (w[i] == "due" && !(i + 1 < w.size() && w[i + 1] == "to"));
    if (has_word(w, "todo") || has_word(w, "todos") || has_any(low, {"to-do", "to do list"}) ||
        (!subject && (due_word || has_prefix(w, "deadline") || has_any(text, {"గడువు"}) || has_phrase(w, {"what", "do", "i", "have"}) ||
                      has_prefix(w, "remind") || has_any(text, {"చేయాల్సి", "గుర్తు చేయ"})))) {
        r.kind = "due"; r.by_rule = true; return r;
    }
    const int64_t today = day_start(now, 0);
    bool named = true;
    if (has_word(w, "yesterday") || has_any(text, {"నిన్న"})) { r.from = day_start(now, -1); r.to = today; }
    else if (has_word(w, "today") || has_word(w, "tonight") || has_any(text, {"ఈ రోజు", "ఈరోజు", "నేడు"})) { r.from = today; r.to = day_start(now, 1); }
    else if (has_phrase(w, {"last", "week"}) || has_any(text, {"గత వారం", "పోయిన వారం"})) {
        int64_t monday = day_start(now, -weekday_from_monday(now));
        r.from = day_start(monday, -7); r.to = monday;
    } else if (has_phrase(w, {"this", "week"}) || has_any(text, {"ఈ వారం"})) {
        int64_t monday = day_start(now, -weekday_from_monday(now));
        r.from = monday; r.to = day_start(monday, 7);
    } else if (has_phrase(w, {"this", "month"}) || has_any(text, {"ఈ నెల"})) { r.from = month_start(now, 0); r.to = month_start(now, 1); }
    else named = false;
    const bool summary_word = has_phrase(w, {"key", "discussions"}) || has_phrase(w, {"key", "discussion"}) || has_phrase(w, {"key", "points"}) ||
        has_prefix(w, "highlight") || has_phrase(w, {"important", "points"}) || has_prefix(w, "summar") ||
        has_prefix(w, "recap") || has_any(text, {"సారాంశ", "ముఖ్యాంశ", "ముఖ్యమైన"});
    // A period alone in small talk or a question of another shape is not asking for a summary.
    bool asks_the_date = false;   // "What is today?", not "What is happening today?"
    for (size_t i = 0; i + 2 < w.size(); ++i)
        asks_the_date = asks_the_date || (w[i] == "what" && (w[i + 1] == "is" || w[i + 1] == "s") &&
                                          (w[i + 2] == "today" || w[i + 2] == "tomorrow" || w[i + 2] == "yesterday" || w[i + 2] == "this" || w[i + 2] == "last"));
    const bool advice = (has_phrase(w, {"should", "i"}) || has_phrase(w, {"can", "i"}) || has_phrase(w, {"how", "do", "i"})) &&
                        !has_word(w, "remember") && !has_word(w, "know") && !has_word(w, "review") && !has_word(w, "revise");
    const bool other_shape = !summary_word && (has_phrase(w, {"how", "are", "you"}) || asks_the_date || advice);
    if ((named || summary_word) && !subject && !other_shape) {
        r.kind = "summary";
        r.by_rule = true;
        if (!named) { r.from = day_start(now, -6); r.to = day_start(now, 1); }
        return r;
    }
    r.kind = "recordings";   // with [from, to) when a period was named
    return r;
}

bool health_text(const std::string &text, const std::vector<std::string> &medicine_names) {
    return health_keywords(text, medicine_names) == HealthFlag::health;
}

bool chat_health_question(const std::string &question, const std::vector<ChatTurn> &earlier,
                          const std::vector<std::string> &medicine_names) {
    if (health_gate(question, medicine_names) || (!earlier.empty() && (earlier.back().medicine || earlier.back().health))) return true;
    for (size_t k = earlier.size(); k-- > 0;)
        if (earlier[k].role == "user") return health_gate(earlier[k].text, medicine_names);
    return false;
}

std::vector<ChatTurn> history_without_health(const std::vector<ChatTurn> &earlier, const std::vector<std::string> &medicine_names) {
    const auto bad_answer = [&](const ChatTurn &a) {
        return a.medicine || a.health || a.text == health_only_text("en") || a.text == health_only_text("te") ||
               health_text(a.text, medicine_names) || health_gate(a.text, medicine_names);
    };
    std::vector<ChatTurn> out;
    for (size_t i = 0; i < earlier.size(); ++i) {
        const ChatTurn &t = earlier[i];
        if (t.role == "user" && i + 1 < earlier.size() && earlier[i + 1].role == "assistant") {
            if (!health_gate(t.text, medicine_names) && !bad_answer(earlier[i + 1])) {
                out.push_back(t);
                out.push_back(earlier[i + 1]);
            }
            ++i;
        } else if (t.role == "user" ? !health_gate(t.text, medicine_names) : !bad_answer(t)) {
            out.push_back(t);
        }
    }
    return out;
}

std::string health_only_text(const std::string &lang) {
    return lang == "te"
        ? "ఆరోగ్యం, మందుల ప్రశ్నలకు మీరు సేవ్ చేసిన మందులు, రికార్డింగ్‌ల నుండి మాత్రమే సమాధానం ఇవ్వగలను. దయచేసి డాక్టర్‌ను లేదా ఫార్మసిస్ట్‌ను అడగండి."
        : "I can only answer health and medicine questions from your saved medicines and recordings. Please ask a doctor or pharmacist.";
}

std::string drop_health_sentences(const std::string &text, const std::vector<std::string> &medicine_names) {
    std::string kept;
    size_t a = 0;
    for (size_t p = 1; p <= text.size(); ++p) {
        bool edge = p == text.size();
        if (!edge && is_ws(text[p]) && !is_ws(text[p - 1])) {
            char prev = text[p - 1];
            edge = text[p] == '\n' || prev == '.' || prev == '!' || prev == '?' || prev == ':' ||
                   (p >= 3 && text.compare(p - 3, 3, "\xE0\xA5\xA4") == 0);
        }
        if (!edge) continue;
        std::string seg = text.substr(a, p - a);
        if (!health_text(seg, medicine_names)) kept += seg;
        a = p;
    }
    return trim_copy(kept);
}

bool health_passage_scores(const std::string &keywords, const std::string &text, const std::vector<std::string> &medicine_names) {
    std::vector<std::string> all, must;
    std::string cur;
    for (char c : keywords + " ") {
        if (c != ' ') { cur += c; continue; }
        if (!cur.empty()) {
            all.push_back(cur);
            if (health_gate(cur, medicine_names)) must.push_back(cur);
        }
        cur.clear();
    }
    if (must.empty()) must = all;
    if (must.empty()) return false;
    for (const auto &k : must) if (keyword_hits(k, text) == 0) return false;
    return true;
}

void drop_source(Relevant &rel, const std::string &label) {
    const std::string tag = "[" + label + "] ";
    size_t a = rel.block.rfind(tag, 0) == 0 ? 0 : rel.block.find("\n" + tag);
    if (a != std::string::npos) {
        if (a != 0 || rel.block[0] == '\n') ++a;
        // The line ends where the next labelled line or the next heading starts.
        size_t e = std::string::npos;
        for (const char *next : {"\n[S", "\nRemembered facts:\n", "\nSummaries of recordings:\n"}) e = std::min(e, rel.block.find(next, a));
        e = e == std::string::npos ? rel.block.size() : e + 1;
        rel.block.erase(a, e - a);
    }
    if (rel.sources.is_array()) {
        json kept = json::array();
        for (const auto &s : rel.sources) if (jstr(s, "label") != label) kept.push_back(s);
        rel.sources = kept;
    }
}

void neutral_titles(Relevant &rel, const std::vector<std::string> &medicine_names) {
    if (!rel.sources.is_array()) return;
    for (const auto &s : rel.sources) {
        const std::string title = jstr(s, "title");
        if (title.empty() || !health_gate(title, medicine_names)) continue;
        const std::string head = "[" + jstr(s, "label") + "] " + title + " ";
        size_t at = rel.block.find(head);
        if (at != std::string::npos) rel.block.replace(at, head.size(), "[" + jstr(s, "label") + "] Recording ");
    }
}

void drop_flagged_facts(Relevant &rel, const std::vector<std::string> &medicine_names) {
    const std::string head = "Remembered facts:\n";
    size_t at = rel.block.find(head);
    if (at == std::string::npos) return;
    size_t i = at + head.size();
    while (i < rel.block.size()) {
        size_t e = rel.block.find('\n', i);
        e = e == std::string::npos ? rel.block.size() : e + 1;
        const std::string line = rel.block.substr(i, e - i);
        if (line.rfind("- ", 0) != 0) break;
        if (health_text(line.substr(2), medicine_names)) rel.block.erase(i, e - i);
        else i = e;
    }
}

char classify_route(const std::string &reply) {
    ThinkStripper t;
    std::string v = t.feed(reply);
    v += t.finish();
    v = trim_copy(v);
    if (v.empty()) return 'R';
    const char c = v[0] >= 'a' && v[0] <= 'z' ? (char)(v[0] - 32) : v[0];
    if (c != 'R' && c != 'G' && c != 'H') return 'R';
    for (size_t i = 1; i < v.size(); ++i) {   // one letter, nothing else but punctuation: "G." yes, "Great" no
        const unsigned char x = (unsigned char)v[i];
        if (x >= 0x80 || (x >= '0' && x <= '9') || (x >= 'a' && x <= 'z') || (x >= 'A' && x <= 'Z')) return 'R';
    }
    return c;
}

std::string classify_prompt(const std::string &question) {
    return "Sort this question.\nR: about the person's own recordings, notes, meetings, people, plans or life.\n"
           "H: about health, illness, medicine, diet for an illness, the body, pregnancy or first aid.\n"
           "G: other general knowledge that their recordings would not answer.\n\nQuestion: " + question +
           "\n\nReply with the single letter R, H or G.";
}

std::vector<std::pair<std::string, std::string>> parse_revision_questions(const std::string &text) {
    std::vector<std::pair<std::string, std::string>> out;
    std::string q, a;
    bool in_q = false, in_a = false;
    auto push = [&] {
        if (in_q && !trim_copy(a).empty()) out.push_back({trim_copy(q), trim_copy(a)});
        q.clear(); a.clear(); in_q = in_a = false;
    };
    size_t i = 0;
    while (i <= text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string::npos) e = text.size();
        std::string line = trim_copy(text.substr(i, e - i));
        i = e + 1;
        std::string body = strip_lead(line);
        if (auto t = tagged(body, true)) { push(); q = *t; in_q = true; }
        else if (auto t2 = tagged(body, false)) { if (in_q) { a = *t2; in_a = true; } }
        else if (!line.empty() && in_a) a += " " + line;
        if (e == text.size()) break;
    }
    push();
    return out;
}

namespace {
template <class F>
void each_quiz(const json &notes, const std::vector<std::string> &names, F f) {
    if (!notes.is_array()) return;
    for (const auto &n : notes) {
        if (!n.is_object()) continue;
        int64_t id = jint(n, "id", 0);
        auto qa = parse_revision_questions(jstr(n, "revision_questions"));
        for (size_t k = 0; k < qa.size(); ++k) {
            if (health_text(qa[k].first + "\n" + qa[k].second, names)) continue;
            QuizItem it;
            it.id = std::to_string(id) + ":" + std::to_string(k + 1);
            it.question = strip_unclear(qa[k].first);
            it.answer = strip_unclear(qa[k].second);
            it.note_id = id;
            it.title = jstr(n, "title");
            if (f(it)) return;
        }
    }
}
}  // namespace

std::optional<QuizItem> pick_quiz(const json &notes, const std::vector<std::string> &asked,
                                  const std::vector<std::string> &medicine_names) {
    std::optional<QuizItem> got;
    each_quiz(notes, medicine_names, [&](const QuizItem &it) {
        if (std::find(asked.begin(), asked.end(), it.id) != asked.end()) return false;
        got = it;
        return true;
    });
    return got;
}

size_t quiz_count(const json &notes, const std::vector<std::string> &medicine_names) {
    size_t n = 0;
    each_quiz(notes, medicine_names, [&](const QuizItem &) { ++n; return false; });
    return n;
}

std::string ymd_add_days(const std::string &ymd, int days) {
    int y = 0;
    unsigned m = 0, d = 0;
    if (sscanf(ymd.c_str(), "%d-%u-%u", &y, &m, &d) != 3) return ymd;
    return civil_from_days(days_from_civil(y, m, d) + days);
}

std::string due_answer(const json &reminders, const json &tasks, const std::string &today, const std::string &lang,
                       json &sources) {
    const bool te = lang == "te";
    const std::string last = ymd_add_days(today, 6);  // today and the 6 days after it
    auto add_source = [&](const json &row) {
        int64_t nid = jint(row, "note_id", 0);
        if (nid <= 0 || sources.size() >= 5) return;
        int64_t sec = jint(row, "start_sec", 0);
        for (const auto &s : sources) if (jint(s, "note_id", 0) == nid && jint(s, "start_sec", 0) == sec) return;
        sources.push_back({{"note_id", nid}, {"start_sec", sec}, {"title", ""}});
    };
    std::vector<std::pair<std::string, std::string>> due;  // (date, line)
    if (tasks.is_array())
        for (const auto &t : tasks) {
            std::string dd = jstr(t, "due_date"), text = trim_copy(jstr(t, "text")), who = jstr(t, "who");
            if (dd.size() != 10 || text.empty() || dd > last) continue;
            std::string when = dd < today ? (te ? "గడువు దాటింది (" + dd + ")" : "overdue (was due " + dd + ")")
                               : dd == today ? (te ? "ఈ రోజు" : "today")
                                             : (te ? "గడువు " + dd : "due " + dd);
            due.push_back({dd, "\xE2\x80\xA2 " + text + (who.empty() ? "" : " (" + who + ")") + " \xE2\x80\x94 " + when});
            add_source(t);
        }
    std::stable_sort(due.begin(), due.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    std::vector<std::string> rings;
    if (reminders.is_array())
        for (const auto &r : reminders) {
            std::string text = trim_copy(jstr(r, "text")), rep = jstr(r, "repeat"), start = jstr(r, "start_date"),
                        until = jstr(r, "until_date");
            if (text.empty() || (!start.empty() && start > last) || (!until.empty() && until < today)) continue;
            std::string word;
            if (rep == "once") {
                if (start.empty() || start < today) continue;
                word = te ? start + "\xE0\xB0\xA8" : "on " + start;  // న
            } else if (rep == "daily") word = te ? "ప్రతిరోజు" : "daily";
            else if (rep == "twice_daily") word = te ? "రోజుకు రెండుసార్లు" : "twice a day";
            else if (rep == "weekly") word = te ? "ప్రతి వారం" : "weekly";
            else continue;
            std::string times;
            if (r.contains("times") && r["times"].is_array())
                for (const auto &x : r["times"]) if (x.is_string()) times += (times.empty() ? "" : ", ") + x.get<std::string>();
            rings.push_back("\xE2\x80\xA2 " + text + " \xE2\x80\x94 " + (times.empty() ? "" : times + ", ") + word);
            add_source(r);
        }
    std::string out;
    if (!due.empty()) {
        out += te ? "రాబోయే 7 రోజుల్లో మీరు చేయాల్సినవి:" : "Here's what's due in the next 7 days:";
        for (const auto &d : due) out += "\n" + d.second;
    }
    if (!rings.empty()) {
        out += std::string(out.empty() ? "" : "\n\n") + (te ? "రాబోయే 7 రోజుల్లో రిమైండర్‌లు:" : "Reminders in the next 7 days:");
        for (const auto &l : rings) out += "\n" + l;
    }
    if (out.empty()) out = te ? "రాబోయే 7 రోజుల్లో చేయాల్సింది ఏమీ లేదు. మీరు ఫ్రీ!" : "Nothing is due in the next 7 days. You're all clear.";
    return out;
}

std::string strip_unclear(const std::string &text) {
    std::string s = text;
    for (size_t a; (a = s.find("[unclear]")) != std::string::npos;) {
        size_t n = 9;
        if (a + n < s.size() && s[a + n] == ' ') ++n;
        s.erase(a, n);
    }
    return trim_copy(s);
}

std::string summary_material(const json &notes, const std::vector<std::string> &medicine_names, bool *any_content) {
    std::string out;
    bool any = false;
    if (notes.is_array())
        for (const auto &n : notes) {
            if (!n.is_object()) continue;
            std::string title = jstr(n, "title");
            if (health_gate(title, medicine_names)) title.clear();
            out += "### " + (title.empty() ? std::string() : title + " ") + "(" + ymd_of(jint(n, "created_at", 0)) + ")\n";
            std::string body = drop_health_sentences(strip_unclear(jstr(n, "short_summary")), medicine_names);
            if (body.empty() && n.contains("sections") && n["sections"].is_array())
                for (const auto &sec : n["sections"]) {
                    json sum = sec.is_object() && sec.contains("summary") ? sec["summary"] : json();
                    if (!sum.is_object()) continue;
                    std::string line = strip_unclear(jstr(sum, "title"));
                    if (health_text(line, medicine_names)) line.clear();
                    if (sum.contains("points") && sum["points"].is_array())
                        for (const auto &p : sum["points"]) {
                            std::string t = strip_unclear(p.is_object() ? jstr(p, "text") : p.is_string() ? p.get<std::string>() : "");
                            if (!t.empty() && !health_text(t, medicine_names)) line += (line.empty() ? "" : " ") + t;
                        }
                    if (!line.empty()) body += "- " + line + "\n";
                }
            if (!body.empty()) any = true;
            out += (body.empty() ? std::string("(no summary yet)") : trim_copy(body)) + "\n\n";
        }
    if (any_content) *any_content = any;
    return out;
}

std::string summary_prompt(const std::string &history, const std::string &material, const std::string &period,
                           const std::string &question, const std::string &lang_name) {
    std::string p;
    if (!history.empty()) p += history + "(The conversation is only context for the request.)\n\n";
    p += "Below are the stored summaries of the person's recordings from " + period + ".\n\n" + material +
         "Request: " + question + "\n\n"
         "Write the key points to remember from these recordings, as a short list: one line per point, each starting "
         "with \"- \" and the recording's title in brackets. Write in " + lang_name + ", even when the summaries are in "
         "another language. Use only these summaries; do not invent anything. Leave out medicines, doses and health "
         "advice.";
    return p;
}

std::string general_prompt(const std::string &history, const std::string &question, const std::string &lang_name) {
    std::string p;
    if (!history.empty()) p += history + "\n";
    p += "Question: " + question + "\n\n"
         "Answer from your own general knowledge, clearly and briefly, in " + lang_name + ". This answer is not from the "
         "person's recordings. Do not give medical advice, medicine names, doses or diagnoses; for a health question, "
         "say to ask a doctor.";
    return p;
}

int keyword_hits(const std::string &keywords, const std::string &text) {
    const std::string low = lower_ascii_copy(text);
    auto is_ascii_word = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    auto found = [&](const std::string &k) {
        bool ascii = true;
        for (unsigned char c : k) ascii = ascii && c < 0x80;
        for (size_t at = low.find(k); at != std::string::npos; at = low.find(k, at + 1))
            if (!ascii) return true;
            else if ((at == 0 || !is_ascii_word((unsigned char)low[at - 1])) &&
                     (k.size() > 3 || at + k.size() == low.size() || !is_ascii_word((unsigned char)low[at + k.size()])))
                return true;
        return false;
    };
    std::vector<std::string> seen;
    int n = 0;
    std::string cur;
    auto flush = [&] {
        if (!cur.empty() && std::find(seen.begin(), seen.end(), cur) == seen.end()) {
            seen.push_back(cur);
            if (found(cur)) ++n;
        }
        cur.clear();
    };
    for (char c : lower_ascii_copy(keywords)) { if (c == ' ') flush(); else cur += c; }
    flush();
    return n;
}

bool passage_scores(int hits, const std::string &keywords) {
    size_t words = words_in(keywords);
    if (words == 0 || hits <= 0) return false;
    return (size_t)hits * 2 >= words && (words < 2 || hits >= 2);
}

int add_passages(Relevant &rel, std::vector<Passage> candidates, const std::string &keywords,
                 const std::vector<std::string> &medicine_names, size_t max_bytes, size_t max_n) {
    std::vector<std::pair<int, Passage>> scored;
    for (auto &c : candidates) {
        int h = keyword_hits(keywords, c.title + "\n" + c.text);
        if (passage_scores(h, keywords) && !health_text(c.title + "\n" + c.text, medicine_names))
            scored.push_back({h, std::move(c)});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    if (!rel.sources.is_array()) rel.sources = json::array();
    int added = 0;
    for (const auto &sp : scored) {
        if ((size_t)added >= max_n) break;
        const Passage &p = sp.second;
        int high = 0;
        for (const auto &s : rel.sources) {
            const std::string l = jstr(s, "label");
            if (l.size() > 1) high = std::max(high, atoi(l.c_str() + 1));
        }
        std::string label = "S" + std::to_string(high + 1);
        const std::string shown_title = health_gate(p.title, medicine_names) ? "Recording" : p.title;
        std::string line = "[" + label + "] " + shown_title + " " + mmss_label(p.start_sec) + ": " + p.text + "\n";
        std::string add = (added == 0 ? std::string(rel.block.empty() ? "" : "\n") + "Summaries of recordings:\n" : "") + line;
        if (rel.block.size() + add.size() > max_bytes) break;
        rel.block += add;
        rel.sources.push_back({{"label", label}, {"note_id", p.note_id}, {"start_sec", p.start_sec}, {"title", p.title}});
        ++added;
    }
    return added;
}

}  // namespace sve
