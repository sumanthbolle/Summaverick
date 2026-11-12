#include "sve_whisper_text.h"
#include <algorithm>
#include <sstream>

namespace sve {

static const int64_t kRate = 16000;
static const int64_t kFrame = 320;           // 20 ms
static const int64_t kMinLen = 4 * kRate;
static const int64_t kMaxLen = 10 * kRate;
static const int64_t kGroupMax = 28 * kRate;

Group plan_group(const float *pcm, int64_t n, int64_t start, bool is_final) {
    Group g{Group::Piece, {}, start};
    int64_t left = n - start;
    if (is_final ? left < kRate / 2 : left < kGroupMax + 3 * kRate) {
        g.status = is_final ? Group::End : Group::Wait;
        return g;
    }
    // Loudness per 20 ms frame (relative to start), smoothed over 300 ms, so a cut lands in a pause.
    int64_t n_fr = left / kFrame;
    std::vector<float> en(n_fr, 0.0f), sm(n_fr, 0.0f);
    for (int64_t i = 0; i < n_fr; i++) {
        double a = 0;
        for (int64_t j = 0; j < kFrame; j++) { float v = pcm[start + i * kFrame + j]; a += v * v; }
        en[i] = (float)a;
    }
    for (int64_t i = 0; i < n_fr; i++) {
        double a = 0; int c = 0;
        for (int64_t k = std::max<int64_t>(0, i - 7); k <= std::min(n_fr - 1, i + 7); k++) { a += en[k]; c++; }
        sm[i] = (float)(a / c);
    }
    int64_t pos = start;
    for (;;) {
        int64_t budget = kGroupMax - (pos - start);
        int64_t len = std::min({kMaxLen, budget, n - pos});
        if (len <= 0) break;
        if (len < n - pos) {
            // More audio follows this span, so it must fit the budget and may be cut at the quietest point.
            if (len < kMinLen) break;
            int64_t f0 = (pos - start + kMinLen) / kFrame;
            int64_t f1 = std::min((pos - start + len) / kFrame, n_fr - 1);
            int64_t best = f1;
            for (int64_t f = f0; f < f1; f++) if (sm[f] < sm[best]) best = f;
            int64_t cut = best * kFrame - (pos - start);
            if (cut >= kMinLen && cut <= len) len = cut;
        } else if (!is_final) {
            break;   // the rest is unfinished audio
        }
        if (len <= 0) break;
        g.spans.push_back({pos, pos + len});
        pos += len;
    }
    if (g.spans.empty()) { g.status = is_final ? Group::End : Group::Wait; return g; }
    g.end_sample = g.spans.back().end;
    return g;
}

std::string collapse_repeats(const std::string &text, int max_repeat) {
    std::vector<std::string> w;
    std::istringstream is(text);
    for (std::string s; is >> s;) w.push_back(s);
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < w.size() && !changed; i++) {
            for (size_t p = 1; p <= 3 && !changed; p++) {
                if (i + p > w.size()) break;
                size_t runs = 1;
                while (i + (runs + 1) * p <= w.size() &&
                       std::equal(w.begin() + i, w.begin() + i + p, w.begin() + i + runs * p)) runs++;
                if (runs > (size_t)max_repeat) {
                    // A run in the middle keeps one copy; a run that ends the text is a loop and goes entirely.
                    bool at_end = i + runs * p == w.size();
                    w.erase(w.begin() + i + (at_end ? 0 : p), w.begin() + i + runs * p);
                    changed = true;
                }
            }
        }
    }
    std::string out;
    for (auto &s : w) { if (!out.empty()) out += ' '; out += s; }
    return out;
}

LangChoice lang_choice(float p_te, float p_en, float min_te, float margin) {
    return (p_te >= min_te && p_te - p_en >= margin) ? LangChoice::Telugu : LangChoice::Gemma;
}

}  // namespace sve
