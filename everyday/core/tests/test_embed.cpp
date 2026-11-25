#include "check.h"
#include <cmath>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>
#include "sve_embed.h"
#include "sve_store.h"

using sve::Store;
using json = nlohmann::json;

namespace {

std::string tmp_db() {
    const char *dir = getenv("TMPDIR");
    std::string t = std::string(dir && *dir ? dir : "/tmp") + "/sve_emb_XXXXXX";
    std::vector<char> buf(t.begin(), t.end());
    buf.push_back('\0');
    int fd = mkstemp(buf.data());
    if (fd >= 0) close(fd);
    return std::string(buf.data());
}

void drop_db(const std::string &p) {
    for (const char *suf : {"", "-wal", "-shm"}) unlink((p + suf).c_str());
}

std::vector<float> vec(float a, float b = 0) {
    std::vector<float> v(sve::kEmbedDims, 0.f);
    v[0] = a;
    v[1] = b;
    return sve::embed_normalize(v, sve::kEmbedDims);
}

int64_t piece_ref(Store &s, int64_t note_id, int idx) {
    for (const auto &r : s.embed_pending("m", 1000))
        if (r["kind"] == "piece" && r["note_id"] == note_id && r["idx"] == idx) return r["ref"].get<int64_t>();
    return -1;
}

// A note with a transcript and two pieces.
int64_t note_with_pieces(Store &s, const char *title) {
    int64_t n = s.create(title, "en", nullptr, "text", 60);
    s.set_pieces(n, json::array({{{"start_sec", 0}, {"text", "first piece"}}, {{"start_sec", 30}, {"text", "second piece"}}}));
    return n;
}

}  // namespace

TEST(embed_normalize_truncates_and_normalises) {
    std::vector<float> v(768);
    for (size_t i = 0; i < v.size(); i++) v[i] = (float)(i % 7) - 3.f + 0.5f;
    std::vector<float> out = sve::embed_normalize(v, 256);
    CHECK_EQ(out.size(), (size_t)256);
    double n = 0;
    for (float x : out) n += (double)x * x;
    CHECK(std::fabs(std::sqrt(n) - 1.0) < 1e-5);
    // Direction of the first 256 values is kept.
    CHECK(out[0] < 0 && out[3] > 0);
    // Shorter than dims: kept at its length; all zeros stays zeros (no NaN).
    std::vector<float> z = sve::embed_normalize(std::vector<float>(10, 0.f), 256);
    CHECK_EQ(z.size(), (size_t)10);
    for (float x : z) CHECK(x == 0.f);
}

TEST(cosine_of_unit_vectors) {
    std::vector<float> a = vec(1, 0), b = vec(0, 1), c = vec(1, 1);
    CHECK(std::fabs(sve::cosine(a.data(), a.data(), a.size()) - 1.f) < 1e-5);
    CHECK(std::fabs(sve::cosine(a.data(), b.data(), a.size())) < 1e-5);
    CHECK(std::fabs(sve::cosine(a.data(), c.data(), a.size()) - std::sqrt(0.5f)) < 1e-5);
}

TEST(rrf_fuses_ranks) {
    auto r = sve::rrf({1, 2, 3}, {3, 4, 1});
    std::vector<int64_t> order;
    for (const auto &p : r) order.push_back(p.first);
    CHECK((order == std::vector<int64_t>{1, 3, 2, 4}));
    CHECK(std::fabs(r[0].second - (1.0 / 61 + 1.0 / 63)) < 1e-12);
    CHECK(std::fabs(r[3].second - 1.0 / 62) < 1e-12);
    // Ties go by keyword rank: 5 (keyword 1) and 6 (meaning 1) score the same.
    auto t = sve::rrf({5}, {6});
    CHECK(t.size() == 2 && t[0].first == 5 && t[1].first == 6);
    // No meaning list: the keyword order.
    auto k = sve::rrf({9, 7, 8}, {});
    CHECK(k.size() == 3 && k[0].first == 9 && k[1].first == 7 && k[2].first == 8);
}

TEST(fuse_search_keeps_keyword_order_without_meaning) {
    std::vector<sve::SearchHit> hits = sve::fuse_search({11, 12, 13}, {}, {5, 6}, {}, 10);
    CHECK_EQ(hits.size(), (size_t)5);
    // Each kind keeps its FTS order; kinds interleave by rank, pieces first on a tie.
    CHECK(hits[0].kind == "piece" && hits[0].ref == 11);
    CHECK(hits[1].kind == "memory" && hits[1].ref == 5);
    CHECK(hits[2].kind == "piece" && hits[2].ref == 12);
    CHECK(hits[3].kind == "memory" && hits[3].ref == 6);
    CHECK(hits[4].kind == "piece" && hits[4].ref == 13);
    CHECK_EQ(sve::fuse_search({1, 2, 3}, {}, {}, {}, 2).size(), (size_t)2);
    // A meaning-only piece joins the list.
    std::vector<sve::SearchHit> m = sve::fuse_search({1}, {2, 1}, {}, {}, 10);
    CHECK(m.size() == 2 && m[0].ref == 1 && m[1].ref == 2);
}

TEST(meaning_ranked_orders_by_cosine) {
    std::vector<std::pair<int64_t, std::vector<float>>> rows = {{1, vec(0, 1)}, {2, vec(1, 0)}, {3, vec(1, 1)}};
    std::vector<float> q = vec(1, 0.1f);
    std::vector<int64_t> r = sve::meaning_ranked(q, rows, 2);
    CHECK((r == std::vector<int64_t>{2, 3}));
    // A vector of another length never matches.
    rows.push_back({4, std::vector<float>(3, 1.f)});
    CHECK_EQ(sve::meaning_ranked(q, rows, 10).size(), (size_t)3);
}

TEST(vector_blob_round_trip) {
    std::vector<float> v = vec(0.3f, -0.7f);
    CHECK(sve::blob_vector(sve::vector_blob(v)) == v);
    CHECK(sve::blob_vector(std::string("abc")).empty());  // not a whole number of floats
}

TEST(embeddings_deleted_with_memory) {
    std::string p = tmp_db();
    {
        Store s(p);
        int64_t m = s.add_memory_item({{"type", "fact"}, {"text", "The locker code is in the blue diary"}, {"origin", "chat"}});
        int64_t keep = s.add_memory_item({{"type", "fact"}, {"text", "Ravi lives in Guntur"}, {"origin", "chat"}});
        CHECK(m > 0 && keep > 0);
        CHECK(s.set_embedding("memory", m, vec(1), "m", "The locker code is in the blue diary"));
        CHECK(s.set_embedding("memory", keep, vec(0, 1), "m", "Ravi lives in Guntur"));
        CHECK_EQ(s.embeddings("memory", "m").size(), (size_t)2);
        CHECK(s.delete_memory_item(m));
        auto left = s.embeddings("memory", "m");
        CHECK(left.size() == 1 && left[0].first == keep);
        // Editing the text makes the vector stale: it goes, and the memory is pending again.
        CHECK(s.update_memory_item(keep, {{"text", "Ravi lives in Vijayawada"}}));
        CHECK(s.embeddings("memory", "m").empty());
        bool pending = false;
        for (const auto &r : s.embed_pending("m", 100)) pending = pending || (r["kind"] == "memory" && r["ref"] == keep);
        CHECK(pending);
        // No vector for a memory that does not exist.
        CHECK(!s.set_embedding("memory", 99999, vec(1), "m", "x"));
        CHECK(s.embeddings("memory", "m").empty());
    }
    drop_db(p);
}

TEST(embeddings_deleted_with_pieces_note_and_wipe) {
    std::string p = tmp_db();
    {
        Store s(p);
        int64_t a = note_with_pieces(s, "A"), b = note_with_pieces(s, "B");
        int64_t a0 = piece_ref(s, a, 0), a1 = piece_ref(s, a, 1), b0 = piece_ref(s, b, 0), b1 = piece_ref(s, b, 1);
        CHECK(a0 > 0 && a1 > 0 && b0 > 0 && b1 > 0);
        CHECK(s.set_embedding("piece", a0, vec(1), "m", "first piece") && s.set_embedding("piece", a1, vec(1), "m", "second piece"));
        CHECK(s.set_embedding("piece", b0, vec(1), "m", "first piece") && s.set_embedding("piece", b1, vec(1), "m", "second piece"));
        CHECK_EQ(s.embeddings("piece", "m").size(), (size_t)4);
        CHECK(s.embed_pending("m", 100).empty());
        // Another model's vectors do not count.
        CHECK_EQ(s.embed_pending("other", 100).size(), (size_t)4);
        CHECK(s.embeddings("piece", "other").empty());

        // New pieces for a note: the old vectors go.
        s.set_pieces(a, json::array({{{"start_sec", 0}, {"text", "only piece"}}}));
        CHECK_EQ(s.embeddings("piece", "m").size(), (size_t)2);
        CHECK_EQ(s.embed_pending("m", 100).size(), (size_t)1);

        // Clearing the transcript drops the note's vectors and does not embed its pieces again.
        CHECK(s.clear_transcript(b));
        CHECK(s.embeddings("piece", "m").empty());
        for (const auto &r : s.embed_pending("m", 100)) CHECK(r["note_id"] != b);

        // Note delete.
        int64_t c = note_with_pieces(s, "C");
        int64_t c0 = piece_ref(s, c, 0);
        CHECK(s.set_embedding("piece", c0, vec(1), "m", "first piece"));
        CHECK(s.remove(c));
        CHECK(s.embeddings("piece", "m").empty());

        // Wipe.
        int64_t d = note_with_pieces(s, "D");
        CHECK(s.set_embedding("piece", piece_ref(s, d, 0), vec(1), "m", "first piece"));
        int64_t mem = s.add_memory_item({{"type", "fact"}, {"text", "x"}, {"origin", "chat"}});
        CHECK(s.set_embedding("memory", mem, vec(1), "m", "x"));
        CHECK(s.wipe_assistant());
        CHECK(s.embeddings("piece", "m").empty());
        CHECK(s.embeddings("memory", "m").empty());
    }
    drop_db(p);
}

TEST(embed_pending_newest_notes_first_and_current_memories_only) {
    std::string p = tmp_db();
    {
        Store s(p);
        int64_t old_n = note_with_pieces(s, "old"), new_n = note_with_pieces(s, "new");
        // Same created_at second: the higher id is newer.
        int64_t wrong = s.add_memory_item({{"type", "fact"}, {"text", "wrong one"}, {"origin", "chat"}, {"review", "wrong"}});
        int64_t queued = s.add_memory_item({{"type", "fact"}, {"text", "queued one"}, {"origin", "chat"}, {"queue", 1}});
        int64_t good = s.add_memory_item({{"type", "fact"}, {"text", "good one"}, {"origin", "chat"}});
        json rows = s.embed_pending("m", 100);
        CHECK_EQ(rows.size(), (size_t)5);
        CHECK(rows[0]["note_id"] == new_n && rows[1]["note_id"] == new_n);
        CHECK(rows[2]["note_id"] == old_n && rows[3]["note_id"] == old_n);
        CHECK(rows[0]["idx"] == 0 && rows[0]["text"] == "first piece");
        CHECK(rows[4]["kind"] == "memory" && rows[4]["ref"] == good && rows[4]["text"] == "good one");
        for (const auto &r : rows) if (r["kind"] == "memory") CHECK(r["ref"] != wrong && r["ref"] != queued);
        CHECK_EQ(s.embed_pending("m", 2).size(), (size_t)2);
        // The current memories with vectors only.
        CHECK(s.set_embedding("memory", wrong, vec(1), "m", "wrong one"));
        CHECK(s.set_embedding("memory", good, vec(1), "m", "good one"));
        auto e = s.embeddings("memory", "m");
        CHECK(e.size() == 1 && e[0].first == good);
        CHECK(s.embedding_count("m") == 2);
    }
    drop_db(p);
}

TEST(piece_refs_by_fts_and_rowid) {
    std::string p = tmp_db();
    {
        Store s(p);
        int64_t n = note_with_pieces(s, "A");
        json hits = s.search_piece_refs("second", 10);
        CHECK_EQ(hits.size(), (size_t)1);
        int64_t ref = hits[0]["ref"].get<int64_t>();
        CHECK(hits[0]["note_id"] == n && hits[0]["start_sec"] == 30);
        json got = s.piece_refs({ref, 424242});
        CHECK(got.size() == 1 && got[0]["ref"] == ref && got[0]["start_sec"] == 30 && got[0]["note_id"] == n);
        CHECK(s.search_memory_refs("nothing", 10).empty());
        int64_t m = s.add_memory_item({{"type", "fact"}, {"text", "Ravi lives in Guntur"}, {"origin", "chat"}});
        json mh = s.search_memory_refs("guntur", 10);
        CHECK(mh.size() == 1 && mh[0] == m);
    }
    drop_db(p);
}

TEST(embedding_refused_when_text_changed_meanwhile) {
    std::string p = tmp_db();
    {
        Store s(p);
        int64_t m = s.add_memory_item({{"type", "fact"}, {"text", "Ravi lives in Guntur"}, {"origin", "chat"}});
        int64_t n = note_with_pieces(s, "A");
        json pending = s.embed_pending("m", 100);
        CHECK_EQ(pending.size(), (size_t)3);
        // Meanwhile (another connection): the memory is edited and the note's pieces are written again; the new
        // piece reuses the old rowid.
        CHECK(s.update_memory_item(m, {{"text", "Ravi lives in Vijayawada"}}));
        s.set_pieces(n, json::array({{{"start_sec", 0}, {"text", "new words"}}, {{"start_sec", 30}, {"text", "more"}}}));
        for (const auto &r : pending)
            CHECK(!s.set_embedding(r["kind"].get<std::string>(), r["ref"].get<int64_t>(), vec(1), "m", r["text"].get<std::string>()));
        CHECK(s.embeddings("piece", "m").empty());
        CHECK(s.embeddings("memory", "m").empty());
        CHECK_EQ(s.embed_pending("m", 100).size(), (size_t)3);  // still pending, with the new text
        // With the text as it is now, it is stored.
        for (const auto &r : s.embed_pending("m", 100))
            CHECK(s.set_embedding(r["kind"].get<std::string>(), r["ref"].get<int64_t>(), vec(1), "m", r["text"].get<std::string>()));
        CHECK(s.embed_pending("m", 100).empty());
    }
    drop_db(p);
}
