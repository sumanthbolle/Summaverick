#include "sve_sections.h"

#include <algorithm>
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
            if (e.contains("start_sec") && e["start_sec"].is_number()) s = e["start_sec"].get<int64_t>();
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

}  // namespace sve
