#include "sve_chat.h"
#include "sve_context.h"  // truncate_utf8
#include "sve_suggest.h"  // keyword_query, mentions_medicine

#include <cstdio>
#include <cstring>

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
        if (role == "assistant") {
            auto med = m.find("medicine");
            if (med != m.end() && med->is_boolean() && med->get<bool>()) {
                out.push_back({role, "(A medicine answer, quoted from the saved facts and recordings.)"});
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
        out.push_back({role, text});
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
    std::string kq = keyword_query(question);
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

AnswerStream::AnswerStream(std::vector<std::string> medicine_names, std::function<bool(const std::string &)> sink)
    : names_(std::move(medicine_names)), sink_(std::move(sink)) {}

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
    std::string upto = shown_ + chunk;
    if (upto.find("NOT_FOUND") != std::string::npos || mentions_medicine(upto, names_)) {
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

}  // namespace sve
