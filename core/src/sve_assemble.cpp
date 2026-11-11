#include "sve_assemble.h"
#include "sve_prompts.h"
#include "sve_sections.h"
#include "sve_textmatch.h"

#include <cctype>
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
        if (is_notes_heading(l)) {
            // "## Topic The Collector's role" (the model went on after the prompt's last heading on its line):
            // the heading is "## Topic", the rest is its content.
            size_t n = 0;
            while (n < l.size() && l[n] == '#') ++n;
            const std::string name = trim(l.substr(n));
            std::string low = name.substr(0, 5);
            for (auto &c : low) c = (char)std::tolower((unsigned char)c);
            if (low == "topic" && name.size() > 5 && (name[5] == ' ' || name[5] == ':')) {
                const std::string rest = trim(name.substr(6));
                out.push_back({l.substr(0, n) + " " + name.substr(0, 5), {}});
                if (!rest.empty()) out.back().lines.push_back(rest);
                continue;
            }
            out.push_back({l, {}});
            continue;
        }
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

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_label_end(char c) { return c == '.' || c == ')' || c == ':'; }

// The text after a label, without the bold marks around it ("**Q1.** What…?" -> "What…?").
std::string after_label(const std::string &line, size_t i) {
    std::string r = trim(line.substr(i));
    while (!r.empty() && r.front() == '*') r = trim(r.substr(1));
    while (!r.empty() && r.back() == '*') r = trim(r.substr(0, r.size() - 1));
    return r;
}

// A line without the list and heading marks in front ("**", "#", "- " is not stripped: it is no label).
std::string unmarked(const std::string &raw) {
    const std::string l = trim(raw);
    const size_t a = l.find_first_not_of("*#_ \t");
    return a == std::string::npos ? "" : l.substr(a);
}

// A question label: "Q1." / "Q 1)" / "Q1:" (any case), or a bare "1." / "1)" when `bare`. Sets rest.
bool q_label(const std::string &l, bool bare, std::string &rest) {
    size_t i = 0;
    const bool q = !l.empty() && (l[0] == 'Q' || l[0] == 'q');
    if (q) { i = 1; while (i < l.size() && l[i] == ' ') ++i; }
    else if (!bare) return false;
    const size_t d = i;
    while (i < l.size() && is_digit(l[i])) ++i;
    if (i == d || i >= l.size() || !is_label_end(l[i]) || (!q && l[i] == ':')) return false;
    rest = after_label(l, i + 1);
    return true;
}

// An answer label: "A1." / "A 1)" / "A." / "A:" / "Answer:" / "Ans 1." Sets rest.
bool a_label(const std::string &l, std::string &rest) {
    size_t i = 0;
    if (l.compare(0, 6, "Answer") == 0) i = 6;
    else if (l.compare(0, 3, "Ans") == 0) i = 3;
    else if (!l.empty() && l[0] == 'A') i = 1;
    else return false;
    while (i < l.size() && l[i] == ' ') ++i;
    while (i < l.size() && is_digit(l[i])) ++i;
    if (i >= l.size() || !is_label_end(l[i])) return false;
    rest = after_label(l, i + 1);
    return true;
}

std::vector<QA> questions_of(const std::string &text) {
    const std::vector<std::string> lines = lines_of(text);
    // A bare "1." starts a question only in a reply without any "Q1." label (else it is a list in an answer).
    bool bare = true;
    for (const auto &raw : lines) {
        std::string rest;
        if (q_label(unmarked(raw), false, rest)) { bare = false; break; }
    }
    std::vector<QA> out;
    bool in_answer = false;
    for (const auto &raw : lines) {
        const std::string l = unmarked(raw);
        std::string rest;
        if (q_label(l, bare && (out.empty() || in_answer), rest)) { out.push_back({rest, ""}); in_answer = false; continue; }
        if (out.empty()) continue;
        if (!in_answer && a_label(l, rest)) { out.back().a = rest; in_answer = true; continue; }
        if (l.empty()) continue;
        std::string &field = in_answer ? out.back().a : out.back().q;
        field += (field.empty() ? "" : " ") + trim(raw);
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

int section_output_max_out(SectionOutputKind kind) { return kind == SectionOutputKind::Notes ? 440 : 300; }

int count_questions(const std::string &reply) { return (int)questions_of(strip_unclear_tags(reply)).size(); }

std::string section_prompt_shared(const std::string &material, const std::string &guide, int part, int total,
                                  bool from_bullets) {
    // total 0: a part written while the recording goes on, when nobody knows yet how many parts there will be.
    std::string head = "This is part " + std::to_string(part) +
                       (total > 0 ? " of " + std::to_string(total) : std::string()) + " of the recording.\n";
    if (!trim(guide).empty())
        head += "A short outline of this part, as a guide only (the transcript is what counts):\n" + trim(guide) + "\n";
    head += "\n";
    return head + "Below " + (from_bullets ? "are bullet points extracted from" : "is") +
           " the transcript of this part (a lecture, lesson, meeting, discussion or personal note).\n\n"
           "Transcript:\n\"\"\"\n" + material + "\n\"\"\"\n\n";
}

std::string section_prompt_suffix(SectionOutputKind kind) {
    if (kind == SectionOutputKind::Notes)
        return "Turn the text above into organised notes using exactly this structure (keep the headings, leave a "
               "heading as 'None' if nothing applies):\n\n"
               "## Topic\n(one line)\n\n"
               "## Key points\n- bullet points, grouped by sub-topic, keep every definition, number, name and date\n\n"
               "## Decisions and instructions\n- anything that was decided, prescribed, assigned or promised\n\n"
               "## Action items\n- [ ] task — who — when (only if stated; only what a speaker asked for or committed to)\n\n"
               "## Dates and numbers mentioned\n- …\n\n"
               "## Open questions\n- things left unresolved\n\n"
               "Use only what is in the text.\n" + faithful_rules(true) + "\n## Topic";
    return "Write 6 revision questions a student could use to test themselves on the text above, from easy to hard. "
           "Every answer must come from the text; do not invent anything.\n" + faithful_rules(false) +
           "Use exactly this format, with a blank line between questions:\n\n"
           "Q1. <question about the text>\nA1. <answer taken from the text>\n\n"
           "Q2. <question about the text>\nA2. <answer taken from the text>\n\n"
           "(continue to Q6)";
}

std::string section_output_prompt(SectionOutputKind kind, const std::string &material, const std::string &guide,
                                  int part, int total, bool from_bullets) {
    return section_prompt_shared(material, guide, part, total, from_bullets) + section_prompt_suffix(kind);
}

bool split_templated(const std::string &full, const std::string &shared_user, const std::string &suffix_user,
                     std::string &head, std::string &tail) {
    const size_t at = full.rfind(shared_user + suffix_user);
    if (shared_user.empty() || at == std::string::npos) return false;
    head = full.substr(0, at + shared_user.size());
    tail = full.substr(at + shared_user.size());
    return true;
}

bool section_pair_fits(int n_seq_max, int n_ctx, int n_ctx_seq, int shared_tokens, int notes_suffix_tokens,
                       int questions_suffix_tokens) {
    if (n_seq_max < 2 || shared_tokens <= 0) return false;
    const int cap_n = section_output_max_out(SectionOutputKind::Notes);
    const int cap_q = section_output_max_out(SectionOutputKind::Questions);
    const long total = (long)shared_tokens + notes_suffix_tokens + questions_suffix_tokens + cap_n + cap_q + kPairMargin;
    if (total > n_ctx) return false;
    if ((long)shared_tokens + notes_suffix_tokens + cap_n + kPairMargin > n_ctx_seq) return false;
    if ((long)shared_tokens + questions_suffix_tokens + cap_q + kPairMargin > n_ctx_seq) return false;
    return true;
}

std::string assemble_notes(const nlohmann::json &sections, const nlohmann::json &outputs, const std::string *placeholder) {
    std::string body;
    std::vector<std::string> tasks;
    std::set<std::string> task_keys;
    // "None" only when every non-empty part has an action items heading that says so: a part without one (cut
    // at its cap before it, or the heading left out) may have had tasks nobody saw.
    bool all_said_none = true, any_part = false;
    const size_t n = sections.is_array() ? sections.size() : 0;
    auto title_of = [&](size_t i) {
        std::string title;
        const auto &s = sections[i];
        if (s.is_object() && s.contains("summary") && s["summary"].is_object() && s["summary"].contains("title") &&
            s["summary"]["title"].is_string())
            title = trim(strip_unclear_tags(s["summary"]["title"].get<std::string>()));
        return title.empty() ? "Part " + std::to_string(i + 1) : title;
    };
    for (size_t i = 0; i < n; ++i) {
        const bool missing = !outputs.is_array() || i >= outputs.size() || outputs[i].is_null();
        if (missing && placeholder) {
            // Not written yet: its heading and the placeholder. It may have had tasks, so never "None".
            all_said_none = false;
            body += "## " + title_of(i) + "\n\n" + *placeholder + "\n\n";
            continue;
        }
        const std::string text = strip_unclear_tags(str_at(outputs, i));
        if (trim(text).empty()) continue;
        any_part = true;
        bool part_said_none = false;
        std::string part;
        for (const auto &b : blocks_of(text)) {
            if (is_action_heading(b.heading)) {
                for (const auto &l : b.lines) {
                    if (says_no_tasks(l)) { part_said_none = true; continue; }
                    if (task_keys.insert(token_key(l)).second) tasks.push_back(trim(l));
                }
                continue;
            }
            if (b.lines.empty() || says_nothing(b)) continue;
            if (!b.heading.empty()) part += demoted(b.heading) + "\n";
            for (const auto &l : b.lines) part += l + "\n";
            part += "\n";
        }
        if (!part_said_none) all_said_none = false;
        if (part.empty()) continue;
        body += "## " + title_of(i) + "\n\n" + part;
    }
    if (!tasks.empty()) {
        body += "## Action items\n";
        for (const auto &t : tasks) body += t + "\n";
    } else if (any_part && all_said_none && !body.empty()) {
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
