// Two connections on one database file, as the iOS app uses them: one context runs the model (and the
// pipeline's writes), the other serves the screens' quick reads and writes. Neither may fail with
// SQLITE_BUSY or miss the other's committed rows.
#include "check.h"
#include <sqlite3.h>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <unistd.h>
#include "sve_store.h"

using sve::Store;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

std::string tmp_db() {
    const char *dir = getenv("TMPDIR");
    std::string t = std::string(dir && *dir ? dir : "/tmp") + "/sve_two_XXXXXX";
    std::vector<char> buf(t.begin(), t.end());
    buf.push_back('\0');
    int fd = mkstemp(buf.data());
    if (fd >= 0) close(fd);
    unlink(buf.data());  // a fresh file: the first Store creates it
    return std::string(buf.data());
}

void drop_db(const std::string &p) {
    for (const char *suf : {"", "-wal", "-shm"}) unlink((p + suf).c_str());
}

long ms_since(Clock::time_point t0) {
    return (long)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

bool has_note(Store &s, int64_t id) {
    for (const auto &n : s.list("", 100)) if (n.id == id) return true;
    return false;
}

}  // namespace

TEST(two_contexts_read_each_others_committed_rows) {
    std::string p = tmp_db();
    {
        Store model(p), screens(p);
        CHECK(model.ok());
        CHECK(screens.ok());

        // Notes: written on one, read (by id, in the list and by full-text search) on the other.
        int64_t a = model.create("Cardiology visit", "en", nullptr, "the doctor said walk daily", 600);
        CHECK(a > 0);
        CHECK(model.set_text_column(a, "short_summary", "Walk every day."));
        auto seen = screens.get(a);
        CHECK(seen.has_value());
        if (seen) CHECK(seen->short_summary == std::optional<std::string>("Walk every day."));
        CHECK(has_note(screens, a));
        CHECK_EQ(screens.list("walk", 10).size(), (size_t)1);

        int64_t b = screens.create("Shopping", "en", nullptr, nullptr, 0);
        CHECK(b > 0);
        CHECK(screens.set_text_column(b, "title", "Shopping list"));
        auto seen_b = model.get(b);
        CHECK(seen_b.has_value());
        if (seen_b) CHECK_EQ(seen_b->title, std::string("Shopping list"));

        // Assistant rows, both ways: a medicine (and its memory) from the screens, pieces and sections and a
        // recording memory from the model side, a review from the screens.
        int64_t med = screens.add_medicine(json::parse(
            R"({"name":"Amlodipine","dose":"5 mg","when_text":"after dinner","times":["20:00"],"repeat":"daily"})"));
        CHECK(med > 0);
        CHECK_EQ(model.medicines().size(), (size_t)1);
        bool med_memory = false;
        for (const auto &m : model.memory_items(json::object()))
            if (m["origin"] == "medicine" && m["medicine_id"] == med) med_memory = true;
        CHECK(med_memory);

        CHECK(model.set_pieces(a, json::parse(R"([{"start_sec":0,"text":"walk daily"}])")));
        CHECK_EQ(screens.pieces(a).size(), (size_t)1);
        CHECK(model.set_sections(a, json::parse(
            R"([{"idx":0,"first_piece":0,"last_piece":1,"start_sec":0,"end_sec":600,"summary":{"title":"Walk"}}])")));
        CHECK_EQ(screens.sections(a).size(), (size_t)1);

        CHECK(model.begin());
        int64_t mem = model.add_memory_item({{"type", "decision"}, {"text", "Walk daily"}, {"queue", 1}});
        CHECK(mem > 0);
        CHECK(model.add_memory_source(mem, a, 0, "walk daily", std::nullopt));
        CHECK(model.mark_memory_section(a, 0));
        CHECK(model.commit());
        CHECK_EQ(screens.memory_items(json{{"queue", 1}}).size(), (size_t)1);
        CHECK(screens.update_memory_item(mem, {{"review", "kept"}, {"queue", 0}}));
        json back = model.memory_item(mem);
        CHECK(!back.is_null() && back["review"] == "kept");

        // Settings and a delete made on the screens are seen by the model side.
        CHECK(screens.setting_set("k", "v"));
        CHECK(model.setting_get("k") == std::optional<std::string>("v"));
        CHECK(screens.remove(b));
        CHECK(!model.get(b).has_value());
        CHECK(!has_note(model, b));
    }
    drop_db(p);
}

TEST(a_writer_waits_for_the_other_contexts_write_instead_of_failing) {
    std::string p = tmp_db();
    {
        Store model(p), screens(p);
        CHECK(model.ok() && screens.ok());
        // The model side holds a write transaction for a while (as the pipeline does when it stores a section's
        // memories); a screen's write in that time waits, then lands.
        CHECK(model.begin());
        CHECK(model.create("held", "en", nullptr, nullptr, 0) > 0);
        int64_t written = -1;
        long waited = -1;
        std::thread screen([&] {
            auto t0 = Clock::now();
            written = screens.create("from the screen", "en", nullptr, nullptr, 0);
            waited = ms_since(t0);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(model.commit());
        screen.join();
        CHECK(written > 0);
        CHECK(waited >= 200);
        CHECK(waited < Store::kBusyTimeoutMs);
        CHECK(has_note(model, written));

        // A multi-row change (BEGIN IMMEDIATE) started on the screens while the model side reads and then writes:
        // both commit, in turn.
        auto rows = model.list("", 10);  // a read on the model side first
        CHECK_EQ(rows.size(), (size_t)2);
        CHECK(screens.begin());
        CHECK(screens.create("in a transaction", "en", nullptr, nullptr, 0) > 0);
        bool model_wrote = false;
        std::thread pipeline([&] { model_wrote = model.set_pieces(written, json::parse(R"([{"start_sec":0,"text":"x"}])")); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(screens.commit());
        pipeline.join();
        CHECK(model_wrote);
        CHECK_EQ(screens.pieces(written).size(), (size_t)1);
        CHECK_EQ(model.list("", 10).size(), (size_t)3);
    }
    drop_db(p);
}

TEST(a_second_open_of_an_up_to_date_file_writes_nothing) {
    std::string p = tmp_db();
    {
        Store first(p);
        CHECK(first.ok());
        CHECK(first.add_medicine(json::parse(R"({"name":"Metformin","dose":"500 mg","when_text":"after lunch"})")) > 0);
        CHECK(first.create("n", "en", nullptr, nullptr, 0) > 0);

        // Another connection holds the write lock the whole time: an open that needed to write would wait the
        // full busy timeout and then fail.
        sqlite3 *holder = nullptr;
        CHECK(sqlite3_open(p.c_str(), &holder) == SQLITE_OK);
        CHECK(sqlite3_exec(holder, "BEGIN IMMEDIATE; INSERT INTO settings(key,value) VALUES('held','1');", nullptr, nullptr,
                           nullptr) == SQLITE_OK);
        auto t0 = Clock::now();
        {
            Store second(p);
            CHECK(second.ok());
            CHECK_EQ(second.list("", 10).size(), (size_t)1);
            CHECK_EQ(second.medicines().size(), (size_t)1);
        }
        CHECK(ms_since(t0) < 1000);
        sqlite3_exec(holder, "ROLLBACK", nullptr, nullptr, nullptr);
        sqlite3_close(holder);

        // Exactly one medicine memory, however many connections opened.
        int n = 0;
        for (const auto &m : first.memory_items(json::object())) if (m["origin"] == "medicine") ++n;
        CHECK_EQ(n, 1);
    }
    drop_db(p);
}

TEST(two_contexts_opening_a_new_file_at_once_both_open) {
    for (int round = 0; round < 40; ++round) {  // the race shows in about 1 round in 7
        std::string p = tmp_db();
        bool ok_a = false, ok_b = false;
        std::thread a([&] { Store s(p); ok_a = s.ok() && s.create("a", "en", nullptr, nullptr, 0) > 0; });
        std::thread b([&] { Store s(p); ok_b = s.ok() && s.create("b", "en", nullptr, nullptr, 0) > 0; });
        a.join();
        b.join();
        CHECK(ok_a);
        CHECK(ok_b);
        Store s(p);
        CHECK(s.ok());
        CHECK_EQ(s.list("", 10).size(), (size_t)2);
        drop_db(p);
    }
}
