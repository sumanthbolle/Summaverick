#include "sve_sections.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

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
         "Rules:\n- 3 to 6 points, each one sentence.\n"
         "- Each point's \"start_sec\" is copied from the [t=SEC] marker of the transcript line it came from.\n"
         "- Use only what was said. Use an empty array when nothing applies.\n";
    if (!vocab_line.empty()) p += vocab_line + "\n";
    p += "\nTranscript (each line starts with its [t=SEC] marker):\n";
    for (const auto &pc : pieces) p += "[t=" + std::to_string(pc.first) + "] " + pc.second + "\n";
    p += "\nJSON:\n";
    return p;
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

static json strings_of(const json &o, const char *key) {
    json out = json::array();
    auto it = o.find(key);
    if (it == o.end() || !it->is_array()) return out;
    for (const auto &e : *it)
        if (e.is_string() && !e.get<std::string>().empty()) out.push_back(e);
    return out;
}

bool parse_section_summary(const std::string &model_out, const std::vector<int64_t> &piece_starts, json &out) {
    try {
        json j;
        // Fences and leading prose sit outside the chosen object; a truncated object never balances.
        if (!first_json_object(drop_thinking(model_out), j)) return false;
        if (!j.contains("title") || !j["title"].is_string() || j["title"].get<std::string>().empty()) return false;
        auto pit = j.find("points");
        if (pit == j.end() || !pit->is_array()) return false;
        json points = json::array();
        for (const auto &e : *pit) {
            if (points.size() >= 6) break;
            if (!e.is_object() || !e.contains("text") || !e["text"].is_string() || e["text"].get<std::string>().empty()) continue;
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
            points.push_back({{"text", e["text"]}, {"start_sec", s}});
        }
        if (points.empty()) return false;
        out = {{"title", j["title"]},
               {"topic", j.contains("topic") && j["topic"].is_string() ? j["topic"] : json("")},
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

}  // namespace sve
