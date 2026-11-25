// Meaning search, pure parts: vector maths, rank fusion and the stored vector format. The model calls that make the
// vectors (EmbeddingGemma, a second CPU-only model) are in sve_brain.cpp; with no embedder loaded the search is the
// keyword (FTS) order.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sve {

// Stored vector length: EmbeddingGemma's 768 values cut to the first 256 (Matryoshka), then L2-normalised.
constexpr size_t kEmbedDims = 256;
// EmbeddingGemma's prompts for retrieval: a question, and a stored passage.
constexpr const char *kEmbedQueryPrompt = "task: search result | query: ";
constexpr const char *kEmbedDocPrompt = "title: none | text: ";
// How many each search takes before fusion.
constexpr int kSearchKeywordTop = 20;
constexpr int kSearchMeaningTop = 20;

// The first `dims` values of v (all of v when shorter), scaled to length 1. An all-zero vector stays zero.
std::vector<float> embed_normalize(std::vector<float> v, size_t dims);
// Dot product over n values divided by both lengths; 0 when either is zero.
float cosine(const float *a, const float *b, size_t n);

// Reciprocal rank fusion: score(id) = sum over the lists it is in of 1/(k + rank), rank from 1. Sorted by score
// descending; ties by keyword rank (an id only in the meaning list after those in it), then by meaning rank.
std::vector<std::pair<int64_t, double>> rrf(const std::vector<int64_t> &keyword_ranked,
                                            const std::vector<int64_t> &meaning_ranked, int k = 60);

// The ids of rows by cosine to q, best first, at most top. Rows whose vector length differs from q's are skipped.
std::vector<int64_t> meaning_ranked(const std::vector<float> &q,
                                    const std::vector<std::pair<int64_t, std::vector<float>>> &rows, int top);

struct SearchHit {
    std::string kind;  // "piece" (ref = transcript_pieces rowid) or "memory" (ref = memories.id)
    int64_t ref = 0;
    double score = 0;
};
// Pieces and memories fused each by rrf, then merged by score (pieces first on a tie), at most limit. With empty
// meaning lists each kind keeps its keyword order and the two interleave by rank.
std::vector<SearchHit> fuse_search(const std::vector<int64_t> &piece_keyword, const std::vector<int64_t> &piece_meaning,
                                   const std::vector<int64_t> &memory_keyword, const std::vector<int64_t> &memory_meaning,
                                   int limit);

// Stored form of a vector: its floats as bytes (host order, little-endian on every target). blob_vector returns
// empty for bytes that are not a whole number of floats.
std::string vector_blob(const std::vector<float> &v);
std::vector<float> blob_vector(const std::string &bytes);

}  // namespace sve
