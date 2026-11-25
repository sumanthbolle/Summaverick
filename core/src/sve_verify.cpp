#include "sve_verify.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <regex>
#include <set>

#include "sve_assemble.h"
#include "sve_context.h"
#include "sve_memory.h"
#include "sve_prompts.h"
#include "sve_sections.h"
#include "sve_skills.h"
#include "sve_textmatch.h"
#include "sve_whisper_text.h"

namespace sve {

namespace {

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string lower_ascii(std::string s) {
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::vector<std::string> lines_of(const std::string &text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) nl = text.size();
        out.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}

// Decodes one UTF-8 code point at i (advancing i); 0xFFFD on a bad byte.
uint32_t next_cp(const std::string &s, size_t &i) {
    const unsigned char c = (unsigned char)s[i];
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (n == 0 || i + n > s.size()) { ++i; return 0xFFFD; }
    uint32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
    for (int k = 1; k < n; ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
    i += n;
    return cp;
}

bool is_telugu_letter(uint32_t cp) { return cp >= 0x0C00 && cp <= 0x0C7F; }
bool is_letter(uint32_t cp) {
    return (cp < 0x80 && std::isalpha((int)cp)) || (cp >= 0xC0 && cp < 0x2000) || is_telugu_letter(cp) ||
           (cp >= 0x0900 && cp <= 0x0DFF);
}

size_t letters(const std::string &s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size();) n += is_letter(next_cp(s, i));
    return n;
}

// Words that say nothing about what was said: English stop words and the notes' own fields.
const std::set<std::string> &stop_words() {
    static const std::set<std::string> s = {
        "the", "and", "for", "that", "this", "with", "from", "will", "would", "should", "could", "have", "has", "had",
        "been", "were", "they", "them", "their", "there", "what", "when", "which", "who", "whom", "about", "into",
        "after", "before", "then", "than", "also", "only", "very", "some", "more", "most", "such", "each", "other",
        "your", "yours", "ours", "our", "his", "her", "hers", "its", "it's", "is", "are", "was", "not", "but", "all",
        "any", "can", "may", "might", "must", "shall", "being", "does", "did", "done", "doing", "make", "made", "said",
        "says", "say", "told", "tell", "asked", "ask", "stated", "mentioned", "decided", "decision", "agreed",
        "action", "item", "items", "task", "none", "unknown", "someone", "speaker", "everyone", "team", "next", "need",
        "needs", "needed", "going", "get", "got", "give", "given", "take", "taken", "until", "once", "over", "upon",
        "because", "since", "while", "where", "these", "those", "here", "just", "like", "well", "back", "first",
        "plan", "plans", "planned", "discussed", "discuss", "noted", "note", "notes", "recording", "transcript",
        "listener", "person", "people", "meeting", "today",
        // Telugu words that carry no fact (and, between, said, happened, is, about, according to, that, this)
        "మరియు", "మధ్య", "అని", "చెప్పాడు", "చెప్పింది", "చెప్పారు", "జరిగింది", "ఉంది", "ఉన్నాయి", "గురించి",
        "ప్రకారం", "అది", "ఇది", "కూడా", "చేశారు", "చేశాడు", "తెలియజేశాడు", "అయితే", "కాబట్టి", "ఏమిటి", "ఎవరు",
    };
    return s;
}

bool is_number(const std::string &t) {
    return !t.empty() && std::all_of(t.begin(), t.end(), [](char c) { return std::isdigit((unsigned char)c); });
}

// A canonical token the notes can name: a number, or a word of 4+ letters that is not a stop word.
bool content_token(const std::string &t) {
    if (is_number(t)) return true;
    // match_tokens' canonical forms (wd1, mo12, rd-1, rw0, rm1); "month" or "model" are words.
    static const std::regex canon(R"(^(wd|mo|rd|rw|rm)-?[0-9]+$)");
    if (std::regex_match(t, canon)) return true;
    if (letters(t) < 4 && (t.empty() || (unsigned char)t[0] < 0x80)) return false;
    if ((unsigned char)t[0] >= 0x80 && letters(t) < 2) return false;
    return !stop_words().count(t);
}

std::vector<std::string> content_tokens(const std::string &s) {
    std::vector<std::string> out;
    for (const auto &t : match_tokens(s))
        if (content_token(t)) out.push_back(t);
    return out;
}

size_t common_prefix(const std::string &a, const std::string &b) {
    size_t n = 0;
    while (n < a.size() && n < b.size() && a[n] == b[n]) ++n;
    return n;
}

// The same word, or the same word with another ending ("decide"/"decided", Telugu case endings): a shared prefix of
// 5 bytes (ASCII) or 3 letters (9 bytes, Telugu) that covers most of the shorter one.
bool same_word(const std::string &a, const std::string &b) {
    if (a == b) return true;
    if (is_number(a) || is_number(b)) return false;
    const size_t p = common_prefix(a, b), shorter = std::min(a.size(), b.size());
    const bool ascii = (unsigned char)a[0] < 0x80;
    if (ascii) return p >= 5 && p * 4 >= shorter * 3;
    return p >= 9 && p * 10 >= shorter * 6;
}

struct Said {
    std::set<std::string> exact;
    std::vector<std::string> all;
};

Said said_tokens(const json &pieces) {
    Said s;
    if (!pieces.is_array()) return s;
    for (const auto &p : pieces)
        for (const auto &t : match_tokens(jstr(p, "text")))
            if (s.exact.insert(t).second) s.all.push_back(t);
    return s;
}

bool said_word(const std::string &t, const Said &s) {
    if (s.exact.count(t)) return true;
    if (is_number(t)) return false;
    for (const auto &u : s.all)
        if (same_word(t, u)) return true;
    return false;
}

std::string bullet_text(const std::string &raw) {
    std::string l = trim(raw);
    while (!l.empty() && (l[0] == '-' || l[0] == '*' || l[0] == '+' || l[0] == ' ')) l.erase(0, 1);
    for (const char *box : {"[ ]", "[x]", "[X]"})
        if (l.rfind(box, 0) == 0) l = trim(l.substr(3));
    // "1." / "1)" list numbers
    size_t d = 0;
    while (d < l.size() && std::isdigit((unsigned char)l[d])) ++d;
    if (d > 0 && d < l.size() && (l[d] == '.' || l[d] == ')')) l = trim(l.substr(d + 1));
    return l;
}

bool says_none(const std::string &l) {
    const std::string x = lower_ascii(trim(l));
    std::string y;
    for (char c : x) if (c != '.' && c != '*' && c != '_') y.push_back(c);
    y = trim(y);
    return y.empty() || y == "none" || y == "none stated" || y == "nothing" || y == "n/a" || y == "no action items" ||
           y == "no decisions" || y.find("ఏమీ లేవు") != std::string::npos || y == "లేవు" || y == "లేదు" ||
           y.find("none were") == 0 || y.find("no specific") == 0 || y.find("no action") == 0 ||
           y.find("no decision") == 0;
}

bool decision_heading(const std::string &h) {
    const std::string x = lower_ascii(h);
    for (const char *w : {"decision", "action", "instruction", "next step", "to-do", "todo", "to do", "follow-up",
                          "follow up", "నిర్ణయ", "చర్య", "సూచన", "పనులు", "చేయవలసిన", "చేయాల్సిన"})
        if (x.find(w) != std::string::npos) return true;
    return false;
}

// The fields of an action line that are not what was said: "— me — not stated" / "(owner: not stated)".
std::string without_empty_fields(const std::string &line) {
    std::string out;
    size_t start = 0;
    const std::string dash = "\xE2\x80\x94";   // —
    std::vector<std::string> fields;
    while (true) {
        size_t k = line.find(dash, start);
        if (k == std::string::npos) { fields.push_back(line.substr(start)); break; }
        fields.push_back(line.substr(start, k - start));
        start = k + dash.size();
    }
    for (auto &f : fields) {
        const std::string x = lower_ascii(trim(f));
        if (x.empty() || x.find("not stated") != std::string::npos || x == "me" || x == "unknown" ||
            x == "not said" || x == "unspecified")
            continue;
        out += (out.empty() ? "" : " ") + trim(f);
    }
    return out;
}

std::string quoted_list(const std::vector<std::string> &items, size_t max_items = 6) {
    std::string out;
    for (size_t i = 0; i < items.size() && i < max_items; ++i) out += "- \"" + items[i] + "\"\n";
    return out;
}

std::string piece_lines(const json &pieces, size_t from, size_t to) {
    std::string out;
    for (size_t k = from; k < pieces.size() && k <= to; ++k)
        out += "[t=" + std::to_string(jint(pieces[k], "start_sec", 0)) + "] " + jstr(pieces[k], "text") + "\n";
    return out;
}

int yes_no(const ModelFn &judge, const std::string &phase, const std::string &prompt) {
    std::string reply;
    if (!judge || !judge(phase, skills::system(skills::Skill::verify), prompt, 4, reply)) return -1;
    return parse_yes_no(trim(reply));
}

// "Copy the line that says it" for a memory or action whose quote is not in its recording: the quote found again
// (start_sec of its piece), or false.
// 1 found, 0 not found, -1 the call failed (unmeasured: never flags).
int find_line_again(const ModelFn &judge, const std::string &what, int64_t near_sec, const json &pieces,
                    std::string &quote, int64_t &start_sec) {
    if (!judge || !pieces.is_array() || pieces.empty()) return -1;
    size_t at = 0;
    for (size_t i = 0; i < pieces.size(); ++i)
        if (jint(pieces[i], "start_sec", 0) <= near_sec) at = i;
    const size_t from = at >= 4 ? at - 4 : 0;
    std::string ctx = near_sec >= 0 ? piece_lines(pieces, from, at + 4) : judge_context(what, pieces);
    const std::string prompt = "Transcript lines (each starts with its [t=SEC] marker):\n" + ctx +
                               "\nSomething taken from this recording: \"" + what + "\"\n\nCopy the words of the one "
                               "transcript line that says it, exactly as written there, without the marker. Reply with "
                               "those words only, or NONE when no line says it.";
    std::string reply;
    if (!judge("verify_quote", skills::system(skills::Skill::verify), prompt, 120, reply)) return -1;
    std::string q = trim(reply);
    if (q.size() >= 2 && q.front() == '"' && q.back() == '"') q = q.substr(1, q.size() - 2);
    if (q.rfind("[t=", 0) == 0) { size_t e = q.find(']'); if (e != std::string::npos) q = trim(q.substr(e + 1)); }
    if (q.empty() || lower_ascii(q).rfind("none", 0) == 0) return 0;
    const QuoteHit h = locate_quote(pieces, q, -1);
    if (!h.found) return 0;
    quote = q;
    start_sec = h.start_sec;
    return 1;
}

// "Ravi: ..." -> "Ravi" (a label of one word before a colon near the start), else "".
std::string speaker_of(const std::string &text) {
    const size_t colon = text.find(':');
    if (colon == std::string::npos || colon == 0 || colon >= 40) return "";
    const std::string label = trim(text.substr(0, colon));
    if (label.empty() || label.find(' ') != std::string::npos) return "";
    return label;
}

}  // namespace

// ---- code checks ---------------------------------------------------------------------------------------------

double telugu_share(const std::string &text) {
    size_t te = 0, all = 0;
    for (size_t i = 0; i < text.size();) {
        const uint32_t cp = next_cp(text, i);
        if (!is_letter(cp)) continue;
        ++all;
        te += is_telugu_letter(cp);
    }
    return all ? (double)te / (double)all : 0.0;
}

bool in_language(const std::string &text, const std::string &lang) {
    // The notes' template headings ("## Key points") and "Q1."/"A1." labels are written in English in every
    // language: only what is under them counts.
    std::string body;
    for (const auto &raw : lines_of(text)) {
        const std::string l = trim(raw);
        if (l.empty() || l[0] == '#') continue;
        body += l + "\n";
    }
    if (letters(body) < 12) return true;
    const double te = telugu_share(body);
    return lang == "te" ? te >= 0.3 : te <= 0.3;
}

std::vector<std::string> decision_action_lines(const std::string &notes) {
    std::vector<std::string> out;
    bool in = false;
    for (const auto &raw : lines_of(notes)) {
        const std::string l = trim(raw);
        if (l.empty()) continue;
        if (l[0] == '#') {
            size_t h = 0;
            while (h < l.size() && l[h] == '#') ++h;
            in = decision_heading(l.substr(h));
            continue;
        }
        // "**Decisions:**" as a bold label on its own line
        if (l.size() > 4 && l.rfind("**", 0) == 0 && l.find("**", 2) != std::string::npos && l.find("**", 2) + 2 >= l.size() - 1) {
            in = decision_heading(l);
            continue;
        }
        if (!in) continue;
        const std::string b = without_empty_fields(bullet_text(l));
        if (says_none(b) || b.empty()) continue;
        out.push_back(b);
    }
    return out;
}

// Whether a clause is negated: "not", "never", "didn't", "no one"; Telugu "లేదు", "కాదు", "వద్దు".
bool negated(const std::string &clause) {
    static const std::set<std::string> words = {"not", "no", "never", "nobody", "nothing", "neither", "nor",
                                                "cannot", "dont", "didnt", "doesnt", "wont", "cant",
                                                "isnt", "arent", "wasnt", "werent", "havent", "hasnt", "hadnt",
                                                "shouldnt", "wouldnt", "couldnt", "mustnt"};
    std::string low;
    for (char c : clause) low += (unsigned char)c < 0x80 ? (char)std::tolower((unsigned char)c) : c;
    if (low.find("n't") != std::string::npos || low.find("n\u2019t") != std::string::npos) return true;
    for (const char *te : {"లేదు", "కాదు", "వద్దు", "లేరు", "లేవు", "కూడదు"})
        if (low.find(te) != std::string::npos) return true;
    std::string word;
    for (size_t i = 0; i <= low.size(); ++i) {
        const char c = i < low.size() ? low[i] : ' ';
        if (std::isalpha((unsigned char)c) || c == '\'') { if (c != '\'') word += c; continue; }
        if (words.count(word)) return true;
        word.clear();
    }
    return false;
}

// The clauses of the transcript: each piece split at , . ; : ! ? and "but".
std::vector<std::string> clauses_of(const json &pieces) {
    std::vector<std::string> out;
    if (!pieces.is_array()) return out;
    for (const auto &p : pieces) {
        std::string text = jstr(p, "text"), cur;
        for (size_t i = 0; i <= text.size(); ++i) {
            const bool end = i == text.size() || std::strchr(",.;:!?", text[i]) ||
                             (text.compare(i, 5, " but ") == 0);
            if (!end) { cur += text[i]; continue; }
            if (!trim(cur).empty()) out.push_back(trim(cur));
            cur.clear();
            if (i < text.size() && text[i] == ' ') i += 3;   // past " but"
        }
    }
    return out;
}

// Words that leave a matter open: pending, proposed, asked, not decided yet ("Approval is still pending").
bool undecided(const std::string &clause) {
    std::string low;
    for (char c : clause) low += (unsigned char)c < 0x80 ? (char)std::tolower((unsigned char)c) : c;
    if (low.find('?') != std::string::npos) return true;
    static const char *marks[] = {"pending", "not yet", "still open", "propos", "suggest", "maybe", "might ",
                                  "consider", "should we", "what if", "not decided", "undecided", "to be decided",
                                  "tbd", "awaiting", "waiting for", "not clear", "unclear", "will decide",
                                  "provisional", "tentative", "not final", "on hold", "interim", "subject to",
                                  "conditional", "తాత్కాలిక",
                                  "ఇంకా", "పెండింగ్", "ప్రతిపాద", "ఆలోచిద్దాం", "నిర్ణయించలేదు"};
    for (const char *m : marks)
        if (low.find(m) != std::string::npos) return true;
    return false;
}

// A line that states as settled what a transcript clause about it leaves open: the line has no "open" words and is
// not negated, and a clause sharing one of its content words (names left aside: they match too widely) says it is
// pending, proposed or asked. "Priya approved the budget" against "Approval is still pending".
bool undecided_conflict(const std::string &line, const json &pieces) {
    if (undecided(line) || negated(line)) return false;
    std::vector<std::string> w;
    for (const auto &t : content_tokens(line)) {
        bool name = false;
        if (pieces.is_array())
            for (const auto &p : pieces) {
                const std::string sp = speaker_of(jstr(p, "text"));
                if (!sp.empty() && match_tokens(sp) == std::vector<std::string>{t}) { name = true; break; }
            }
        if (!name && !is_number(t)) w.push_back(t);
    }
    if (w.empty()) return false;
    for (const auto &c : clauses_of(pieces)) {
        if (!undecided(c)) continue;
        for (const auto &u : match_tokens(c))
            for (const auto &t : w)
                if (t == u || same_word(t, u)) return true;
    }
    return false;
}

bool polarity_conflict(const std::string &line, const json &pieces) {
    const std::vector<std::string> w = content_tokens(line);
    if (w.size() < 2) return false;
    size_t best = 0;
    std::vector<bool> best_neg;
    for (const auto &c : clauses_of(pieces)) {
        const std::vector<std::string> ct = match_tokens(c);
        size_t n = 0;
        for (const auto &t : w)
            for (const auto &u : ct)
                if (t == u || same_word(t, u)) { ++n; break; }
        if (n == 0 || n < best) continue;
        if (n > best) { best = n; best_neg.clear(); }
        best_neg.push_back(negated(c));
    }
    if (best * 2 < w.size()) return false;   // no clause says it closely enough to compare
    const bool line_neg = negated(line);
    for (bool n : best_neg)
        if (n == line_neg) return false;
    return true;
}

int said_check(const std::string &line, const json &pieces) {
    const std::vector<std::string> w = content_tokens(line);
    const Said s = said_tokens(pieces);
    // A number never said is never "said", however short the line ("9000" as a whole answer).
    for (const auto &t : w)
        if (is_number(t) && !said_word(t, s)) return -1;
    if (w.size() < 2) return 1;
    size_t found = 0;
    for (const auto &t : w) found += said_word(t, s);
    if (found * 4 < w.size() * 3) return 0;
    return polarity_conflict(line, pieces) || undecided_conflict(line, pieces) ? -2 : 1;
}

std::vector<std::string> summary_claims(const std::string &summary) {
    std::vector<std::string> out;
    for (const auto &raw : lines_of(summary)) {
        const std::string l = trim(raw);
        if (l.empty() || l[0] == '#') continue;
        std::string body = bullet_text(l), cur;
        for (size_t i = 0; i <= body.size(); ++i) {
            const bool end = i == body.size() || ((body[i] == '.' || body[i] == '!' || body[i] == '?') &&
                                                  (i + 1 == body.size() || body[i + 1] == ' '));
            if (!end) { cur += body[i]; continue; }
            if (i < body.size()) cur += body[i];
            const std::string c = trim(cur);
            cur.clear();
            if (content_tokens(c).size() >= 2) out.push_back(c);
        }
    }
    return out;
}

int64_t evidence_sec(const std::string &claim, const json &pieces) {
    if (!pieces.is_array()) return -1;
    const std::vector<std::string> w = content_tokens(claim);
    if (w.empty()) return -1;
    size_t best = 0;
    int64_t sec = -1;
    for (const auto &p : pieces) {
        const std::vector<std::string> pt = match_tokens(jstr(p, "text"));
        size_t n = 0;
        for (const auto &t : w)
            for (const auto &u : pt)
                if (t == u || same_word(t, u)) { ++n; break; }
        if (n > best) { best = n; sec = jint(p, "start_sec", -1); }
    }
    return best * 2 >= w.size() || best >= 2 ? sec : -1;
}

std::vector<std::pair<Claim, Claim>> conflicting_claims(const std::vector<Claim> &claims) {
    std::vector<std::pair<Claim, Claim>> out;
    std::vector<std::vector<std::string>> toks;
    for (const auto &c : claims) toks.push_back(content_tokens(c.text));
    for (size_t i = 0; i < claims.size(); ++i)
        for (size_t j = i + 1; j < claims.size(); ++j) {
            if (claims[i].part == claims[j].part && claims[i].id == claims[j].id) continue;
            if (claims[i].part == claims[j].part && claims[i].part != "memory") continue;   // one output: its own business
            const auto &a = toks[i], &b = toks[j];
            const size_t shorter = std::min(a.size(), b.size());
            if (shorter < 2) continue;
            size_t shared = 0;
            for (const auto &t : a)
                for (const auto &u : b)
                    if (t == u || same_word(t, u)) { ++shared; break; }
            if (shared < 2 || shared * 10 < shorter * 6) continue;
            const bool open_a = undecided(claims[i].text), open_b = undecided(claims[j].text);
            if (negated(claims[i].text) != negated(claims[j].text) || open_a != open_b) out.push_back({claims[i], claims[j]});
        }
    return out;
}

bool said_in(const std::string &line, const json &pieces) { return said_check(line, pieces) == 1; }

std::string wrong_owner(const std::string &line, const json &pieces) {
    if (!pieces.is_array()) return "";
    std::vector<std::string> speakers;
    for (const auto &p : pieces) {
        const std::string sp = speaker_of(jstr(p, "text"));
        if (!sp.empty() && std::find(speakers.begin(), speakers.end(), sp) == speakers.end()) speakers.push_back(sp);
    }
    if (speakers.size() < 2) return "";
    const std::vector<std::string> lt = match_tokens(line);
    // The speakers the line names (as a whole token).
    std::vector<std::string> named;
    for (const auto &sp : speakers) {
        const std::vector<std::string> st = match_tokens(sp);
        if (st.size() == 1 && std::find(lt.begin(), lt.end(), st[0]) != lt.end()) named.push_back(sp);
    }
    if (named.size() != 1) return "";
    // The piece that shares the most content words with the line, the names left out.
    std::vector<std::string> w;
    for (const auto &t : content_tokens(line)) {
        bool is_name = false;
        for (const auto &sp : speakers) is_name = is_name || match_tokens(sp) == std::vector<std::string>{t};
        if (!is_name) w.push_back(t);
    }
    if (w.size() < 2) return "";
    long best = -1;
    size_t best_n = 0;
    bool tie = false;
    for (size_t i = 0; i < pieces.size(); ++i) {
        const std::vector<std::string> pt = match_tokens(jstr(pieces[i], "text"));
        size_t n = 0;
        for (const auto &t : w)
            for (const auto &u : pt)
                if (same_word(t, u)) { ++n; break; }
        if (n > best_n) { best = (long)i; best_n = n; tie = false; }
        else if (n == best_n && n > 0) tie = true;
    }
    if (best < 0 || tie || best_n * 2 < w.size()) return "";
    const std::string text = jstr(pieces[(size_t)best], "text");
    const std::string sp = speaker_of(text);
    if (sp.empty() || sp == named[0]) return "";
    // The speaker gave it to the person named ("Kiran, please order..."): not wrong.
    const std::vector<std::string> said = match_tokens(text.substr(text.find(':') + 1));
    if (std::find(said.begin(), said.end(), match_tokens(named[0])[0]) != said.end()) return "";
    return sp;
}

std::vector<QA> parse_qa(const std::string &questions) {
    std::vector<QA> out;
    for (const auto &p : question_pairs(questions)) out.push_back({p.first, p.second});
    return out;
}

int said_word_count(const json &pieces, const std::vector<std::string> &vocab) {
    int n = 0;
    std::string prev;
    if (!pieces.is_array()) return 0;
    for (const auto &p : pieces) {
        const std::string raw = jstr(p, "text");
        const std::string c = clean_piece(strip_unclear_tags(raw), prev, vocab);
        prev = raw;
        for (const auto &t : match_tokens(c)) n += !t.empty();
    }
    return n;
}

std::vector<std::string> noise_terms(const json &pieces, const std::vector<std::string> &vocab) {
    std::vector<std::string> noise_terms;
    if (pieces.is_array() && !vocab.empty()) {
        std::string kept, dropped, prev;
        for (const auto &p : pieces) {
            const std::string raw = strip_unclear_tags(jstr(p, "text"));
            (clean_piece(raw, prev, vocab).empty() ? dropped : kept) += raw + "\n";
            prev = raw;
        }
        const std::vector<std::string> kt = match_tokens(kept), dt = match_tokens(dropped);
        for (const auto &v : vocab) {
            const std::vector<std::string> vw = match_tokens(v);
            if (vw.empty()) continue;
            auto has = [&](const std::vector<std::string> &hay) {
                for (size_t i = 0; i + vw.size() <= hay.size(); ++i)
                    if (std::equal(vw.begin(), vw.end(), hay.begin() + (long)i)) return true;
                return false;
            };
            if (has(dt) && !has(kt)) noise_terms.push_back(v);
        }
    }
    return noise_terms;
}

json clean_pieces(const json &pieces, const std::vector<std::string> &vocab) {
    json out = json::array();
    if (!pieces.is_array()) return out;
    std::string prev;
    for (const auto &p : pieces) {
        const std::string raw = jstr(p, "text");
        const std::string c = clean_piece(raw, prev, vocab);
        prev = raw;
        if (c.empty()) continue;
        json q = p;
        q["text"] = c;
        out.push_back(q);
    }
    return out;
}

std::vector<std::string> noise_lines(const std::string &output, const json &pieces, const std::vector<std::string> &vocab) {
    std::vector<std::string> out;
    // Saved words the transcript says only in pieces clean_piece drops.
    const std::vector<std::string> noise_terms = sve::noise_terms(pieces, vocab);
    std::map<std::string, int> seen;
    for (const auto &raw : lines_of(output)) {
        const std::string l = trim(raw);
        if (l.empty() || l[0] == '#') continue;
        const std::string body = bullet_text(l);
        if (body.empty() || says_none(body)) continue;
        std::string key;
        for (const auto &t : match_tokens(body)) key += t + " ";
        bool bad = false;
        if (body.size() >= 12 && ++seen[key] == 2) bad = true;   // the same line again: a loop
        if (!bad && body.size() >= 24 && collapse_repeats(body, 3).size() * 10 < body.size() * 8) bad = true;
        if (!bad) {
            const std::vector<std::string> bt = match_tokens(body);
            for (const auto &v : noise_terms) {
                const std::vector<std::string> vw = match_tokens(v);
                for (size_t i = 0; !bad && i + vw.size() <= bt.size(); ++i)
                    if (std::equal(vw.begin(), vw.end(), bt.begin() + (long)i)) bad = true;
            }
        }
        if (bad) out.push_back(body);
    }
    return out;
}

std::string judge_context(const std::string &item, const json &pieces, size_t max_bytes) {
    if (!pieces.is_array() || pieces.empty()) return "";
    const std::string all = piece_lines(pieces, 0, pieces.size() - 1);
    if (all.size() <= max_bytes) return all;
    const std::vector<std::string> w = content_tokens(item);
    std::vector<std::pair<int, size_t>> score;
    for (size_t i = 0; i < pieces.size(); ++i) {
        const std::vector<std::string> pt = match_tokens(jstr(pieces[i], "text"));
        int s = 0;
        for (const auto &t : w)
            for (const auto &u : pt)
                if (same_word(t, u)) { ++s; break; }
        score.push_back({s, i});
    }
    std::stable_sort(score.begin(), score.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    std::set<size_t> take;
    for (size_t k = 0; k < score.size() && k < 3; ++k) {
        const size_t i = score[k].second;
        for (size_t j = i >= 2 ? i - 2 : 0; j <= i + 2 && j < pieces.size(); ++j) take.insert(j);
    }
    std::string out;
    for (size_t j : take) {
        const std::string l = piece_lines(pieces, j, j);
        if (out.size() + l.size() > max_bytes) break;
        out += l;
    }
    return out;
}

// ---- the parent ------------------------------------------------------------------------------------------------

std::vector<Failure> check_output(const std::string &part, const std::string &text, const VerifyInput &in,
                                  const ModelFn &judge, int &judge_calls, int &unjudged) {
    std::vector<Failure> f;
    if (trim(text).empty()) return f;
    const std::string lname = language_name(in.lang);
    if (!in_language(text, in.lang)) f.push_back({"language", "not written in " + lname});

    // From noise: too little said for any output, then the noise lines.
    if (said_word_count(in.pieces, in.vocab) < 4) {
        f.push_back({"noise", "the recording has fewer than 4 clear words"});
        return f;
    }
    for (const auto &l : noise_lines(text, in.pieces, in.vocab)) f.push_back({"noise", l});

    auto judged = [&](const std::string &phase, const std::string &prompt) -> int {
        if (!judge) return 1;
        if (judge_calls >= kMaxJudgeCalls) { ++unjudged; return 1; }   // not measured: never a failure
        ++judge_calls;
        const int a = yes_no(judge, phase, prompt);
        if (a < 0) { ++unjudged; return 1; }   // the call failed or the reply is not yes/no: unmeasured, never a failure
        return a;
    };
    // A line whose closest transcript clause says the opposite ("did not approve" / "approved") has evidence
    // against it: only a "yes" from the judge clears it; no judge, no budget or no clear reply flags it.
    auto cleared = [&](const std::string &phase, const std::string &prompt) -> bool {
        if (!judge || judge_calls >= kMaxJudgeCalls) return false;
        ++judge_calls;
        return yes_no(judge, phase, prompt) == 1;
    };
    const std::string same_meaning =
        "\n\nDo the transcript lines say this, in any language, with the same meaning? Answer no if they say the "
        "opposite (for example \"did not approve\" against \"approved\"), or only propose, ask about or leave pending "
        "what the line states as decided, done or agreed. Answer yes or no.";
    // A claim against the transcript: every number said, the words said (else the judge), the same meaning.
    auto check_claim = [&](const std::string &l, const char *what) {
        const int code = said_check(l, in.pieces);
        if (code == 1) return;
        const int64_t sec = evidence_sec(l, in.pieces);
        if (code == -1) { f.push_back({"grounded", l, sec}); return; }
        const std::string prompt = "Transcript lines:\n" + judge_context(l, in.pieces) + "\nA line of the " + what +
                                   ": \"" + l + "\"" + same_meaning;
        if (code == -2) {
            if (!cleared("verify_meaning", prompt)) f.push_back({"meaning", l, sec});
            return;
        }
        if (judged("verify_grounded", prompt) != 1) f.push_back({"grounded", l, sec});
    };
    if (part == "summary")
        for (const auto &l : summary_claims(text)) check_claim(l, "summary");
    if (part == "notes") {
        for (const auto &l : decision_action_lines(text)) {
            const std::string by = wrong_owner(l, in.pieces);
            if (!by.empty()) { f.push_back({"owner", l + " (said by " + by + ")", evidence_sec(l, in.pieces)}); continue; }
            check_claim(l, "notes");
        }
    }
    if (part == "questions") {
        for (const auto &qa : parse_qa(text)) {
            const int code = said_check(qa.a, in.pieces);
            if (code == 1) continue;
            const int64_t sec = evidence_sec(qa.q + " " + qa.a, in.pieces);
            if (code == -1) { f.push_back({"answerable", qa.q, sec}); continue; }
            const std::string prompt = "Transcript lines:\n" + judge_context(qa.q + " " + qa.a, in.pieces) +
                                       "\nA revision question: \"" + qa.q + "\"\nIts answer: \"" + qa.a +
                                       "\"\n\nCan the question be answered from the transcript lines, with this answer "
                                       "and the same meaning (not its opposite)? Answer yes or no.";
            if (code == -2 ? !cleared("verify_meaning", prompt) : judged("verify_answerable", prompt) != 1)
                f.push_back({"answerable", qa.q, sec});
        }
    }
    return f;
}

std::string feedback_for(const std::string &part, const std::vector<Failure> &failures, const std::string &lang_name,
                         const std::vector<std::string> &not_said) {
    std::vector<std::string> grounded, noise, answerable, owner, meaning, consistency;
    bool language = false;
    for (const auto &x : failures) {
        if (x.check == "language") language = true;
        else if (x.check == "grounded") grounded.push_back(x.item);
        else if (x.check == "noise") noise.push_back(x.item);
        else if (x.check == "answerable") answerable.push_back(x.item);
        else if (x.check == "owner") owner.push_back(x.item);
        else if (x.check == "meaning") meaning.push_back(x.item);
        else if (x.check == "consistency") consistency.push_back(x.item);
    }
    std::string fb = "A check of your earlier " + part + " found problems. Write it again and fix them:\n";
    if (language) fb += "- Write all of it in " + lang_name + " (names and terms from other languages may stay, in brackets).\n";
    if (!grounded.empty())
        fb += "- These decisions or action items are not said in the transcript. Leave them out unless the "
              "transcript says them, and then write them as said:\n" + quoted_list(grounded);
    if (!meaning.empty())
        fb += "- These lines say the opposite of the transcript, or write as decided what was only proposed or asked. "
              "Write each one with the meaning the transcript gives it (\"did not approve\" stays not approved):\n" +
              quoted_list(meaning);
    if (!consistency.empty())
        fb += "- These lines disagree with another part of the notes about the same thing, and the transcript does not "
              "say them this way. Write each one as the transcript says it:\n" + quoted_list(consistency);
    if (!owner.empty())
        fb += "- These lines give the action or decision to the wrong person. Each line of the transcript starts with "
              "who said it; write who said or took it on as the transcript shows:\n" + quoted_list(owner);
    if (!noise.empty())
        fb += "- These lines repeat or come from noise (a loop, or words that were not said). Write each point once "
              "and leave these out:\n" + quoted_list(noise);
    if (!noise.empty() && !not_said.empty()) {
        fb += "- These words were never said (background noise wrote them); do not write them at all:";
        for (size_t i = 0; i < not_said.size(); ++i) fb += (i ? ", " : " ") + not_said[i];
        fb += "\n";
    }
    if (!answerable.empty())
        fb += "- These questions cannot be answered from the transcript. Replace them with questions the transcript "
              "answers:\n" + quoted_list(answerable);
    return fb;
}

namespace {

std::string check_names(const std::vector<Failure> &f) {
    std::set<std::string> s;
    for (const auto &x : f) s.insert(x.check);
    std::string out;
    for (const auto &x : s) out += (out.empty() ? "" : ",") + x;
    return out.empty() ? "all" : out;
}

}  // namespace

namespace {

const char *kSameMeaning =
    "\n\nDo the transcript lines say this, in any language, with the same meaning? Answer no if they say the opposite "
    "(for example \"did not approve\" against \"approved\"), or only propose, ask about or leave pending what the line "
    "states as decided, done or agreed. Answer yes or no.";

// Only a judge "yes" clears a claim with evidence against it (no judge, no budget or no clear reply: not cleared).
bool cleared_by(const ModelFn &judge, int &judge_calls, const std::string &phase, const std::string &prompt) {
    if (!judge || judge_calls >= kMaxJudgeCalls) return false;
    ++judge_calls;
    return yes_no(judge, phase, prompt) == 1;
}

std::vector<std::string> claims_of(const std::string &part, const std::string &text) {
    if (part == "summary") return summary_claims(text);
    if (part == "notes") return decision_action_lines(text);
    std::vector<std::string> out;
    if (part == "questions")
        for (const auto &qa : parse_qa(text)) {
            // The question asks; its answer is the claim. Without the "?", a question reads as an open matter.
            std::string q = qa.q;
            q.erase(std::remove(q.begin(), q.end(), '?'), q.end());
            out.push_back(q + " " + qa.a);
        }
    return out;
}

bool relationship_memory(const json &m) {
    const std::string t = jstr(m, "type");
    return t == "decision" || t == "question" || t == "promise";
}

// The claim does not match the transcript by code: the opposite meaning, or settled where it is open.
bool off_source(const std::string &claim, const json &pieces) {
    return polarity_conflict(claim, pieces) || undecided_conflict(claim, pieces);
}

}  // namespace

VerifyResult verify_outputs(const VerifyInput &in, const ModelFn &judge, const RewriteFn &rewrite, const EventFn &on_event) {
    VerifyResult r;
    r.summary = in.summary;
    r.notes = in.notes;
    r.questions = in.questions;
    auto emit = [&](const VerifyEvent &e) {
        r.events.push_back(e);
        if (on_event) on_event(e);
    };
    const std::string lname = language_name(in.lang);

    // The claims of every output but `part` (as kept so far) and the map's relationships.
    auto other_claims = [&](const std::string &part) {
        std::vector<Claim> out;
        for (const std::string p : {"summary", "notes", "questions"}) {
            if (p == part) continue;
            for (const auto &c : claims_of(p, p == "summary" ? r.summary : p == "notes" ? r.notes : r.questions))
                out.push_back({p, c, 0});
        }
        if (in.memories.is_array())
            for (const auto &m : in.memories)
                if (relationship_memory(m)) out.push_back({"memory", jstr(m, "text"), jint(m, "id", 0)});
        return out;
    };
    // Claims of `text` (output `part`) that another output contradicts and the transcript does not back.
    auto consistency = [&](const std::string &part, const std::string &text, const std::vector<Failure> &already) {
        std::vector<Failure> f;
        std::vector<Claim> all;
        for (const auto &c : claims_of(part, text)) all.push_back({part, c, 0});
        if (all.empty()) return f;
        const size_t own = all.size();
        for (const auto &c : other_claims(part)) all.push_back(c);
        std::set<std::string> done;
        for (const auto &x : already) done.insert(x.item);
        for (const auto &pr : conflicting_claims(all)) {
            const Claim *mine = pr.first.part == part ? &pr.first : pr.second.part == part ? &pr.second : nullptr;
            if (!mine || (&pr.first == mine ? &pr.second : &pr.first)->part == part) continue;
            if (done.count(mine->text)) continue;
            const Claim &theirs = &pr.first == mine ? pr.second : pr.first;
            // Failed only on evidence: the transcript backs the other output and not this one (by code). Otherwise the
            // disagreement is left to each claim's own source check: a hypothetical question ("if he had sent it
            // without...") or a paraphrase reads as a contradiction to word matching, and a judge asked about it
            // in isolation says no to anything not stated as such (measured 2026-11-26: false flags on 2 of 5).
            const bool mine_off = off_source(mine->text, in.pieces), theirs_off = off_source(theirs.text, in.pieces);
            if (!mine_off || theirs_off) continue;
            f.push_back({"consistency", mine->text, evidence_sec(mine->text, in.pieces)});
            done.insert(mine->text);
        }
        (void)own;
        return f;
    };
    auto with_consistency = [&](const std::string &part, const std::string &text, std::vector<Failure> f) {
        for (auto &x : consistency(part, text, f)) f.push_back(std::move(x));
        return f;
    };

    for (const std::string part : {"summary", "notes", "questions"}) {
        std::string &text = part == "summary" ? r.summary : part == "notes" ? r.notes : r.questions;
        if (trim(text).empty()) continue;
        PartVerdict v;
        v.part = part;
        // How many things the check measured (for the log): the output, its claims or its questions.
        auto measured = [&](const std::string &t) {
            return part == "notes" ? 1 + (int)decision_action_lines(t).size()
                 : part == "questions" ? 1 + (int)parse_qa(t).size() : 1;
        };
        std::vector<Failure> best = with_consistency(part, text, check_output(part, text, in, judge, r.judge_calls, r.unjudged));
        emit({part, check_names(best), "check", 0, measured(text), (int)best.size(), 0});
        // Too little said: writing it again cannot help.
        const bool hopeless = std::any_of(best.begin(), best.end(), [](const Failure &x) {
            return x.check == "noise" && x.item.find("fewer than 4") != std::string::npos;
        });
        std::string best_text = text;
        for (int attempt = 1; attempt <= kMaxRetries && !best.empty() && !hopeless && rewrite; ++attempt) {
            std::vector<std::string> failing;
            for (const auto &x : best) if (x.check != "language") failing.push_back(x.item);
            emit({part, check_names(best), "retry", attempt, 0, (int)best.size(), 0});
            v.retries = attempt;
            std::string out;
            if (!rewrite(part, attempt, v.chosen, feedback_for(part, best, lname, noise_terms(in.pieces, in.vocab)), failing, out) || trim(out).empty()) continue;
            std::vector<Failure> got = with_consistency(part, out, check_output(part, out, in, judge, r.judge_calls, r.unjudged));
            emit({part, check_names(got), "check", attempt, measured(out), (int)got.size(), 0});
            // Kept only when better: a language failure counts more than any other single line.
            auto weight = [](const std::vector<Failure> &f) {
                int w = 0;
                for (const auto &x : f) w += x.check == "language" ? 5 : 1;
                return w;
            };
            if (weight(got) < weight(best)) {
                best = got;
                best_text = out;
                v.chosen = attempt;
            }
        }
        text = best_text;
        v.failures = best;
        v.please_check = !best.empty();
        emit({part, check_names(best), v.please_check ? "please_check" : "kept", v.chosen, 0, (int)best.size(), 0});
        r.parts.push_back(v);
    }

    // Memories and action items: their quote in the recording; a worker copies the line again (at most twice).
    for (const std::string kind : {"memory", "action"}) {
        const json &items = kind == "memory" ? in.memories : in.actions;
        if (!items.is_array()) continue;
        int failed = 0, total = 0;
        for (const auto &m : items) {
            ++total;
            const int64_t id = jint(m, "id", 0);
            const QuoteHit h = locate_quote(in.pieces, jstr(m, "quote"), jint(m, "start_sec", -1));
            if (h.found) continue;
            ++failed;
            emit({kind, "quote", "check", 0, 1, 1, id});
            bool fixed = false, measured = false;
            for (int attempt = 1; attempt <= kMaxRetries && !fixed && judge && r.judge_calls < kMaxJudgeCalls; ++attempt) {
                emit({kind, "quote", "retry", attempt, 0, 1, id});
                ++r.judge_calls;
                std::string q;
                int64_t sec = -1;
                const int got = find_line_again(judge, jstr(m, "text"), jint(m, "start_sec", -1), in.pieces, q, sec);
                if (got < 0) continue;
                measured = true;
                if (got == 1) {
                    (kind == "memory" ? r.memory_fixes : r.action_fixes).push_back({{"id", id}, {"quote", q}, {"start_sec", sec}});
                    emit({kind, "quote", "fixed", attempt, 1, 0, id});
                    fixed = true;
                }
            }
            if (!fixed && !measured && judge) { ++r.unjudged; continue; }   // no answer at all: not flagged on a guess
            if (!fixed) {
                (kind == "memory" ? r.memories_flagged : r.actions_flagged).push_back(id);
                emit({kind, "quote", "please_check", 0, 1, 1, id});
            }
        }
        if (total > 0) emit({kind, "quote", "check", -1, total, failed, 0});   // the tally (attempt -1)
    }

    // The map's relationships, checked once on the memory each comes from (who decided, asked or promised what):
    // the same meaning as the transcript, the right person, and no output saying otherwise. A flagged one waits for
    // the person in the relations review with its reason and where it is said.
    if (in.memories.is_array()) {
        std::vector<Claim> outputs;
        for (const std::string p : {"summary", "notes", "questions"})
            for (const auto &c : claims_of(p, p == "summary" ? r.summary : p == "notes" ? r.notes : r.questions))
                outputs.push_back({p, c, 0});
        int total = 0, failed = 0;
        for (const auto &m : in.memories) {
            if (!relationship_memory(m)) continue;
            ++total;
            const int64_t id = jint(m, "id", 0);
            const std::string text = jstr(m, "text"), who = trim(jstr(m, "who")), type = jstr(m, "type");
            const int64_t sec = evidence_sec(text, in.pieces);
            std::string check;
            const int code = said_check(text, in.pieces);
            if (code == -1) check = "meaning";
            else if (code == -2 && !cleared_by(judge, r.judge_calls, "verify_relation",
                                                "Transcript lines:\n" + judge_context(text, in.pieces) + "\nA " + type +
                                                    " from this recording: \"" + text + "\"" + kSameMeaning))
                check = "meaning";
            if (check.empty() && !who.empty()) {
                const std::string lw = who;
                std::string low;
                for (char c : lw) low += (char)std::tolower((unsigned char)c);
                if (low != "me" && low != "i" && low != "unknown" && low != "someone") {
                    const std::string by = wrong_owner(who + " " + text, in.pieces);
                    const std::string verb = type == "decision" ? "decided" : type == "question" ? "asked" : "promised";
                    if (!by.empty() &&
                        !cleared_by(judge, r.judge_calls, "verify_relation",
                                    "Transcript lines:\n" + judge_context(text, in.pieces) + "\nDoes the transcript say that " +
                                        who + " " + verb + " this: \"" + text + "\"? Answer no if someone else did. Answer yes or no."))
                        check = "who";
                }
            }
            if (check.empty()) {
                std::vector<Claim> all = outputs;
                all.push_back({"memory", text, id});
                for (const auto &pr : conflicting_claims(all)) {
                    if (pr.first.part != "memory" && pr.second.part != "memory") continue;
                    const Claim &other = pr.first.part == "memory" ? pr.second : pr.first;
                    // On evidence only, as for the outputs: the transcript backs the output and not the relationship.
                    if (!off_source(text, in.pieces) || off_source(other.text, in.pieces)) continue;
                    check = "consistency";
                    break;
                }
            }
            if (check.empty()) continue;
            ++failed;
            r.relation_flags.push_back({{"memory_id", id}, {"check", check}, {"start_sec", sec}});
            if (std::find(r.memories_flagged.begin(), r.memories_flagged.end(), json(id)) == r.memories_flagged.end())
                r.memories_flagged.push_back(id);
            emit({"relation", check, "please_check", 0, 1, 1, id});
        }
        if (total > 0) emit({"relation", "all", "check", -1, total, failed, 0});
    }

    // The evidence of every checked claim: where the recording says it, and whether it passed.
    for (const auto &v : r.parts) {
        const std::string &text = v.part == "summary" ? r.summary : v.part == "notes" ? r.notes : r.questions;
        for (const auto &c : claims_of(v.part, text)) {
            if (r.evidence.size() >= kMaxEvidence) break;
            bool ok = true;
            for (const auto &f : v.failures)
                if (f.item == c || c.rfind(f.item, 0) == 0 || f.item.rfind(c, 0) == 0) ok = false;
            r.evidence.push_back({{"part", v.part}, {"item", c}, {"start_sec", evidence_sec(c, in.pieces)}, {"ok", ok}});
        }
    }
    if (in.memories.is_array())
        for (const auto &m : in.memories) {
            if (!relationship_memory(m) || r.evidence.size() >= kMaxEvidence) continue;
            const int64_t id = jint(m, "id", 0);
            bool ok = true;
            for (const auto &x : r.relation_flags) ok = ok && jint(x, "memory_id", 0) != id;
            r.evidence.push_back({{"part", "relation"}, {"item", jstr(m, "text")}, {"id", id},
                                  {"start_sec", evidence_sec(jstr(m, "text"), in.pieces)}, {"ok", ok}});
        }
    return r;
}

}  // namespace sve
