#include "sve_onepass.h"

#include <cstring>

namespace sve {

static std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) b--;
    return s.substr(a, b - a);
}

std::optional<AllParts> split_all(const std::string &text) {
    static const char *const kHeads[4] = {"### TITLE", "### SUMMARY", "### NOTES", "### QUESTIONS"};
    std::string parts[4];
    int cur = -1;   // the header the lines belong to; -1 before the first
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        const size_t end = nl == std::string::npos ? text.size() : nl;
        const std::string line = text.substr(pos, end - pos);
        const std::string t = trim(line);
        int hit = -1;
        for (int i = 0; i < 4; i++) if (t == kHeads[i]) { hit = i; break; }
        if (hit >= 0) {
            if (hit != cur + 1) return std::nullopt;   // out of order, repeated or skipped
            cur = hit;
        } else if (cur >= 0) {
            parts[cur] += line;
            parts[cur] += '\n';
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    if (cur != 3) return std::nullopt;
    AllParts r{trim(parts[0]), trim(parts[1]), trim(parts[2]), trim(parts[3])};
    if (r.title.empty() || r.summary.empty() || r.notes.empty() || r.questions.empty()) return std::nullopt;
    return r;
}

} // namespace sve
