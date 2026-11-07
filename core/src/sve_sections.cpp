#include "sve_sections.h"

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

}  // namespace sve
