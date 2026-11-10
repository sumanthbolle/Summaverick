#include "sve_assemble.h"
#include "sve_prompts.h"
#include "sve_sections.h"
#include "sve_textmatch.h"

#include <set>
#include <vector>

namespace sve {

namespace {

std::string trim(const std::string &s) {
    const char *ws = " \t\r\n";
    const size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

std::vector<std::string> lines_of(const std::string &text) {
    std::vector<std::string> out;
    for (size_t a = 0;;) {
        const size_t nl = text.find('\n', a);
        out.push_back(text.substr(a, nl == std::string::npos ? std::string::npos : nl - a));
        if (nl == std::string::npos) break;
        a = nl + 1;
    }
    return out;
}

std::string str_at(const nlohmann::json &arr, size_t i) {
    return arr.is_array() && i < arr.size() && arr[i].is_string() ? arr[i].get<std::string>() : std::string();
}

// The key two lines are the same task (or question) by: their match_tokens, joined; the trimmed line without them.
std::string token_key(const std::string &line) {
    std::string k;
    for (const auto &t : match_tokens(line)) k += t + " ";
    return k.empty() ? trim(line) : k;
}

struct Block {
    std::string heading;             // "" for the lines before a part's first heading
    std::vector<std::string> lines;  // non-blank content lines, trimmed on the right
};

std::vector<Block> blocks_of(const std::string &text) {
    std::vector<Block> out(1);
    for (const auto &raw : lines_of(text)) {
        const std::string l = trim(raw);
        if (is_notes_heading(l)) { out.push_back({l, {}}); continue; }
        if (!l.empty()) out.back().lines.push_back(raw.substr(0, raw.find_last_not_of(" \t\r") + 1));
    }
    return out;
}

// A block whose every line only says there is nothing ("None", "N/A", "No action items were stated").
bool says_nothing(const Block &b) {
    for (const auto &l : b.lines)
        if (!says_no_tasks(l)) return false;
    return true;
}

// "## Key points" -> "### Key points": a part's headings sit under its section heading.
std::string demoted(const std::string &heading) {
    size_t n = 0;
    while (n < heading.size() && heading[n] == '#') ++n;
    return std::string(n + 1, '#') + " " + trim(heading.substr(n));
}

struct QA { std::string q, a; };

// "Q3. text" / "A3) text" / "Q3: text" -> the text; false when the line is no such label.
bool label(const std::string &line, char c, std::string &rest) {
    if (line.size() < 3 || line[0] != c || line[1] < '0' || line[1] > '9') return false;
    size_t i = 1;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9') ++i;
    if (i >= line.size() || (line[i] != '.' && line[i] != ')' && line[i] != ':')) return false;
    rest = trim(line.substr(i + 1));
    return true;
}

std::vector<QA> questions_of(const std::string &text) {
    std::vector<QA> out;
    bool in_answer = false;
    for (const auto &raw : lines_of(text)) {
        const std::string l = trim(raw);
        std::string rest;
        if (label(l, 'Q', rest)) { out.push_back({rest, ""}); in_answer = false; continue; }
        if (out.empty()) continue;
        if (label(l, 'A', rest)) { out.back().a = rest; in_answer = true; continue; }
        if (l.empty()) continue;
        std::string &field = in_answer ? out.back().a : out.back().q;
        field += (field.empty() ? "" : " ") + l;
    }
    std::vector<QA> kept;
    for (auto &x : out)
        if (!x.q.empty() && !x.a.empty()) kept.push_back(x);
    return kept;
}

double jaccard(const std::set<std::string> &a, const std::set<std::string> &b) {
    if (a.empty() && b.empty()) return 1.0;
    size_t both = 0;
    for (const auto &x : a) both += b.count(x);
    return (double)both / (double)(a.size() + b.size() - both);
}

}  // namespace

int section_output_max_out(SectionOutputKind kind) { return kind == SectionOutputKind::Notes ? 360 : 200; }

std::string section_output_prompt(SectionOutputKind kind, const std::string &material, const std::string &guide,
                                  int part, int total, bool from_bullets) {
    std::string head = "This is part " + std::to_string(part) + " of " + std::to_string(total) + " of the recording.\n";
    if (!trim(guide).empty())
        head += "A short outline of this part, as a guide only (the transcript is what counts):\n" + trim(guide) + "\n";
    head += "\n";
    return head + (kind == SectionOutputKind::Notes ? detailed_notes_prompt(material, from_bullets)
                                                    : revision_questions_prompt(material, from_bullets));
}

std::string assemble_notes(const nlohmann::json &sections, const nlohmann::json &outputs) {
    std::string body;
    std::vector<std::string> tasks;
    std::set<std::string> task_keys;
    bool said_none = false;
    const size_t n = sections.is_array() ? sections.size() : 0;
    for (size_t i = 0; i < n; ++i) {
        const std::string text = strip_unclear_tags(str_at(outputs, i));
        if (trim(text).empty()) continue;
        std::string part;
        for (const auto &b : blocks_of(text)) {
            if (is_action_heading(b.heading)) {
                for (const auto &l : b.lines) {
                    if (says_no_tasks(l)) { said_none = true; continue; }
                    if (task_keys.insert(token_key(l)).second) tasks.push_back(trim(l));
                }
                continue;
            }
            if (b.lines.empty() || says_nothing(b)) continue;
            if (!b.heading.empty()) part += demoted(b.heading) + "\n";
            for (const auto &l : b.lines) part += l + "\n";
            part += "\n";
        }
        if (part.empty()) continue;
        std::string title;
        const auto &s = sections[i];
        if (s.is_object() && s.contains("summary") && s["summary"].is_object() && s["summary"].contains("title") &&
            s["summary"]["title"].is_string())
            title = trim(strip_unclear_tags(s["summary"]["title"].get<std::string>()));
        if (title.empty()) title = "Part " + std::to_string(i + 1);
        body += "## " + title + "\n\n" + part;
    }
    if (!tasks.empty()) {
        body += "## Action items\n";
        for (const auto &t : tasks) body += t + "\n";
    } else if (said_none && !body.empty()) {
        body += "## Action items\nNone\n";
    }
    return trim(none_action_items(body));
}

std::string assemble_questions(const nlohmann::json &outputs, int max) {
    std::vector<std::vector<QA>> parts;
    size_t longest = 0;
    for (size_t i = 0; outputs.is_array() && i < outputs.size(); ++i) {
        parts.push_back(questions_of(strip_unclear_tags(str_at(outputs, i))));
        longest = std::max(longest, parts.back().size());
    }
    std::vector<QA> kept;
    std::vector<std::set<std::string>> kept_tokens;
    for (size_t r = 0; r < longest && (int)kept.size() < max; ++r) {
        for (const auto &p : parts) {
            if (r >= p.size() || (int)kept.size() >= max) continue;
            const auto toks = match_tokens(p[r].q);
            const std::set<std::string> t(toks.begin(), toks.end());
            bool dup = false;
            for (const auto &k : kept_tokens)
                if (jaccard(t, k) >= 0.7) { dup = true; break; }
            if (dup) continue;
            kept.push_back(p[r]);
            kept_tokens.push_back(t);
        }
    }
    std::string out;
    for (size_t i = 0; i < kept.size(); ++i) {
        const std::string n = std::to_string(i + 1);
        out += (i ? "\n\n" : "") + std::string("Q") + n + ". " + kept[i].q + "\nA" + n + ". " + kept[i].a;
    }
    return out;
}

}  // namespace sve
