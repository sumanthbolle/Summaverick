// Pure helpers for the Whisper Telugu path: where to cut audio, how to group pieces, repeat cleanup, language choice.
// No whisper.h and no model here, so all of it is testable on the Mac.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace sve {

struct Span { int64_t start, end; };   // samples at 16 kHz

struct Group {
    enum Status { Piece, Wait, End } status;
    std::vector<Span> spans;   // contiguous, each 4-10 s except a final tail
    int64_t end_sample;        // where the next group starts
};

// Plans the next group of pieces (up to 28 s in total) starting at start_sample. Pieces end at the quietest
// moment between 4 s and 10 s. Not final: Wait until 31 s are available. Final: End when under 0.5 s is left.
Group plan_group(const float *pcm, int64_t n_samples, int64_t start_sample, bool is_final);

// A word or phrase (1-3 words) repeated more than max_repeat times in a row is cut to one copy; text that is
// nothing but one such run becomes empty. Words are joined with single spaces.
std::string collapse_repeats(const std::string &text, int max_repeat = 3);

enum class LangChoice { Telugu, Gemma };
LangChoice lang_choice(float p_te, float p_en, float min_te = 0.6f, float margin = 0.3f);

}  // namespace sve
