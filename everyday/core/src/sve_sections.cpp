#include "sve_sections.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using nlohmann::json;

namespace sve {

std::vector<std::pair<size_t, size_t>> plan_sections(const std::vector<PieceRef> &p, bool is_final,
                                                     int target, int min_len, int max_len) {
    std::vector<std::pair<size_t, size_t>> out;
    const size_t n = p.size();
    size_t first = 0;
    while (first < n) {
        size_t k = first;
        bool closed = false;
        for (; k < n; ++k) {
            int64_t len = p[k].end_sec - p[first].start_sec;
            bool next_overflows = k + 1 < n && p[k + 1].end_sec - p[first].start_sec > max_len;
            if (len >= target || (next_overflows && len >= min_len)) { closed = true; break; }
        }
        if (!closed) {  // ran out of pieces: the tail
            if (!is_final) break;  // more audio may still arrive; the tail has not reached the target
            out.push_back({first, n});
            break;
        }
        out.push_back({first, k + 1});
        first = k + 1;
    }
    return out;
}

static int64_t sane_sec(const json &e) {
    if (!e.is_object() || !e.contains("start_sec")) return 0;
    const json &v = e["start_sec"];
    if (v.is_number_integer()) return std::max<int64_t>(0, v.get<int64_t>());
    if (v.is_number_float()) {
        double d = v.get<double>();
        if (!(d > 0)) return 0;  // negative or NaN
        return d >= 9.0e15 ? (int64_t)9.0e15 : (int64_t)d;
    }
    return 0;
}

std::string plan_sections_json(const std::string &pieces_json, int64_t end_sec_of_last, bool is_final) {
    json arr = json::parse(pieces_json, nullptr, false);
    json out = json::array();
    if (!arr.is_array() || arr.empty()) return out.dump();
    std::vector<int64_t> starts;
    starts.reserve(arr.size());
    for (const auto &e : arr) {
        int64_t s = sane_sec(e);
        if (!starts.empty()) s = std::max(s, starts.back());
        starts.push_back(s);
    }
    std::vector<PieceRef> refs;
    refs.reserve(starts.size());
    for (size_t i = 0; i < starts.size(); ++i)
        refs.push_back({starts[i], i + 1 < starts.size() ? starts[i + 1] : std::max(end_sec_of_last, starts[i])});
    for (const auto &r : plan_sections(refs, is_final)) out.push_back(json::array({r.first, r.second}));
    return out.dump();
}

std::string section_prompt(const SectionPieces &pieces, const std::string &lang, const std::string &vocab_line) {
    const bool te = lang == "te";
    std::string p = "Summarize this part of a recording. Write the summary in ";
    p += te ? "Telugu" : "English";
    p += ". Keep names, places and technical terms in the script they were spoken in. Reply with one JSON object "
         "and nothing else, in this shape:\n"
         "{\"title\":\"short title, at most 8 words\",\"topic\":\"one line on what this part is about\","
         "\"points\":[{\"text\":\"one key point\",\"start_sec\":0}],\"terms\":[\"important names or technical terms\"],"
         "\"hints\":[\"things worth following up, if any\"]}\n"
         "Rules:\n- 3 to 6 points, each one sentence. Draw points from the beginning, middle and end of this part, "
         "not only its start.\n"
         "- Lines marked [unclear] were hard to hear: do not state anything from them as fact; skip them or say the "
         "passage was unclear.\n"
         "- Each point's \"start_sec\" is copied from the [t=SEC] marker of the transcript line it came from.\n"
         "- Use only what was said. Use an empty array when nothing applies.\n"
         "- Never expand an abbreviation the speaker did not expand. Leave out advertisements and sponsor messages.\n";
    if (!vocab_line.empty()) p += vocab_line + "\n";
    p += "\nTranscript (each line starts with its [t=SEC] marker):\n";
    for (const auto &pc : pieces) p += "[t=" + std::to_string(pc.first) + "] " + pc.second + "\n";
    p += "\nJSON:\n";
    return p;
}

// ---- N7 rev 1: unclear pieces, vocabulary said or not ----

const char *const kUnclearTag = "[unclear] ";

static std::string ascii_lower(std::string s) {
    for (char &c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return s;
}

bool piece_unclear(double conf) { return std::isfinite(conf) && conf < kUnclearConf; }

static bool blank(const std::string &t) { return t.find_first_not_of(" \t\r\n") == std::string::npos; }

std::string unclear_marked(const std::string &text, double conf) {
    return piece_unclear(conf) && !blank(text) ? kUnclearTag + text : text;
}

double piece_conf(const json &e) {
    if (e.is_object()) {
        auto it = e.find("conf");
        if (it != e.end() && it->is_number()) return it->get<double>();
    }
    return std::nan("");
}

static std::string trim_ws(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string notes_material(const json &pieces) {
    if (!pieces.is_array()) return "";
    std::string out;
    bool first = true;
    for (const auto &e : pieces) {
        if (!e.is_object() || !e.contains("text") || !e["text"].is_string()) continue;
        if (!first) out += "\n";
        out += unclear_marked(e["text"].get<std::string>(), piece_conf(e));
        first = false;
    }
    return trim_ws(out);
}

json section_pieces(const json &pieces, int64_t first, int64_t last) {
    json out = json::array();
    if (!pieces.is_array()) return out;
    for (const auto &p : pieces) {
        if (!p.is_object() || !p.contains("idx") || !p["idx"].is_number_integer()) continue;
        const int64_t i = p["idx"].get<int64_t>();
        if (i >= first && i <= last) out.push_back(p);
    }
    return out;
}

std::string strip_unclear_tags(const std::string &s) {
    static const std::string tag = "[unclear]";
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '[' && s.size() - i >= tag.size() && ascii_lower(s.substr(i, tag.size())) == tag) {
            i += tag.size();
            if (i < s.size() && s[i] == ' ') ++i;  // the tag's own space
            continue;
        }
        out += s[i++];
    }
    return out;
}

std::string said_text(const std::string &material) {
    std::string out;
    for (size_t a = 0; a <= material.size();) {
        size_t nl = material.find('\n', a);
        if (nl == std::string::npos) nl = material.size();
        const std::string line = material.substr(a, nl - a);
        if (line.rfind(kUnclearTag, 0) != 0) out += (out.empty() ? "" : "\n") + line;
        a = nl + 1;
    }
    return out;
}

SectionInput section_input(const json &arr) {
    SectionInput in;
    if (!arr.is_array()) return in;
    for (const auto &e : arr) {
        if (!e.is_object() || !e.contains("text") || !e["text"].is_string()) continue;
        const std::string t = e["text"].get<std::string>();
        if (blank(t)) continue;  // blank piece
        int64_t s = e.contains("start_sec") && e["start_sec"].is_number() ? e["start_sec"].get<int64_t>() : 0;
        in.plain.push_back({s, t});
        in.marked.push_back({s, unclear_marked(t, piece_conf(e))});
        in.starts.push_back(s);
        // The vocabulary check counts only what was heard clearly (fix round, item 3).
        if (!piece_unclear(piece_conf(e))) in.said += (in.said.empty() ? "" : "\n") + t;
    }
    return in;
}

// The code point starting at byte i (len set to its byte length); U+FFFD for a bad byte.
static char32_t cp_at(const std::string &s, size_t i, size_t &len) {
    const unsigned char c = (unsigned char)s[i];
    const int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (n == 0 || i + n > s.size()) { len = 1; return 0xFFFD; }
    char32_t cp = n == 1 ? c : c & (0xFF >> (n + 1));
    for (int k = 1; k < n; ++k) {
        const unsigned char d = (unsigned char)s[i + k];
        if ((d & 0xC0) != 0x80) { len = 1; return 0xFFFD; }
        cp = (cp << 6) | (d & 0x3F);
    }
    len = n;
    return cp;
}

// A letter or digit of a Latin-script word: ASCII alphanumerics and the Latin letters up to U+024F.
static bool latin_word_cp(char32_t c) {
    if (c < 0x80) return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    return c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7;
}

bool term_said(const std::string &term_in, const std::string &text) {
    const std::string term = ascii_lower(trim_ws(term_in));
    if (term.empty()) return false;
    const std::string hay = ascii_lower(text);
    bool latin = true;
    for (size_t i = 0, n = 0; i < term.size(); i += n)
        if (cp_at(term, i, n) > 0x24F) { latin = false; break; }
    if (!latin) return hay.find(term) != std::string::npos;
    for (size_t pos = hay.find(term); pos != std::string::npos; pos = hay.find(term, pos + 1)) {
        size_t n = 0;
        if (pos > 0) {  // the code point before the match
            size_t b = pos - 1;
            while (b > 0 && ((unsigned char)hay[b] & 0xC0) == 0x80) --b;
            if (latin_word_cp(cp_at(hay, b, n))) continue;
        }
        size_t end = pos + term.size();
        // A plural or possessive of the term counts: "IPOs", "IPO's", "IPO’s" (fix round, item 4).
        if (hay.compare(end, 1, "s") == 0) end += 1;
        else if (hay.compare(end, 2, "'s") == 0) end += 2;
        else if (hay.compare(end, 4, "\xE2\x80\x99s") == 0) end += 4;
        if (end < hay.size() && latin_word_cp(cp_at(hay, end, n))) continue;  // "IPOss", "IPOx": another word
        return true;
    }
    return false;
}

std::vector<std::string> terms_said(const std::vector<std::string> &vocab, const std::string &text) {
    std::vector<std::string> out;
    for (const auto &v : vocab) if (term_said(v, text)) out.push_back(v);
    return out;
}

int drop_unsaid_vocab(json &summary, const std::vector<std::string> &vocab, const std::string &said) {
    std::vector<std::string> unsaid;
    for (const auto &v : vocab) if (!blank(v) && !term_said(v, said)) unsaid.push_back(v);
    if (unsaid.empty() || !summary.is_object()) return 0;
    auto mentions = [&](const std::string &t) {
        for (const auto &u : unsaid) if (term_said(u, t)) return true;
        return false;
    };
    int dropped = 0;
    for (const char *key : {"terms", "hints"}) {
        auto it = summary.find(key);
        if (it == summary.end() || !it->is_array()) continue;
        json keep = json::array();
        for (const auto &e : *it) {
            if (e.is_string() && mentions(e.get<std::string>())) { ++dropped; continue; }
            keep.push_back(e);
        }
        *it = keep;
    }
    auto pit = summary.find("points");
    if (pit != summary.end() && pit->is_array()) {
        json keep = json::array();
        for (const auto &p : *pit) {
            if (p.is_object() && p.contains("text") && p["text"].is_string() && mentions(p["text"].get<std::string>())) {
                ++dropped;
                continue;
            }
            keep.push_back(p);
        }
        *pit = keep;
    }
    return dropped;
}

static std::string drop_thinking(std::string s) {
    for (auto tags : {std::make_pair(std::string("<think>"), std::string("</think>")),
                      std::make_pair(std::string("<|channel>"), std::string("<channel|>"))}) {
        for (;;) {
            size_t a = s.find(tags.first);
            if (a == std::string::npos) break;
            size_t b = s.find(tags.second, a);
            if (b == std::string::npos) { s.erase(a); break; }
            s.erase(a, b + tags.second.size() - a);
        }
    }
    return s;
}

const char *kMemoryShape =
    "{\"memories\":[{\"type\":\"fact|decision|promise|question|person|idea\",\"text\":\"…\",\"who\":\"\",\"to_whom\":\"\","
    "\"due_text\":\"\",\"quote\":\"…\",\"start_sec\":0,\"names\":[{\"as_said\":\"…\",\"as_written\":\"…\"}],"
    "\"numbers\":[],\"dates\":[],\"times\":[]}]}";

std::string memory_rules(const std::string &lang) {
    std::string r = "- At most 5 \"memories\", only things worth remembering later.\n- Each memory's \"text\" is one sentence in ";
    r += lang == "te" ? "Telugu" : "English";
    r += ".\n"
         "- Each memory's \"quote\" is copied word for word from one or two neighbouring transcript lines, in the script "
         "it was spoken in.\n"
         "- \"names\", \"numbers\", \"dates\" and \"times\" hold the exact words of the quote that the memory relies on. "
         "For each name, \"as_said\" is the quote's exact words and \"as_written\" is the form the text uses.\n"
         "- \"who\" and \"to_whom\" only when the transcript says them, else \"\".\n"
         "- \"due_text\" holds the exact words of a deadline, else \"\".\n"
         "- No memories about medicines or doses.\n";
    return r;
}

std::string memory_prompt(const SectionPieces &pieces, const std::string &lang, const std::string &vocab_line) {
    std::string p = "List what is worth remembering from this part of a recording. Keep names, places and technical "
                    "terms in the script they were spoken in. Reply with one JSON object and nothing else, in this shape:\n";
    p += kMemoryShape;
    p += "\nRules:\n- Use only what was said. Use an empty array when nothing applies.\n" + memory_rules(lang);
    if (!vocab_line.empty()) p += vocab_line + "\n";
    p += "\nTranscript (each line starts with its [t=SEC] marker):\n";
    for (const auto &pc : pieces) p += "[t=" + std::to_string(pc.first) + "] " + pc.second + "\n";
    p += "\nJSON:\n";
    return p;
}

json complete_memory_items(const std::string &model_out) {
    json out = json::array();
    const std::string s = drop_thinking(model_out);
    size_t key = s.find("\"memories\"");
    if (key == std::string::npos) return out;
    size_t open = s.find_first_not_of(" \t\r\n", key + 10);
    if (open == std::string::npos || s[open] != ':') return out;
    open = s.find_first_not_of(" \t\r\n", open + 1);
    if (open == std::string::npos || s[open] != '[') return out;
    int depth = 0;
    bool in_str = false, esc = false;
    size_t item = std::string::npos;
    for (size_t i = open + 1; i < s.size(); ++i) {
        const char c = s[i];
        if (in_str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') in_str = true;
        else if (c == '{' || c == '[') { if (depth++ == 0) item = i; }
        else if (c == '}' || c == ']') {
            if (depth == 0) break;  // the array closed
            if (--depth == 0 && item != std::string::npos) {
                json j = json::parse(s.substr(item, i + 1 - item), nullptr, false);
                if (j.is_object()) out.push_back(j);
                item = std::string::npos;
            }
        }
    }
    return out;
}

static json strings_of(const json &o, const char *key) {
    json out = json::array();
    auto it = o.find(key);
    if (it == o.end() || !it->is_array()) return out;
    for (const auto &e : *it) {
        if (!e.is_string()) continue;
        const std::string t = trim_ws(strip_unclear_tags(e.get<std::string>()));
        if (!t.empty()) out.push_back(t);
    }
    return out;
}

bool parse_section_summary(const std::string &model_out, const std::vector<int64_t> &piece_starts, json &out) {
    try {
        json j;
        // Fences and leading prose sit outside the chosen object; a truncated object never balances.
        if (!first_json_object(drop_thinking(model_out), j)) return false;
        if (!j.contains("title") || !j["title"].is_string()) return false;
        // The [unclear] tag of the prompt never reaches what the person sees (fix round, item 1).
        const std::string title = trim_ws(strip_unclear_tags(j["title"].get<std::string>()));
        if (title.empty()) return false;
        auto pit = j.find("points");
        if (pit == j.end() || !pit->is_array()) return false;
        json points = json::array();
        for (const auto &e : *pit) {
            if (points.size() >= kSectionMaxPoints) break;
            if (!e.is_object() || !e.contains("text") || !e["text"].is_string()) continue;
            const std::string text = trim_ws(strip_unclear_tags(e["text"].get<std::string>()));
            if (text.empty()) continue;
            int64_t s = 0;
            if (e.contains("start_sec") && e["start_sec"].is_number()) {
                double d = e["start_sec"].get<double>();  // non-finite or absurd values count as 0, then clamp to a piece
                if (std::isfinite(d) && std::fabs(d) < 1e12) s = (int64_t)d;
            }
            if (!piece_starts.empty()) {
                int64_t best = piece_starts[0];
                for (int64_t c : piece_starts) if (std::llabs(c - s) < std::llabs(best - s)) best = c;
                s = best;
            }
            points.push_back({{"text", text}, {"start_sec", s}});
        }
        if (points.empty()) return false;
        out = {{"title", title},
               {"topic", j.contains("topic") && j["topic"].is_string() ? trim_ws(strip_unclear_tags(j["topic"].get<std::string>())) : ""},
               {"points", points}, {"terms", strings_of(j, "terms")}, {"hints", strings_of(j, "hints")}};
        return true;
    } catch (...) {
        return false;
    }
}

json fallback_section_summary(const SectionPieces &pieces) {
    std::string title;
    int words = 0;
    for (const auto &pc : pieces) {
        const std::string &t = pc.second;
        size_t i = 0;
        while (words < 8 && i < t.size()) {
            while (i < t.size() && isspace((unsigned char)t[i])) ++i;
            size_t b = i;
            while (i < t.size() && !isspace((unsigned char)t[i])) ++i;
            if (i > b) { if (words) title += ' '; title += t.substr(b, i - b); ++words; }
        }
        if (words >= 8) break;
    }
    json points = json::array();
    for (const auto &pc : pieces) {
        if (points.size() >= 3) break;
        const std::string &t = pc.second;
        size_t b = t.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        size_t e = t.find_first_of(".?!\n", b);
        // '.' '?' '!' are ASCII; the Telugu/Devanagari danda (U+0964) ends a sentence too.
        size_t d = t.find("।", b);
        size_t end = std::min(e == std::string::npos ? t.size() : e + 1, d == std::string::npos ? t.size() : d + 3);
        std::string sent = t.substr(b, end - b);
        while (!sent.empty() && isspace((unsigned char)sent.back())) sent.pop_back();
        if (!sent.empty()) points.push_back({{"text", sent}, {"start_sec", pc.first}});
    }
    return {{"title", title}, {"topic", ""}, {"points", points}, {"terms", json::array()}, {"hints", json::array()},
            {"fallback", true}};
}

namespace {

// Decodes UTF-8 into code points; bad bytes become U+FFFD.
std::u32string decode_utf8(const std::string &s) {
    std::u32string o;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (n == 0 || i + n > s.size()) { o.push_back(0xFFFD); ++i; continue; }
        char32_t cp = n == 1 ? c : c & (0xFF >> (n + 1));
        bool ok = true;
        for (int k = 1; k < n; ++k) {
            unsigned char d = (unsigned char)s[i + k];
            if ((d & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (d & 0x3F);
        }
        if (!ok) { o.push_back(0xFFFD); ++i; continue; }
        o.push_back(cp);
        i += n;
    }
    return o;
}

char32_t lower_cp(char32_t c) {
    if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if ((c >= 0xC0 && c <= 0xDE && c != 0xD7) || (c >= 0x391 && c <= 0x3A9) || (c >= 0x410 && c <= 0x42F)) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return (c & 1) ? c : c + 1;  // Latin Extended-A pairs
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c + 1 : c;
    return c;
}

bool space_cp(char32_t c) { return c == ' ' || (c >= 9 && c <= 13) || c == 0xA0 || c == 0x2003 || c == 0x2009 || c == 0x3000 || c == 0x200B; }

std::string normalise_topic(const std::string &s) {
    std::u32string cps = decode_utf8(s), out;
    bool gap = false;
    for (char32_t c : cps) {
        if (space_cp(c)) { gap = !out.empty(); continue; }
        if (gap) { out.push_back(' '); gap = false; }
        out.push_back(lower_cp(c));
    }
    std::string r;
    for (char32_t c : out) {
        if (c < 0x80) r += (char)c;
        else if (c < 0x800) { r += (char)(0xC0 | (c >> 6)); r += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { r += (char)(0xE0 | (c >> 12)); r += (char)(0x80 | ((c >> 6) & 0x3F)); r += (char)(0x80 | (c & 0x3F)); }
        else { r += (char)(0xF0 | (c >> 18)); r += (char)(0x80 | ((c >> 12) & 0x3F)); r += (char)(0x80 | ((c >> 6) & 0x3F)); r += (char)(0x80 | (c & 0x3F)); }
    }
    return r;
}

std::string str_of(const json &j, const char *k) { return j.is_object() && j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : ""; }
int64_t start_of(const json &sec) { return sec.is_object() && sec.contains("start_sec") && sec["start_sec"].is_number() ? sec["start_sec"].get<int64_t>() : 0; }
const json &summary_of(const json &sec) { static const json none = json::object(); return sec.is_object() && sec.contains("summary") && sec["summary"].is_object() ? sec["summary"] : none; }

std::string clock_label(int64_t s) {
    if (s < 0) s = 0;
    char b[24];
    if (s >= 3600) snprintf(b, sizeof b, "%lld:%02lld:%02lld", (long long)(s / 3600), (long long)(s / 60 % 60), (long long)(s % 60));
    else snprintf(b, sizeof b, "%lld:%02lld", (long long)(s / 60), (long long)(s % 60));
    return b;
}

std::string render_section(const json &sec) {
    const json &sm = summary_of(sec);
    std::string o = "[" + clock_label(start_of(sec)) + "] " + str_of(sm, "title") + "\n";
    if (sm.contains("points") && sm["points"].is_array())
        for (const auto &p : sm["points"]) { std::string t = str_of(p, "text"); if (!t.empty()) o += "- " + t + "\n"; }
    return o;
}

std::string cut_to_fit(std::string s, int budget, const std::function<int(const std::string &)> &count) {
    while (!s.empty() && count(s) > budget) {
        size_t n = s.size() * 9 / 10;
        if (n >= s.size()) n = s.size() - 1;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) --n;
        s.resize(n);
    }
    return s;
}

}  // namespace

json merge_chapters(const json &sections) {
    json out = json::array();
    if (!sections.is_array()) return out;
    std::string last;
    bool have = false;
    for (const auto &sec : sections) {
        const json &sm = summary_of(sec);
        std::string title = str_of(sm, "title");
        std::string key = normalise_topic(str_of(sm, "topic").empty() ? title : str_of(sm, "topic"));
        if (have && key == last) continue;
        out.push_back({{"title", title}, {"start_sec", start_of(sec)}});
        last = key;
        have = true;
    }
    return out;
}

std::vector<std::string> section_groups(const json &sections, int budget_tokens,
                                        const std::function<int(const std::string &)> &count_tokens) {
    std::vector<std::string> groups;
    if (!sections.is_array()) return groups;
    std::string cur;
    for (const auto &sec : sections) {
        std::string b = render_section(sec);
        if (b.empty()) continue;
        if (count_tokens(b) > budget_tokens) b = cut_to_fit(b, budget_tokens, count_tokens);
        // Counting the joined text keeps the guarantee for tokenisers that do not add up per block.
        if (!cur.empty() && count_tokens(cur + b) > budget_tokens) { groups.push_back(cur); cur.clear(); }
        cur += b;
    }
    if (!cur.empty()) groups.push_back(cur);
    return groups;
}

std::string sections_text(const json &sections) {
    std::string out;
    if (!sections.is_array()) return out;
    for (const auto &sec : sections) out += render_section(sec);
    return sections.empty() ? std::string() : out;
}

namespace {

// The line ends a sentence: . ! ? or the danda (U+0964), optionally followed by closing brackets or quotes.
bool ends_sentence(const std::string &line) {
    std::string t = line;
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r')) t.pop_back();
    while (!t.empty() && (t.back() == ')' || t.back() == ']' || t.back() == '"' || t.back() == '\'' || t.back() == '*')) t.pop_back();
    if (t.empty()) return false;
    const char c = t.back();
    if (c == '.' || c == '!' || c == '?') return true;
    return t.size() >= 3 && t.compare(t.size() - 3, 3, "\xE0\xA5\xA4") == 0;  // ।
}

// Words of a line before byte `upto`, not counting a list marker ("-", "*", "1.", "Q3.", "A3.", "[ ]").
int words_before(const std::string &line, size_t upto) {
    int n = 0;
    size_t i = 0;
    bool first = true;
    while (i < upto) {
        while (i < upto && (line[i] == ' ' || line[i] == '\t')) ++i;
        size_t b = i;
        while (i < upto && line[i] != ' ' && line[i] != '\t') ++i;
        if (i == b) break;
        const std::string w = line.substr(b, i - b);
        bool marker = false;
        if (first) {
            std::string core = w;
            if (!core.empty() && core.back() == '.') core.pop_back();
            if (!core.empty() && (core[0] == 'Q' || core[0] == 'A')) core.erase(0, 1);
            marker = w == "-" || w == "*" || w == "#" || w == "##" || w == "###" ||
                     (!core.empty() && core.find_first_not_of("0123456789") == std::string::npos);
        }
        if (w == "[" || w == "]" || w == "[ ]" || w == "[x]") marker = true;
        if (!marker) ++n;
        first = false;
    }
    return n;
}

bool is_heading(const std::string &l) { return l.rfind("#", 0) == 0; }
bool is_question(const std::string &l) {
    return l.size() > 2 && l[0] == 'Q' && l[1] >= '0' && l[1] <= '9';
}

}  // namespace

// ---- owner, note 19: invented expansions, empty action items ----

namespace {

bool ascii_upper(char c) { return c >= 'A' && c <= 'Z'; }
bool ascii_digit(char c) { return c >= '0' && c <= '9'; }
bool ascii_alnum(char c) { return ascii_upper(c) || ascii_digit(c) || (c >= 'a' && c <= 'z'); }

// The abbreviation ending right before byte `end` (exclusive), or "" when there is none.
std::string abbreviation_before(const std::string &t, size_t end) {
    size_t b = end;
    while (b > 0 && (ascii_upper(t[b - 1]) || ascii_digit(t[b - 1]) || t[b - 1] == '&')) --b;
    if (b > 0 && (ascii_alnum(t[b - 1]) || t[b - 1] == '\'' || t[b - 1] == '-')) return "";  // inside a word: "Plan" + "A"? no
    while (b < end && t[b] == '&') ++b;
    size_t e = end;
    while (e > b && t[e - 1] == '&') --e;
    const std::string a = t.substr(b, e - b);
    int caps = 0, alnum = 0;
    for (char c : a) { caps += ascii_upper(c); alnum += ascii_upper(c) || ascii_digit(c); }
    return caps >= 1 && alnum >= 2 && e == end ? a : "";
}

// The bracket's content reads as an expansion: it starts with a capital Latin letter (not a quote, a bracket or
// another script: a Telugu bracket is most often the term as said, in its own script, as the system prompt asks),
// has no digit (ASCII or Telugu) and no bracket of its own, and has at most 10 words.
bool reads_as_expansion(const std::string &c) {
    if (c.empty() || !ascii_upper(c[0])) return false;
    if (c.find('(') != std::string::npos || c.find(')') != std::string::npos) return false;
    size_t n = 0;
    int words = 1;
    for (size_t i = 0; i < c.size(); i += n) {
        const char32_t cp = cp_at(c, i, n);
        if ((cp >= '0' && cp <= '9') || (cp >= 0x0C66 && cp <= 0x0C6F)) return false;
        if (cp == ' ') ++words;
    }
    return words <= 10;
}

bool said_in(const std::string &content, const std::string &source) {
    const std::string lc = ascii_lower(content), ls = ascii_lower(source);
    if (ls.find(lc) != std::string::npos) return true;
    static const char *const kSmall[] = {"of", "the", "and", "for", "in", "on", "a", "an", "to", "at", "by", "&"};
    int words = 0, said = 0;
    size_t a = 0;
    while (a < lc.size()) {
        size_t sp = lc.find_first_of(" \t,;/", a);
        std::string w = lc.substr(a, sp == std::string::npos ? std::string::npos : sp - a);
        a = sp == std::string::npos ? lc.size() : sp + 1;
        while (!w.empty() && std::strchr(".,;:!?\"'()-", w.back())) w.pop_back();
        while (!w.empty() && std::strchr(".,;:!?\"'()-", w.front())) w.erase(0, 1);
        if (w.size() > 2 && w.compare(w.size() - 2, 2, "'s") == 0) w.resize(w.size() - 2);
        if (w.empty() || std::find_if(std::begin(kSmall), std::end(kSmall), [&](const char *s) { return w == s; }) != std::end(kSmall))
            continue;
        ++words;
        said += term_said(w, source) ? 1 : 0;
    }
    return words > 0 && said * 2 > words;
}

std::string lower_trimmed_task(const std::string &line) {
    std::string t = trim_ws(line);
    for (bool more = true; more;) {
        more = false;
        for (const char *m : {"- ", "* ", "[ ]", "[x]", "[X]", "-", "*"}) {
            const size_t n = std::strlen(m);
            if (t.compare(0, n, m) == 0) { t = trim_ws(t.substr(n)); more = true; break; }
        }
    }
    while (!t.empty() && std::strchr(".!;:", t.back())) t.pop_back();
    return ascii_lower(trim_ws(t));
}

}  // namespace

bool is_notes_heading(const std::string &l) { return is_heading(l); }

bool is_action_heading(const std::string &l) {
    return is_heading(l) && (ascii_lower(l).find("action item") != std::string::npos ||
                             l.find("\xE0\xB0\x9A\xE0\xB0\xB0\xE0\xB1\x8D\xE0\xB0\xAF") != std::string::npos);  // చర్య
}

std::string drop_invented_expansions(const std::string &text, const std::string &source) {
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        const size_t open = text.find('(', i);
        if (open == std::string::npos) { out.append(text, i, std::string::npos); break; }
        // The matching ")" on the same line, by depth: "BPSC (Bihar (state) Commission)" is one bracket.
        size_t close = std::string::npos;
        for (size_t k = open + 1, depth = 1; k < text.size() && text[k] != '\n'; ++k) {
            if (text[k] == '(') ++depth;
            else if (text[k] == ')' && --depth == 0) { close = k; break; }
        }
        const size_t nl = text.find('\n', open);
        const bool space = open > 0 && text[open - 1] == ' ';
        const size_t abbr_end = space ? open - 1 : open;
        if (close == std::string::npos || (nl != std::string::npos && nl < close) || abbr_end < i ||
            abbreviation_before(text, abbr_end).empty()) {
            out.append(text, i, open + 1 - i);
            i = open + 1;
            continue;
        }
        const std::string content = trim_ws(text.substr(open + 1, close - open - 1));
        if (!reads_as_expansion(content) || said_in(content, source)) {
            out.append(text, i, close + 1 - i);
        } else {
            out.append(text, i, abbr_end - i);   // the abbreviation stays; the space and the bracket go
        }
        i = close + 1;
    }
    return out;
}

bool says_no_tasks(const std::string &line) {
    const std::string t = lower_trimmed_task(line);
    if (t.empty()) return false;
    // A task's who and when ("task — who — when", as the prompt asks): a real task, whatever its first words.
    for (const char *sep : {" \xE2\x80\x94 ", " \xE2\x80\x93 ", " - "})
        if (t.find(sep) != std::string::npos) return false;
    std::vector<std::string> w;
    for (size_t a = 0; a < t.size();) {
        size_t sp = t.find(' ', a);
        if (sp == std::string::npos) sp = t.size();
        std::string word = t.substr(a, sp - a);
        while (!word.empty() && std::strchr(",;:.!", word.back())) word.pop_back();
        if (!word.empty()) w.push_back(word);
        a = sp + 1;
    }
    // Short lines only: "No action items were stated in the transcript." is 8 words; a long line is a task.
    if (w.empty() || w.size() > 10) return false;
    auto in = [](const std::string &x, std::initializer_list<const char *> set) {
        for (const char *s : set) if (x == s) return true;
        return false;
    };
    // What may follow "none" or "no tasks": the line only says there are none ("None stated", "No tasks were given").
    const auto after = {"stated", "mentioned", "given", "listed", "assigned", "specified", "noted", "discussed",
                        "identified", "said", "were", "was", "are", "is", "have", "has", "in", "for", "from", "yet",
                        "so", "at", "today"};
    if (w.size() == 1 && in(w[0], {"none", "n/a", "na", "nil"})) return true;
    if (w[0] == "none") return in(w[1], after);
    // "no [specific|clear|explicit] action item(s)/action(s)/task(s)/to-do(s)", optionally after "there are/were".
    size_t i = 0;
    if (w[0] == "there" && w.size() > 2 && in(w[1], {"are", "were", "is", "was"})) i = 2;
    if (w[i] == "no") {
        size_t k = i + 1;
        if (k < w.size() && in(w[k], {"specific", "clear", "explicit", "other", "further"})) ++k;
        if (k < w.size() && w[k] == "action" && k + 1 < w.size() && in(w[k + 1], {"item", "items"})) k += 2;
        else if (k < w.size() && in(w[k], {"action", "actions", "task", "tasks", "to-do", "to-dos", "todo", "todos",
                                           "action-items"})) ++k;
        else return false;
        return k == w.size() || in(w[k], after);
    }
    if (w[0] == "nothing")
        for (const auto &x : w)
            if (in(x, {"stated", "mentioned", "assigned", "given", "specified", "noted", "discussed", "said", "listed"}))
                return true;
    // Telugu: ఏవీ లేవు, ఏమీ లేవు, ఏమీ లేదు ("none", "nothing"), on a short line.
    if (w.size() <= 8)
        for (const char *p : {"\xE0\xB0\x8F\xE0\xB0\xB5\xE0\xB1\x80 \xE0\xB0\xB2\xE0\xB1\x87\xE0\xB0\xB5\xE0\xB1\x81",
                              "\xE0\xB0\x8F\xE0\xB0\xAE\xE0\xB1\x80 \xE0\xB0\xB2\xE0\xB1\x87\xE0\xB0\xB5\xE0\xB1\x81",
                              "\xE0\xB0\x8F\xE0\xB0\xAE\xE0\xB1\x80 \xE0\xB0\xB2\xE0\xB1\x87\xE0\xB0\xA6\xE0\xB1\x81"})
            if (t.find(p) != std::string::npos) return true;
    return false;
}

std::string none_action_items(const std::string &text) {
    std::vector<std::string> lines;
    for (size_t a = 0;;) {
        const size_t nl = text.find('\n', a);
        lines.push_back(text.substr(a, nl == std::string::npos ? std::string::npos : nl - a));
        if (nl == std::string::npos) break;
        a = nl + 1;
    }
    std::vector<bool> keep(lines.size(), true);
    for (size_t h = 0; h < lines.size(); ++h) {
        if (!is_action_heading(trim_ws(lines[h]))) continue;
        size_t end = h + 1;
        while (end < lines.size() && !is_heading(trim_ws(lines[end]))) ++end;
        bool real = false;
        std::vector<size_t> none;
        for (size_t k = h + 1; k < end; ++k) {
            if (trim_ws(lines[k]).empty()) continue;
            if (says_no_tasks(lines[k])) none.push_back(k); else real = true;
        }
        for (size_t n = 0; n < none.size(); ++n) {
            if (n == 0 && !real) lines[none[n]] = "None";
            else keep[none[n]] = false;
        }
        h = end - 1;
    }
    std::string out;
    bool first = true;
    for (size_t k = 0; k < lines.size(); ++k) {
        if (!keep[k]) continue;
        out += (first ? "" : "\n") + lines[k];
        first = false;
    }
    return out;
}

std::string trim_cut_reply(const std::string &reply) {
    const std::string whole = trim_ws(reply);
    if (whole.empty()) return whole;
    std::vector<std::string> lines;
    for (size_t a = 0;;) {
        size_t nl = whole.find('\n', a);
        lines.push_back(whole.substr(a, nl == std::string::npos ? std::string::npos : nl - a));
        if (nl == std::string::npos) break;
        a = nl + 1;
    }
    std::string &last = lines.back();
    if (!ends_sentence(last)) {
        // The last finished sentence on the line: ". " / "! " / "? " / "। ".
        size_t cut = std::string::npos;
        for (size_t i = 0; i + 1 < last.size(); ++i) {
            const char c = last[i];
            if ((c == '.' || c == '!' || c == '?') && last[i + 1] == ' ') cut = i + 1;
            if (i + 3 < last.size() && last.compare(i, 3, "\xE0\xA5\xA4") == 0 && last[i + 3] == ' ') cut = i + 3;
        }
        if (cut != std::string::npos && words_before(last, cut) >= 3) {
            last.resize(cut);
        } else {
            lines.pop_back();
        }
    }
        // Trailing blank lines, headings with nothing under them, a question left without its answer.
    for (;;) {
        while (!lines.empty() && trim_ws(lines.back()).empty()) lines.pop_back();
        if (lines.empty()) break;
        const std::string l = trim_ws(lines.back());
        if (is_heading(l) || l == "-" || l == "*") { lines.pop_back(); continue; }
        if (is_question(l)) { lines.pop_back(); continue; }  // its answer was cut, or never came
        break;
    }
    if (lines.empty()) return whole;
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) out += (i ? "\n" : "") + lines[i];
    return trim_ws(out);
}

}  // namespace sve
