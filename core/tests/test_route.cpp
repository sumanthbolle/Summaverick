// Ask: the right source for each kind of question (N10): the code rule, the period range math, quiz, due,
// passage scoring, and the answer stream for summaries and general answers.
#include "check.h"
#include "sve_chat.h"
#include "sve_suggest.h"

#include <cstdlib>
#include <ctime>

using namespace sve;

namespace {
// Local time (the phone's time zone; India here) as epoch seconds.
int64_t local(int y, int mo, int d, int h = 0, int mi = 0) {
    setenv("TZ", "Asia/Kolkata", 1);
    tzset();
    struct tm t = {};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_isdst = -1;
    return (int64_t)mktime(&t);
}
const int64_t kNow = local(2026, 11, 11, 15, 30);  // Wednesday; the week began on Monday 9 November

void expect_range(const std::string &q, const char *kind, int64_t from, int64_t to) {
    Route r = route_question(q, kNow);
    if (r.kind != kind || r.from != from || r.to != to || !r.by_rule)
        std::fprintf(stderr, "route(%s) -> %s [%lld, %lld) rule=%d\n", q.c_str(), r.kind.c_str(), (long long)r.from,
                     (long long)r.to, (int)r.by_rule);
    CHECK_EQ(r.kind, std::string(kind));
    CHECK_EQ(r.from, from);
    CHECK_EQ(r.to, to);
    CHECK(r.by_rule);
}
std::string kind_of(const std::string &q, const std::vector<std::string> &names = {}) { return route_question(q, kNow, names).kind; }
}  // namespace

TEST(route_owner_phrasings) {
    // The owner's two failing questions on the phone.
    expect_range("Highlight this week key discussions to remember", "summary", local(2026, 11, 9), local(2026, 11, 16));
    CHECK_EQ(kind_of("Quiz me on my recent recordings"), std::string("quiz"));
    CHECK_EQ(kind_of("Quiz me on my recent recordings."), std::string("quiz"));
}

TEST(route_period_ranges_english) {
    expect_range("What did we talk about today?", "summary", local(2026, 11, 11), local(2026, 11, 12));
    expect_range("Yesterday's meetings", "summary", local(2026, 11, 10), local(2026, 11, 11));
    expect_range("Summarise this week's recordings.", "summary", local(2026, 11, 9), local(2026, 11, 16));
    expect_range("What happened last week?", "summary", local(2026, 11, 2), local(2026, 11, 9));
    expect_range("Key points this month", "summary", local(2026, 11, 1), local(2026, 12, 1));
    // highlight words with no period: the last 7 days, today included
    expect_range("Key discussions to remember", "summary", local(2026, 11, 5), local(2026, 11, 12));
    expect_range("Give me a recap", "summary", local(2026, 11, 5), local(2026, 11, 12));
    expect_range("highlights please", "summary", local(2026, 11, 5), local(2026, 11, 12));
    expect_range("What are the important points?", "summary", local(2026, 11, 5), local(2026, 11, 12));
}

TEST(route_period_ranges_telugu) {
    expect_range("ఈ వారం ముఖ్యమైన చర్చలు ఏమిటి?", "summary", local(2026, 11, 9), local(2026, 11, 16));
    expect_range("ఈ వారం రికార్డింగ్‌ల సారాంశం చెప్పండి.", "summary", local(2026, 11, 9), local(2026, 11, 16));
    expect_range("నిన్న ఏం మాట్లాడాం?", "summary", local(2026, 11, 10), local(2026, 11, 11));
    expect_range("ఈరోజు ఏమి జరిగింది?", "summary", local(2026, 11, 11), local(2026, 11, 12));
    expect_range("ఈ రోజు చర్చలు", "summary", local(2026, 11, 11), local(2026, 11, 12));
    expect_range("నేడు ఏమి చెప్పారు?", "summary", local(2026, 11, 11), local(2026, 11, 12));
    expect_range("గత వారం ఏమి జరిగింది?", "summary", local(2026, 11, 2), local(2026, 11, 9));
    expect_range("పోయిన వారం చర్చలు", "summary", local(2026, 11, 2), local(2026, 11, 9));
    expect_range("ఈ నెల ముఖ్యాంశాలు", "summary", local(2026, 11, 1), local(2026, 12, 1));
}

TEST(route_week_starts_monday_and_month_ends) {
    // On a Monday the week is that day on; on a Sunday it began six days before.
    Route mon = route_question("this week", local(2026, 11, 9, 8), {});
    CHECK_EQ(mon.from, local(2026, 11, 9));
    Route sun = route_question("this week", local(2026, 11, 15, 22), {});
    CHECK_EQ(sun.from, local(2026, 11, 9));
    CHECK_EQ(sun.to, local(2026, 11, 16));
    Route dec = route_question("this month", local(2026, 12, 31, 23), {});
    CHECK_EQ(dec.from, local(2026, 12, 1));
    CHECK_EQ(dec.to, local(2027, 1, 1));
    Route jan = route_question("yesterday", local(2027, 1, 1, 9), {});
    CHECK_EQ(jan.from, local(2026, 12, 31));
}

TEST(route_quiz_due_medicine) {
    CHECK_EQ(kind_of("test me"), std::string("quiz"));
    CHECK_EQ(kind_of("Next question"), std::string("quiz"));
    CHECK_EQ(kind_of("నా ఇటీవలి రికార్డింగ్‌ల మీద నాకు క్విజ్ పెట్టండి."), std::string("quiz"));
    CHECK_EQ(kind_of("తర్వాతి ప్రశ్న"), std::string("quiz"));
    CHECK_EQ(kind_of("Quiz me on this week's recordings"), std::string("quiz"));  // quiz before the period

    CHECK_EQ(kind_of("What's due soon?"), std::string("due"));
    CHECK_EQ(kind_of("What's due today?"), std::string("due"));  // due before the period
    CHECK_EQ(kind_of("Remind me what is coming up"), std::string("due"));
    CHECK_EQ(kind_of("Show my to-do list"), std::string("due"));
    CHECK_EQ(kind_of("what do I have tomorrow"), std::string("due"));
    CHECK_EQ(kind_of("త్వరలో ఏమేమి చేయాల్సి ఉంది?"), std::string("due"));
    CHECK(kind_of("What was the procedure?") != "due");   // whole words only

    // Medicine first: never a summary, a list of what is due, or general knowledge.
    CHECK_EQ(kind_of("Which tablets did the doctor give this week?"), std::string("medicine"));
    CHECK_EQ(kind_of("When is my Amlodipine due?", {"Amlodipine"}), std::string("medicine"));
    CHECK_EQ(kind_of("What is the dose of paracetamol?"), std::string("medicine"));
    CHECK_EQ(kind_of("నా మందులు ఏమిటి?"), std::string("medicine"));

    Route r = route_question("What did Ravi say about the pipe?", kNow);
    CHECK_EQ(r.kind, std::string("recordings"));
    CHECK(!r.by_rule);   // decided by retrieval, then the model
    CHECK(!route_question("Who wrote Gitanjali?", kNow).by_rule);
}

TEST(classify_reply) {
    CHECK(classify_general("G"));
    CHECK(classify_general(" g\n"));
    CHECK(classify_general("<|channel>thought\nR?<channel|>G"));
    CHECK(!classify_general("R"));
    CHECK(!classify_general(""));
    CHECK(!classify_general("I think"));
}

TEST(parse_revision_questions_pairs) {
    auto qa = parse_revision_questions(
        "Q1. What must be replaced?\nA1. The kitchen pipe.\n\n**Q2.** Who sends the quotation?\n**A2.** Ravi, by Friday\nevening.\n\n"
        "Q3. Unanswered?\n\nnot a question");
    CHECK_EQ(qa.size(), (size_t)2);
    if (qa.size() < 2) return;
    CHECK_EQ(qa[0].first, std::string("What must be replaced?"));
    CHECK_EQ(qa[0].second, std::string("The kitchen pipe."));
    CHECK_EQ(qa[1].first, std::string("Who sends the quotation?"));
    CHECK_EQ(qa[1].second, std::string("Ravi, by Friday evening."));
    CHECK(parse_revision_questions("").empty());
}

TEST(pick_quiz_unasked_newest_first) {
    json notes = json::parse(R"([
        {"id":7,"title":"Plumber","revision_questions":"Q1. What must be replaced?\nA1. The pipe.\nQ2. Take which tablet?\nA2. Dolo 650.\nQ3. Who sends the quote?\nA3. Ravi."},
        {"id":3,"title":"Class","revision_questions":"Q1. What is osmosis?\nA1. Water moving across a membrane."},
        {"id":2,"title":"Empty","revision_questions":null}])");
    auto a = pick_quiz(notes, {}, {});
    CHECK(a.has_value());
    if (!a) return;
    CHECK_EQ(a->id, std::string("7:1"));
    CHECK_EQ(a->question, std::string("What must be replaced?"));
    CHECK_EQ(a->answer, std::string("The pipe."));
    CHECK_EQ(a->note_id, (int64_t)7);
    CHECK_EQ(a->title, std::string("Plumber"));
    // a question about a medicine is never asked; asked ones are skipped
    auto b = pick_quiz(notes, {"7:1"}, {});
    CHECK(b && b->id == "7:3");
    auto c = pick_quiz(notes, {"7:1", "7:3"}, {});
    CHECK(c && c->id == "3:1");
    CHECK(!pick_quiz(notes, {"7:1", "7:3", "3:1"}, {}).has_value());
    CHECK_EQ(quiz_count(notes, {}), (size_t)3);
    CHECK_EQ(quiz_count(json::array(), {}), (size_t)0);
}

TEST(ymd_add_days_math) {
    CHECK_EQ(ymd_add_days("2026-11-11", 7), std::string("2026-11-18"));
    CHECK_EQ(ymd_add_days("2026-12-28", 7), std::string("2027-01-04"));
    CHECK_EQ(ymd_add_days("2028-02-27", 2), std::string("2028-02-29"));
    CHECK_EQ(ymd_add_days("2026-03-01", -1), std::string("2026-02-28"));
}

TEST(due_listing_in_code) {
    json tasks = json::parse(R"([
        {"id":1,"text":"Pay the electricity bill","due_date":"2026-11-13","who":null,"note_id":4,"start_sec":30},
        {"id":2,"text":"Send the report","due_date":"2026-11-09","who":"Ravi"},
        {"id":3,"text":"Someday","due_date":null},
        {"id":4,"text":"Too far","due_date":"2026-12-30"}])");
    json rems = json::parse(R"([
        {"id":1,"text":"Call the plumber","times":["09:00"],"repeat":"once","start_date":"2026-11-12","until_date":null},
        {"id":2,"text":"Walk","times":["06:30","18:00"],"repeat":"daily","start_date":"2026-11-01","until_date":null},
        {"id":3,"text":"Old","times":["10:00"],"repeat":"once","start_date":"2026-11-01"},
        {"id":4,"text":"Ended","times":["10:00"],"repeat":"daily","start_date":"2026-10-01","until_date":"2026-11-05"},
        {"id":5,"text":"Later","times":["10:00"],"repeat":"weekly","start_date":"2026-12-25"}])");
    json src = json::array();
    std::string en = due_answer(rems, tasks, "2026-11-11", "en", src);
    CHECK(en.find("Send the report") < en.find("Pay the electricity bill"));  // by date, overdue first
    CHECK(en.find("overdue") != std::string::npos);
    CHECK(en.find("Ravi") != std::string::npos);
    CHECK(en.find("Call the plumber") != std::string::npos && en.find("09:00") != std::string::npos);
    CHECK(en.find("Walk") != std::string::npos && en.find("06:30, 18:00") != std::string::npos);
    for (const char *gone : {"Someday", "Too far", "Old", "Ended", "Later"}) CHECK(en.find(gone) == std::string::npos);
    CHECK_EQ(src.size(), (size_t)1);
    if (!src.empty()) CHECK(src[0]["note_id"] == 4 && src[0]["start_sec"] == 30);

    json none = json::array();
    std::string empty = due_answer(json::array(), json::array(), "2026-11-11", "en", none);
    CHECK_EQ(empty, std::string("Nothing is due in the next 7 days."));
    std::string te = due_answer(json::array(), json::array(), "2026-11-11", "te", none);
    CHECK(te.find("7") != std::string::npos && te != empty);
}

TEST(summary_material_from_stored_summaries) {
    json notes = json::parse(R"([
        {"title":"Plumber visit","created_at":0,"short_summary":"The kitchen pipe must be replaced.","sections":[]},
        {"title":"Class","created_at":0,"short_summary":null,
         "sections":[{"summary":{"title":"[unclear] Osmosis","points":[{"text":"Water moves across a membrane."},{"text":"[unclear] It needs no energy."}]}}]},
        {"title":"Nothing stored","created_at":0,"short_summary":"","sections":[]}])");
    std::string m = summary_material(notes);
    CHECK(m.find("### Plumber visit") != std::string::npos && m.find("The kitchen pipe must be replaced.") != std::string::npos);
    CHECK(m.find("Osmosis") != std::string::npos && m.find("Water moves across a membrane.") != std::string::npos);
    CHECK(m.find("[unclear]") == std::string::npos);
    CHECK(m.find("### Nothing stored") != std::string::npos);
    CHECK_EQ(strip_unclear("[unclear] It needs no energy."), std::string("It needs no energy."));
    CHECK_EQ(strip_unclear("A [unclear] B"), std::string("A B"));
}

TEST(passage_scores) {
    CHECK_EQ(keyword_hits("kitchen pipe plumber", "The plumber said the Kitchen pipe leaks"), 3);
    CHECK_EQ(keyword_hits("kitchen pipe", "nothing here"), 0);
    CHECK(passage_scores(2, "kitchen pipe plumber"));
    CHECK(!passage_scores(1, "kitchen pipe plumber"));
    CHECK(passage_scores(1, "budget"));
    CHECK(!passage_scores(0, "budget"));
    CHECK(!passage_scores(0, ""));

    Relevant rel;
    rel.block = "From recordings:\n[S1] A 00:00: x\n";
    rel.sources = json::parse(R"([{"label":"S1","note_id":1,"start_sec":0,"title":"A"}])");
    std::vector<Passage> cands = {{2, 60, "Class", "Osmosis: water moves across a membrane."},
                                  {3, 0, "Doctor", "Take the membrane tablet twice."},
                                  {4, 0, "Other", "Nothing to do with it."}};
    int n = add_passages(rel, cands, "membrane water", {}, 6000);
    CHECK_EQ(n, 1);   // the medicine summary is never added; the unrelated one does not score
    CHECK_EQ(rel.sources.size(), (size_t)2);
    CHECK(rel.block.find("[S2] Class") != std::string::npos);
    CHECK(rel.sources[1]["note_id"] == 2 && rel.sources[1]["start_sec"] == 60 && rel.sources[1]["label"] == "S2");
}

TEST(answer_stream_skips_medicine_sentences) {
    std::string got;
    AnswerStream s({}, [&](const std::string &p) { got += p; return true; }, AnswerStream::SkipMedicine);
    const std::string raw = "- The pipe must be replaced.\n- Take 5 mg after dinner.\n- Ravi sends the quote.\n";
    for (size_t i = 0; i < raw.size(); i += 2) s.feed(raw.substr(i, 2));
    s.finish();
    CHECK_EQ(got, std::string("- The pipe must be replaced.\n- Ravi sends the quote."));
    CHECK_EQ(s.shown(), got);
    CHECK(!s.done());

    std::string g2;
    AnswerStream t({}, [&](const std::string &p) { g2 += p; return true; }, AnswerStream::SkipMedicine);
    t.feed("Take one tablet. ");
    t.feed("Then rest. ");
    t.finish();
    CHECK_EQ(g2, std::string("Then rest."));
}

TEST(answer_stream_done_when_held) {
    AnswerStream s({}, [&](const std::string &) { return true; });
    CHECK(!s.done());
    s.feed("Fine [S1]. Take 5 mg [S1]. Then");
    CHECK(s.held());
    CHECK(s.done());       // generation can stop: the answer is built in code
    CHECK(!s.cancelled());
}

#include <sqlite3.h>
#include <unistd.h>

TEST(store_note_summaries_in_a_range) {
    char path[] = "/tmp/sve_route_XXXXXX";
    int fd = mkstemp(path);
    if (fd >= 0) close(fd);
    {
        Store st(path);
        CHECK(st.ok());
        int64_t a = st.create("A", "en", nullptr, "a", 10), b = st.create("B", "en", nullptr, "b", 10),
                c = st.create("C", "en", nullptr, "c", 10);
        sqlite3 *db = nullptr;
        sqlite3_open(path, &db);
        auto at = [&](int64_t id, int64_t t) {
            std::string sql = "UPDATE notes SET created_at=" + std::to_string(t) + " WHERE id=" + std::to_string(id);
            sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
        };
        at(a, 2000); at(b, 1000); at(c, 3000);
        sqlite3_close(db);
        json rows = st.note_summaries(1000, 3000, 10, true);
        CHECK_EQ(rows.size(), (size_t)2);
        if (rows.size() == 2) { CHECK(rows[0]["title"] == "B"); CHECK(rows[1]["title"] == "A"); }
        CHECK(st.note_summaries(1, 1000, 10).empty());
        CHECK_EQ(st.note_summaries(1, 5000, 1).size(), (size_t)1);
    }
    unlink(path);
}

// ---- Fix round 1

TEST(route_subject_decides_period_and_summary) {
    Route a = route_question("What did Ravi say today about the pipe?", kNow);
    CHECK_EQ(a.kind, std::string("recordings"));
    CHECK_EQ(a.from, local(2026, 11, 11));
    CHECK_EQ(a.to, local(2026, 11, 12));
    CHECK(!a.by_rule);
    CHECK(kind_of("Who is the president today?") != "summary");
    Route b = route_question("Summarise the plumber call", kNow);
    CHECK_EQ(b.kind, std::string("recordings"));
    CHECK_EQ(b.from, (int64_t)0);
}

TEST(route_due_only_when_it_is_about_what_is_due) {
    CHECK(kind_of("What do I have to do for the exam?") != "due");
    CHECK(kind_of("Why was the meeting late due to rain?") != "due");
    CHECK(kind_of("What did Ravi remind me about?") != "due");
    CHECK(kind_of("రవి ఏమి చేయాల్సి ఉందని చెప్పాడు?") != "due");
}

TEST(route_quiz_only_when_asked_for) {
    CHECK(kind_of("పరీక్ష ఎప్పుడు?") != "quiz");
    CHECK(kind_of("రక్త పరీక్ష రిపోర్ట్ ఏమిటి?") != "quiz");
    CHECK(kind_of("What did the teacher say about the quiz on Friday?") != "quiz");
    CHECK_EQ(kind_of("నన్ను పరీక్ష చేయండి"), std::string("quiz"));
    CHECK_EQ(kind_of("పరీక్ష పెట్టండి"), std::string("quiz"));
    CHECK_EQ(kind_of("quiz on the budget"), std::string("quiz"));
}

TEST(passage_scoring_word_start_and_two_hits) {
    CHECK_EQ(keyword_hits("ai", "Ravi said it twice"), 0);                     // "What is AI?"
    CHECK_EQ(keyword_hits("ai", "The new AI tool"), 1);
    CHECK_EQ(keyword_hits("cook rice", "The price of the cook"), 1);           // "How do I cook rice?"
    CHECK(!passage_scores(keyword_hits("cook rice", "The price of the cook"), "cook rice"));
    CHECK(!passage_scores(keyword_hits("capital france", "Capital spending rose this year"), "capital france"));
    CHECK(passage_scores(1, "budget"));
    CHECK(!passage_scores(1, "budget plan"));
    CHECK(passage_scores(2, "budget plan"));
    CHECK(!passage_scores(2, "a1 b2 c3 d4 e5"));                               // still at least half
    CHECK_EQ(keyword_hits("పైపు", "వంటగది పైపులు మార్చాలి"), 1);                  // Telugu suffixes still match
}

TEST(health_question_gate) {
    for (const char *q : {"What is paracetamol used for?", "What should I take for a headache?", "How do I bring down a fever?",
                          "జ్వరం తగ్గడానికి ఏం చేయాలి?", "What did the doctor prescribe?", "రక్త పరీక్ష రిపోర్ట్ ఏమిటి?"})
        CHECK(chat_health_question(q, {}, {}));
    CHECK(!chat_health_question("Who wrote Gitanjali?", {}, {}));
    CHECK(!chat_health_question("What is the capital of France?", {}, {}));
    // a follow-up of a medicine answer, whatever it says
    std::vector<ChatTurn> after = chat_turns(json::parse(
        R"([{"role":"user","text":"Which tablet?"},{"role":"assistant","text":"Dolo 650","medicine":true}])"));
    CHECK(chat_health_question("Who wrote Gitanjali?", after, {}));
    std::vector<ChatTurn> plain = {{"user", "a"}, {"assistant", "b"}};
    CHECK(!chat_health_question("Who wrote Gitanjali?", plain, {}));
    CHECK(health_only_text("en") ==
          "I can only answer health and medicine questions from your saved medicines and recordings. Please ask a doctor or pharmacist.");
    CHECK(health_only_text("te") ==
          "ఆరోగ్యం, మందుల ప్రశ్నలకు మీరు సేవ్ చేసిన మందులు, రికార్డింగ్‌ల నుండి మాత్రమే సమాధానం ఇవ్వగలను. దయచేసి డాక్టర్‌ను లేదా ఫార్మసిస్ట్‌ను అడగండి.");
}

TEST(health_sentences_never_reach_or_leave_the_model) {
    // the check itself, on the verified leaks
    for (const char *t : {"Paracetamol relieves pain and fever.", "[Doctor visit] Crocin was prescribed at night."})
        CHECK(health_text(t, {}));
    // summary material: sentences and titles
    json notes = json::parse(R"([
        {"title":"Doctor visit","created_at":0,"short_summary":"Crocin was prescribed at night. The next visit is in March.","sections":[]},
        {"title":"Class","created_at":0,"short_summary":null,
         "sections":[{"summary":{"title":"Fever notes","points":[{"text":"Paracetamol relieves pain and fever."},{"text":"Water moves across a membrane."}]}}]}])");
    std::string m = summary_material(notes);
    for (const char *gone : {"Crocin", "Doctor visit", "Paracetamol", "Fever notes"}) CHECK(m.find(gone) == std::string::npos);
    CHECK(m.find("The next visit is in March.") != std::string::npos);
    CHECK(m.find("Water moves across a membrane.") != std::string::npos);
    // quiz
    json qn = json::parse(R"([{"id":1,"title":"T","revision_questions":"Q1. What did the doctor prescribe?\nA1. Crocin.\nQ2. [unclear] What is due?\nA2. The [unclear] report."}])");
    auto q = pick_quiz(qn, {}, {});
    CHECK(q && q->id == "1:2");
    if (q) { CHECK_EQ(q->question, std::string("What is due?")); CHECK_EQ(q->answer, std::string("The report.")); }
    CHECK_EQ(quiz_count(qn, {}), (size_t)1);
    // passages
    Relevant rel;
    rel.sources = json::array();
    CHECK_EQ(add_passages(rel, {{1, 0, "Class", "Paracetamol relieves pain in the class"}, {2, 0, "Class", "The class moved to March"}}, "class", {}, 6000), 1);
    CHECK(rel.block.find("Paracetamol") == std::string::npos);
    // streams
    std::string kept;
    AnswerStream skip({}, [&](const std::string &p) { kept += p; return true; }, AnswerStream::SkipMedicine);
    skip.feed("The pipe leaks. Paracetamol relieves pain and fever. Ravi calls. ");
    skip.finish();
    CHECK_EQ(kept, std::string("The pipe leaks. Ravi calls."));
    std::string shown;
    AnswerStream hold({}, [&](const std::string &p) { shown += p; return true; });
    hold.feed("Gitanjali is poetry. Paracetamol relieves pain and fever. More");
    CHECK(hold.held());
    CHECK(shown.find("Paracetamol") == std::string::npos);
}

TEST(summary_material_says_when_nothing_is_stored) {
    bool any = true;
    summary_material(json::parse(R"([{"title":"A","created_at":0,"short_summary":"","sections":[]}])"), {}, &any);
    CHECK(!any);
    summary_material(json::parse(R"([{"title":"A","created_at":0,"short_summary":"Real.","sections":[]}])"), {}, &any);
    CHECK(any);
    summary_material(json::parse(R"([{"title":"A","created_at":0,"short_summary":"Take one tablet.","sections":[]}])"), {}, &any);
    CHECK(!any);
}

TEST(parse_revision_questions_more_forms) {
    auto qa = parse_revision_questions("**Q1. First?**\n**A1. One.**\nQuestion 2: Second?\nAnswer 2: Two.\nప్ర3. మూడవది?\nజ3. మూడు.\nQuestions are fun\n");
    CHECK_EQ(qa.size(), (size_t)3);
    if (qa.size() != 3) return;
    CHECK_EQ(qa[0].first, std::string("First?"));
    CHECK_EQ(qa[0].second, std::string("One."));
    CHECK_EQ(qa[1].first, std::string("Second?"));
    CHECK_EQ(qa[1].second, std::string("Two."));
    CHECK_EQ(qa[2].first, std::string("మూడవది?"));
    CHECK_EQ(qa[2].second, std::string("మూడు. Questions are fun"));   // not a new item: no number
}

TEST(store_retrieval_within_a_period) {
    char path[] = "/tmp/sve_route2_XXXXXX";
    int fd = mkstemp(path);
    if (fd >= 0) close(fd);
    {
        Store st(path);
        int64_t a = st.create("Old", "en", nullptr, "x", 10), b = st.create("New", "en", nullptr, "x", 10);
        st.set_text_column(b, "short_summary", "New summary");
        st.set_pieces(a, json::parse(R"([{"start_sec":0,"text":"walnut plan"}])"));
        st.set_pieces(b, json::parse(R"([{"start_sec":0,"text":"walnut plan again"}])"));
        sqlite3 *db = nullptr;
        sqlite3_open(path, &db);
        sqlite3_exec(db, ("UPDATE notes SET created_at=1000 WHERE id=" + std::to_string(a)).c_str(), nullptr, nullptr, nullptr);
        sqlite3_exec(db, ("UPDATE notes SET created_at=2000 WHERE id=" + std::to_string(b)).c_str(), nullptr, nullptr, nullptr);
        sqlite3_close(db);
        CHECK_EQ(relevant(st, "walnut", 6000).sources.size(), (size_t)2);
        Relevant r = relevant(st, "walnut", 6000, 1500, 2500);
        CHECK_EQ(r.sources.size(), (size_t)1);
        if (!r.sources.empty()) CHECK(r.sources[0]["note_id"] == b);
        json rows = st.note_summaries(1500, 2500, 10);
        CHECK_EQ(rows.size(), (size_t)1);
        if (!rows.empty()) { CHECK(rows[0]["short_summary"] == "New summary"); CHECK(!rows[0].contains("transcript")); }
        CHECK_EQ(st.note_summaries(0, 0, 10).size(), (size_t)2);
        CHECK(st.note_summaries(0, 0, 10)[0]["title"] == "New");   // newest first when not asked otherwise
        CHECK(st.note_summaries(0, 5000, 10, true)[0]["title"] == "Old");
    }
    unlink(path);
}
