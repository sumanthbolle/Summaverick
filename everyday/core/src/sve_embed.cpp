#include "sve_embed.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace sve {

std::vector<float> embed_normalize(std::vector<float> v, size_t dims) {
    if (v.size() > dims) v.resize(dims);
    double n = 0;
    for (float x : v) n += (double)x * x;
    if (n <= 0) return v;
    const double inv = 1.0 / std::sqrt(n);
    for (float &x : v) x = (float)(x * inv);
    return v;
}

float cosine(const float *a, const float *b, size_t n) {
    double dot = 0, na = 0, nb = 0;
    for (size_t i = 0; i < n; i++) {
        dot += (double)a[i] * b[i];
        na += (double)a[i] * a[i];
        nb += (double)b[i] * b[i];
    }
    if (na <= 0 || nb <= 0) return 0.f;
    return (float)(dot / (std::sqrt(na) * std::sqrt(nb)));
}

std::vector<std::pair<int64_t, double>> rrf(const std::vector<int64_t> &keyword_ranked,
                                            const std::vector<int64_t> &meaning_ranked, int k) {
    struct Acc {
        double score = 0;
        size_t kw = SIZE_MAX, mn = SIZE_MAX;  // rank from 0; SIZE_MAX = not in that list
    };
    std::map<int64_t, Acc> acc;
    for (size_t i = 0; i < keyword_ranked.size(); i++) {
        Acc &a = acc[keyword_ranked[i]];
        if (a.kw != SIZE_MAX) continue;  // a repeated id counts once, at its best rank
        a.kw = i;
        a.score += 1.0 / (k + (double)(i + 1));
    }
    for (size_t i = 0; i < meaning_ranked.size(); i++) {
        Acc &a = acc[meaning_ranked[i]];
        if (a.mn != SIZE_MAX) continue;
        a.mn = i;
        a.score += 1.0 / (k + (double)(i + 1));
    }
    std::vector<std::pair<int64_t, Acc>> rows(acc.begin(), acc.end());
    std::stable_sort(rows.begin(), rows.end(), [](const auto &x, const auto &y) {
        if (x.second.score != y.second.score) return x.second.score > y.second.score;
        if (x.second.kw != y.second.kw) return x.second.kw < y.second.kw;
        return x.second.mn < y.second.mn;
    });
    std::vector<std::pair<int64_t, double>> out;
    out.reserve(rows.size());
    for (const auto &r : rows) out.push_back({r.first, r.second.score});
    return out;
}

std::vector<int64_t> meaning_ranked(const std::vector<float> &q,
                                    const std::vector<std::pair<int64_t, std::vector<float>>> &rows, int top) {
    std::vector<std::pair<float, int64_t>> scored;
    for (const auto &r : rows)
        if (r.second.size() == q.size() && !q.empty()) scored.push_back({cosine(q.data(), r.second.data(), q.size()), r.first});
    std::stable_sort(scored.begin(), scored.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    std::vector<int64_t> out;
    for (const auto &s : scored) {
        if ((int)out.size() >= top) break;
        out.push_back(s.second);
    }
    return out;
}

std::vector<SearchHit> fuse_search(const std::vector<int64_t> &piece_keyword, const std::vector<int64_t> &piece_meaning,
                                   const std::vector<int64_t> &memory_keyword, const std::vector<int64_t> &memory_meaning,
                                   int limit) {
    std::vector<SearchHit> out;
    for (const auto &p : rrf(piece_keyword, piece_meaning)) out.push_back({"piece", p.first, p.second});
    for (const auto &m : rrf(memory_keyword, memory_meaning)) out.push_back({"memory", m.first, m.second});
    // Stable: within a kind the rrf order stays, and pieces come before memories on a tie.
    std::stable_sort(out.begin(), out.end(), [](const SearchHit &a, const SearchHit &b) { return a.score > b.score; });
    if (limit >= 0 && out.size() > (size_t)limit) out.resize(limit);
    return out;
}

std::string vector_blob(const std::vector<float> &v) {
    std::string s(v.size() * sizeof(float), '\0');
    if (!v.empty()) std::memcpy(&s[0], v.data(), s.size());
    return s;
}

std::vector<float> blob_vector(const std::string &bytes) {
    if (bytes.size() % sizeof(float) != 0) return {};
    std::vector<float> v(bytes.size() / sizeof(float));
    if (!v.empty()) std::memcpy(v.data(), bytes.data(), bytes.size());
    return v;
}

}  // namespace sve
