// Plans the next piece of a 16 kHz WAV and reads sample ranges, for a speech model run outside the core.
// Pure: no model. Reads the WAV size at call time, so a file still being written is fine.
#include "sve_core.h"
#include "sve_audio.h"
#include "sve_whisper_text.h"

#include <cstdlib>
#include <cstring>
#include <vector>

int sve_plan_next_piece(const char *wav_path, int64_t start_sample, int is_final, int64_t *piece_end) {
    try {
        if (!wav_path || !piece_end || start_sample < 0) return -1;
        sve::WavInfo info;
        switch (sve::wav_probe(wav_path, info)) {
            case sve::WavProbe::ok: break;
            case sve::WavProbe::incomplete:
            case sve::WavProbe::bad:
                // While recording, a header not (fully) written yet is not an error.
                return is_final ? -1 : 1;
        }
        const uint64_t start = (uint64_t)start_sample, total = info.frames;
        const uint64_t window = sve::kPieceTarget + sve::kPieceSearch;   // 28 s + 3 s, as the planner needs
        if (!is_final && (total <= start || total - start < window)) return 1;
        std::vector<float> pcm;
        if (start < total && !sve::wav_read(wav_path, info, start, window, pcm)) return -1;
        // Planned in the window's own sample numbers.
        const sve::Group g = sve::plan_group(pcm.data(), (int64_t)pcm.size(), 0, is_final != 0);
        if (g.status == sve::Group::Wait) return 1;
        if (g.status == sve::Group::End) return 2;
        *piece_end = start_sample + g.end_sample;
        return 0;
    } catch (...) {
        return -1;
    }
}

int64_t sve_read_samples(const char *wav_path, int64_t start, int64_t end, float **out) {
    if (out) *out = nullptr;
    try {
        if (!wav_path || !out || start < 0 || end < start) return -1;
        sve::WavInfo info;
        if (sve::wav_probe(wav_path, info) != sve::WavProbe::ok) return -1;
        std::vector<float> pcm;
        if (!sve::wav_read(wav_path, info, (uint64_t)start, (uint64_t)(end - start), pcm)) return -1;
        float *p = (float *)std::malloc((pcm.empty() ? 1 : pcm.size()) * sizeof(float));
        if (!p) return -1;
        if (!pcm.empty()) std::memcpy(p, pcm.data(), pcm.size() * sizeof(float));
        *out = p;
        return (int64_t)pcm.size();
    } catch (...) {
        return -1;
    }
}

void sve_free_samples(float *p) { std::free(p); }
