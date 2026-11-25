#include "check.h"
#include "sve_suggest.h"

using namespace sve;
using nlohmann::json;

static const json kPieces = json::parse(R"([
  {"start_sec":0,"text":"Doctor: Your BP is a little high, Lakshmi garu."},
  {"start_sec":28,"text":"Take Amlodipine 5 mg once a day after dinner. Start from today."},
  {"start_sec":56,"text":"Bring your blood test report on Saturday."},
  {"start_sec":84,"text":"Salt less than one teaspoon a day. No pickles for two"},
  {"start_sec":112,"text":"weeks, please."}
])");
static const json kRoutine = json::parse(R"({"breakfast":"08:00","lunch":"13:00","dinner":"20:00","bed":"22:00"})");

static Extracted ex(const char *s) { return parse_extraction(s); }

static bool has(const json &arr, const char *v) {
    for (const auto &e : arr) if (e.is_string() && e.get<std::string>() == v) return true;
    return false;
}

TEST(getters_tolerate_wrong_types) {
    json o = json::parse(R"({"s":5,"i":"x","a":{},"o":[1],"ok":"y","n":7,"d":7.0,"arr":[1]})");
    CHECK_EQ(jstr(o, "s"), std::string(""));
    CHECK_EQ(jstr(o, "ok"), std::string("y"));
    CHECK_EQ(jint(o, "i", -1), (int64_t)-1);
    CHECK_EQ(jint(o, "n", -1), (int64_t)7);
    CHECK_EQ(jint(o, "d", -1), (int64_t)7);
    CHECK(jarr(o, "a").is_array() && jarr(o, "a").empty());
    CHECK_EQ(jarr(o, "arr").size(), (size_t)1);
    CHECK(jobj(o, "o").is_null());
    CHECK(jobj(o, "a").is_object());
    CHECK_EQ(jstr(json("str"), "s"), std::string(""));
    CHECK_EQ(jint(json(nullptr), "s", 3), (int64_t)3);
}

TEST(time_date_repeat_validation) {
    CHECK(valid_hhmm("20:00"));
    CHECK(valid_hhmm("07:05"));
    CHECK(!valid_hhmm("24:00"));
    CHECK(!valid_hhmm("8:00"));
    CHECK(!valid_hhmm("08:60"));
    CHECK(valid_ymd("2026-11-07"));
    CHECK(!valid_ymd("2026-13-01"));
    CHECK(!valid_ymd("null"));
    CHECK_EQ(sanitize_repeat("daily"), std::string("daily"));
    CHECK_EQ(sanitize_repeat("once|daily"), std::string(""));
    auto t = valid_times(json::parse(R"(["20:00","x",5,"20:00","08:00"])"));
    CHECK_EQ(t.size(), (size_t)2);
}

TEST(resolve_times_from_routine) {
    auto t = resolve_times("after dinner", kRoutine);
    CHECK_EQ(t.size(), (size_t)1);
    CHECK_EQ(t[0], std::string("20:00"));
    auto t2 = resolve_times("After breakfast and dinner", kRoutine);
    CHECK_EQ(t2.size(), (size_t)2);
    CHECK_EQ(t2[0], std::string("08:00"));
    CHECK(resolve_times("రాత్రి భోజనం తర్వాత", kRoutine).size() == 1);
    CHECK(resolve_times("when needed", kRoutine).empty());
    CHECK(resolve_times("after dinner", json()).empty());
}

TEST(resolve_times_tiffin_and_sleep) {
    json r = json::parse(R"({"breakfast":"08:00","dinner":"20:00","bed":"22:00"})");
    CHECK_EQ(resolve_times("after tiffin", r), std::vector<std::string>({"08:00"}));
    CHECK_EQ(resolve_times("నిద్ర ముందు", r), std::vector<std::string>({"22:00"}));
}

TEST(windows_respect_budget) {
    json p = json::array();
    for (int i = 0; i < 5; ++i) p.push_back({{"start_sec", i * 28}, {"text", "abcdefghij"}});
    auto count = [](const std::string &s) { return (int)s.size(); };
    // each line "[t=N] abcdefghij\n" is 17-18 bytes; budget 40 fits two
    auto w = group_windows(p, 40, count);
    CHECK_EQ(w.size(), (size_t)3);
    CHECK_EQ(w[0].size(), (size_t)2);
    CHECK_EQ(w[2].size(), (size_t)1);
    // oversized piece gets its own window
    json big = json::array({{{"start_sec", 0}, {"text", "x"}}, {{"start_sec", 1}, {"text", std::string(100, 'y')}}, {{"start_sec", 2}, {"text", "z"}}});
    auto w2 = group_windows(big, 40, count);
    CHECK_EQ(w2.size(), (size_t)3);
    CHECK(group_windows(json::array(), 40, count).empty());
    CHECK(group_windows(json("bad"), 40, count).empty());
}

TEST(merge_dedupes_and_caps) {
    std::vector<Extracted> parts = {
        ex(R"({"reminders":[{"text":"a"}],"topics":["Health","BP"],"people":["Ravi","Sita"],"vocab":["Amlodipine"]})"),
        ex(R"({"reminders":[{"text":"b"}],"tasks":[{"text":"t"}],"topics":["health","Diet","Sleep"],"people":["ravi","A","B","C","D"],"vocab":["amlodipine","ECG"]})"),
        Extracted()};
    Extracted m = merge_extracted(parts);
    CHECK(m.ok);
    CHECK_EQ(m.reminders.size(), (size_t)2);
    CHECK_EQ(m.tasks.size(), (size_t)1);
    CHECK_EQ(m.topics.size(), (size_t)3);
    CHECK(has(m.topics, "Health") && has(m.topics, "BP") && has(m.topics, "Diet"));
    CHECK_EQ(m.people.size(), (size_t)5);
    CHECK_EQ(m.vocab.size(), (size_t)2);
    CHECK(!merge_extracted({Extracted()}).ok);
}

TEST(quote_in_cited_piece) {
    QuoteHit h = locate_quote(kPieces, "Take Amlodipine 5 mg once a day after dinner.", 28);
    CHECK(h.found);
    CHECK_EQ(h.start_sec, (int64_t)28);
    // normalization: case, punctuation, number words
    CHECK(locate_quote(kPieces, "amlodipine five mg", 28).found);
}

TEST(quote_in_other_piece_corrects_start) {
    QuoteHit h = locate_quote(kPieces, "Bring your blood test report on Saturday", 0);
    CHECK(h.found);
    CHECK_EQ(h.start_sec, (int64_t)56);
}

TEST(quote_across_adjacent_pieces) {
    QuoteHit h = locate_quote(kPieces, "No pickles for two weeks", 84);
    CHECK(h.found);
    CHECK_EQ(h.start_sec, (int64_t)84);
}

TEST(quote_missing_or_empty_not_found) {
    CHECK(!locate_quote(kPieces, "Take Amlodipine 10 mg", 28).found);
    CHECK(!locate_quote(kPieces, "", 28).found);
    CHECK(!locate_quote(kPieces, "  ..  ", 28).found);
    CHECK(!locate_quote(json("bad"), "Take", 28).found);
}

TEST(dose_without_unit) {
    CHECK(dose_missing_unit("5"));
    CHECK(dose_missing_unit("1/2"));
    CHECK(!dose_missing_unit("5 mg"));
    CHECK(!dose_missing_unit("5mg"));
    CHECK(!dose_missing_unit(""));
    CHECK(!dose_missing_unit("half tablet"));
}

static json find_kind(const std::vector<json> &v, const char *kind) {
    for (const auto &s : v) if (s["kind"] == kind) return s;
    return nullptr;
}

TEST(verified_medicine_reminder_is_ok) {
    Extracted e = ex(R"({"reminders":[{"text":"Take Amlodipine 5 mg after dinner","when_text":"after dinner","times":[],
        "repeat":"daily","until_date":null,"quote":"Take Amlodipine 5 mg once a day after dinner.","start_sec":28,
        "medicine":{"name":"Amlodipine","dose":"5 mg","frequency":"once a day","duration":""}}]})");
    auto v = build_suggestions(e, kPieces, 7, kRoutine);
    CHECK_EQ(v.size(), (size_t)1);
    json s = find_kind(v, "reminder");
    CHECK(!s.is_null());
    CHECK_EQ(s["check_status"].get<std::string>(), std::string("ok"));
    CHECK(s["check_fields"].empty());
    CHECK_EQ(s["note_id"].get<int64_t>(), (int64_t)7);
    CHECK_EQ(s["start_sec"].get<int64_t>(), (int64_t)28);
    CHECK_EQ(s["payload"]["times"], json::array({"20:00"}));
    CHECK_EQ(s["payload"]["repeat"].get<std::string>(), std::string("daily"));
    CHECK_EQ(s["payload"]["medicine"]["name"].get<std::string>(), std::string("Amlodipine"));
    CHECK(s["payload"]["until_date"].is_null());
}

TEST(unverifiable_medicine_is_dropped) {
    Extracted e = ex(R"({"reminders":[{"text":"Take Metformin 500 mg","quote":"Take Amlodipine 5 mg once a day after dinner.",
        "start_sec":28,"medicine":{"name":"Metformin","dose":"500 mg"}}]})");
    CHECK(build_suggestions(e, kPieces, 7, kRoutine).empty());
}

TEST(wrong_dose_is_check) {
    Extracted e = ex(R"({"reminders":[{"text":"Take Amlodipine 10 mg","when_text":"after dinner","quote":"Take Amlodipine 5 mg once a day after dinner.",
        "start_sec":28,"medicine":{"name":"Amlodipine","dose":"10 mg"}}]})");
    auto v = build_suggestions(e, kPieces, 7, kRoutine);
    CHECK_EQ(v.size(), (size_t)1);
    CHECK_EQ(v[0]["check_status"].get<std::string>(), std::string("check"));
    CHECK(has(v[0]["check_fields"], "dose"));
}

TEST(unitless_dose_is_check) {
    Extracted e = ex(R"({"reminders":[{"text":"Take Amlodipine","when_text":"after dinner","quote":"Take Amlodipine 5 mg once a day after dinner.",
        "start_sec":28,"medicine":{"name":"Amlodipine","dose":"5"}}]})");
    auto v = build_suggestions(e, kPieces, 7, kRoutine);
    CHECK_EQ(v.size(), (size_t)1);
    CHECK_EQ(v[0]["check_status"].get<std::string>(), std::string("check"));
    CHECK(has(v[0]["check_fields"], "dose"));
}

TEST(invented_quote_is_check) {
    Extracted e = ex(R"({"tasks":[{"text":"Bring X-ray","quote":"Bring your X-ray on Monday","start_sec":56}],
        "facts":[{"text":"BP high","quote":"Your BP is a little high","start_sec":56}]})");
    auto v = build_suggestions(e, kPieces, 7, kRoutine);
    CHECK_EQ(v.size(), (size_t)2);
    json t = find_kind(v, "task");
    CHECK_EQ(t["check_status"].get<std::string>(), std::string("check"));
    CHECK(has(t["check_fields"], "quote"));
    json f = find_kind(v, "fact");
    CHECK_EQ(f["check_status"].get<std::string>(), std::string("ok"));
    CHECK_EQ(f["start_sec"].get<int64_t>(), (int64_t)0);  // corrected from 56
    CHECK_EQ(f["payload"]["memory_kind"].get<std::string>(), std::string("fact"));
}

TEST(model_times_on_medicine_without_routine_word_are_check) {
    Extracted e = ex(R"({"reminders":[{"text":"Take Amlodipine 5 mg","when_text":"","times":["21:00"],
        "quote":"Take Amlodipine 5 mg once a day after dinner.","start_sec":28,"medicine":{"name":"Amlodipine","dose":"5 mg"}}]})");
    auto v = build_suggestions(e, kPieces, 7, kRoutine);
    CHECK_EQ(v.size(), (size_t)1);
    CHECK_EQ(v[0]["check_status"].get<std::string>(), std::string("check"));
    CHECK(has(v[0]["check_fields"], "times"));
    CHECK_EQ(v[0]["payload"]["times"], json::array({"21:00"}));
}

TEST(routine_overrides_model_times) {
    Extracted e = ex(R"({"reminders":[{"text":"Take Amlodipine 5 mg","when_text":"after dinner","times":["21:30"],
        "quote":"Take Amlodipine 5 mg once a day after dinner.","start_sec":28,"medicine":{"name":"Amlodipine","dose":"5 mg"}}]})");
    auto v = build_suggestions(e, kPieces, 7, kRoutine);
    CHECK_EQ(v[0]["payload"]["times"], json::array({"20:00"}));
    CHECK_EQ(v[0]["check_status"].get<std::string>(), std::string("ok"));
}

TEST(wrong_field_types_do_not_throw) {
    Extracted e = ex(R"({"reminders":[{"text":5,"when_text":["x"],"times":"20:00","repeat":3,"quote":{"a":1},"start_sec":"28",
        "medicine":{"name":["Amlodipine"],"dose":5}}],
        "tasks":[{"text":"Bring report","due_date":20261107,"who":null,"quote":"Bring your blood test report on Saturday","start_sec":"x"}],
        "profile":[{"change":7,"detail":"x"}],"facts":[{"text":null}]})");
    std::vector<json> v;
    bool threw = false;
    try { v = build_suggestions(e, kPieces, 7, kRoutine); } catch (...) { threw = true; }
    CHECK(!threw);
    json t = find_kind(v, "task");
    CHECK(!t.is_null());
    CHECK(t["payload"]["due_date"].is_null());
    CHECK_EQ(t["start_sec"].get<int64_t>(), (int64_t)56);
    CHECK(find_kind(v, "profile").is_null());  // unknown change
    CHECK(find_kind(v, "fact").is_null());     // empty text
}

TEST(profile_medicine_is_checked) {
    Extracted e = ex(R"({"profile":[
        {"change":"add_medicine","detail":"Amlodipine 10 mg daily","quote":"Take Amlodipine 5 mg once a day after dinner.","start_sec":28,
         "medicine":{"name":"Amlodipine","dose":"5 mg","when_text":"after dinner","frequency":"once a day","duration":""}},
        {"change":"add_medicine","detail":"x","quote":"Take Amlodipine 5 mg once a day after dinner.","start_sec":28,
         "medicine":{"name":"Telma","dose":"40 mg"}},
        {"change":"add_medicine","detail":"no medicine object","quote":"Take Amlodipine 5 mg","start_sec":28,"medicine":null},
        {"change":"routine","detail":"Dinner at 21:00","quote":"Salt less than one teaspoon a day","start_sec":84,"medicine":null,
         "routine":{"dinner":"21:00","lunch":"1 pm","snack":"16:00"}},
        {"change":"routine","detail":"Walk daily","quote":"Salt less than one teaspoon a day","start_sec":84,"medicine":null}]})");
    auto v = build_suggestions(e, kPieces, 7, kRoutine);
    CHECK_EQ(v.size(), (size_t)2);
    CHECK_EQ(v[0]["kind"].get<std::string>(), std::string("profile"));
    CHECK_EQ(v[0]["check_status"].get<std::string>(), std::string("ok"));
    CHECK_EQ(v[0]["payload"]["change"].get<std::string>(), std::string("add_medicine"));
    // text is built in code from the checked fields, never the model's detail
    CHECK_EQ(v[0]["payload"]["text"].get<std::string>(), std::string("Add medicine: Amlodipine 5 mg"));
    CHECK_EQ(v[0]["payload"]["medicine"]["when_text"].get<std::string>(), std::string("after dinner"));
    CHECK_EQ(v[1]["payload"]["change"].get<std::string>(), std::string("routine"));
    CHECK_EQ(v[1]["payload"]["routine"], json::parse(R"({"dinner":"21:00"})"));
    CHECK_EQ(v[1]["check_status"].get<std::string>(), std::string("check"));  // 21:00 is not in the quote
    CHECK(has(v[1]["check_fields"], "routine"));
}

TEST(profile_medicine_without_time_is_check) {
    Extracted e = ex(R"({"profile":[{"change":"add_medicine","detail":"","quote":"Take Amlodipine 5 mg once a day after dinner.","start_sec":28,
         "medicine":{"name":"Amlodipine","dose":"5 mg","when_text":"","frequency":"once a day"}}]})");
    auto v = build_suggestions(e, kPieces, 7, kRoutine);
    CHECK_EQ(v.size(), (size_t)1);
    CHECK(has(v[0]["check_fields"], "times"));
}

TEST(vocab_suggestions) {
    Extracted e = ex(R"({"vocab":["Amlodipine","  ",""]})");
    auto v = build_suggestions(e, kPieces, 7, kRoutine);
    CHECK_EQ(v.size(), (size_t)1);
    CHECK_EQ(v[0]["kind"].get<std::string>(), std::string("vocab"));
    CHECK_EQ(v[0]["payload"]["text"].get<std::string>(), std::string("Amlodipine"));
}

TEST(keyword_query_drops_stopwords) {
    CHECK_EQ(keyword_query("What did the doctor say about salt?"), std::string("doctor salt"));
    CHECK_EQ(keyword_query("When do I take Amlodipine?"), std::string("amlodipine"));
    CHECK(keyword_query("ఉప్పు గురించి డాక్టర్ ఏమన్నారు").find("ఉప్పు") != std::string::npos);
}

TEST(medicine_question_detection) {
    std::vector<std::string> names = {"Amlodipine", "Telma AM"};
    CHECK(is_medicine_question("When do I take Amlodipine?", names));
    CHECK(is_medicine_question("when do i take telma am", names));
    CHECK(is_medicine_question("Which tablets do I take?", {}));
    CHECK(is_medicine_question("What is my dose?", {}));
    CHECK(is_medicine_question("నా మందులు ఏమిటి?", {}));
    CHECK(is_medicine_question("ఏ మాత్ర వేసుకోవాలి", {}));
    CHECK(!is_medicine_question("What did the doctor say about salt?", names));
    CHECK(!is_medicine_question("Where is my pillow?", {}));
    auto n = names_in_question("Is Amlodipine before food?", names);
    CHECK_EQ(n.size(), (size_t)1);
}

TEST(citations_map_and_strip) {
    json src = json::parse(R"([{"label":"S1","note_id":3,"start_sec":84,"title":"Doctor"},{"label":"S2","note_id":3,"start_sec":0,"title":"Doctor"}])");
    Cited c = apply_citations("Less than one teaspoon of salt a day [S1].", src);
    CHECK(!c.not_found);
    CHECK_EQ(c.answer, std::string("Less than one teaspoon of salt a day."));
    CHECK_EQ(c.sources.size(), (size_t)1);
    CHECK_EQ(c.sources[0]["start_sec"].get<int64_t>(), (int64_t)84);
    CHECK(!c.sources[0].contains("label"));
    Cited c2 = apply_citations("A [S2, S1] and B [S1][S2]", src);
    CHECK_EQ(c2.sources.size(), (size_t)2);
    CHECK_EQ(c2.sources[0]["start_sec"].get<int64_t>(), (int64_t)0);
    CHECK(c2.answer.find('[') == std::string::npos);
    CHECK(apply_citations("NOT_FOUND", src).not_found);
    CHECK(apply_citations("Something [S9].", src).not_found);
    CHECK(apply_citations("No citation here.", src).not_found);
    CHECK(apply_citations("x [S1]", json("bad")).not_found);
}

TEST(not_found_text_exact) {
    CHECK_EQ(not_found_text("en"), std::string("I didn't hear that in your recordings."));
    CHECK_EQ(not_found_text("te"), std::string("మీ రికార్డింగ్‌లలో ఇది నేను వినలేదు."));
    CHECK_EQ(not_found_text("hi"), std::string("I didn't hear that in your recordings."));
}

TEST(medicine_answer_quotes_verbatim) {
    json meds = json::parse(R"([{"name":"Amlodipine","dose":"5 mg","when_text":"after dinner","times":["20:00"],"repeat":"daily","until_date":null}])");
    json hits = json::parse(R"([{"note_id":3,"title":"Doctor visit","start_sec":28,"text":"Take Amlodipine 5 mg once a day after dinner. Start from today."}])");
    std::string en = medicine_answer(meds, hits, "en");
    CHECK(en.find("Amlodipine 5 mg") != std::string::npos);
    CHECK(en.find("20:00") != std::string::npos);
    CHECK(en.find("\xE2\x80\x9CTake Amlodipine 5 mg once a day after dinner. Start from today.\xE2\x80\x9D") != std::string::npos);
    CHECK(en.find("Doctor visit @0:28") != std::string::npos);
    std::string te = medicine_answer(meds, hits, "te");
    CHECK(te.find("మీ ప్రొఫైల్") != std::string::npos);
    CHECK(te.find("Take Amlodipine 5 mg once a day after dinner.") != std::string::npos);
    CHECK(medicine_answer(json::array(), json::array(), "en").empty());
    CHECK(medicine_answer(json("x"), json(5), "en").empty());
}

TEST(topic_summary_sanitized) {
    json j = json::parse(R"({"summary":"S","key_terms":[{"term":"ETF","meaning":"fund"},{"term":5},"x"],"quiz":[{"q":"Q","a":"A"},{"q":"only"}]})");
    json s = sanitize_topic_summary(j);
    CHECK_EQ(s["summary"].get<std::string>(), std::string("S"));
    CHECK_EQ(s["key_terms"].size(), (size_t)1);
    CHECK_EQ(s["quiz"].size(), (size_t)1);
    json e = sanitize_topic_summary(json("bad"));
    CHECK(e["summary"].is_string() && e["key_terms"].is_array() && e["quiz"].is_array());
}

TEST(mentions_medicine_detects_units_and_names) {
    CHECK(mentions_medicine("Take 5 mg tonight", {}));
    CHECK(mentions_medicine("Remember your amlodipine", {"Amlodipine"}));
    CHECK(!mentions_medicine("Revise the ETF quiz before Friday", {"Amlodipine"}));
}

TEST(weekday_names_checked_against_dates) {
    CHECK_EQ(weekday_name("2026-11-06"), std::string("Friday"));
    CHECK_EQ(weekday_name("2026-11-07"), std::string("Saturday"));
    CHECK_EQ(weekday_name("bad"), std::string(""));
    CHECK(date_matches_weekday("2026-11-07", "Bring the report on Saturday"));
    CHECK(!date_matches_weekday("2026-11-09", "Bring the report on Saturday"));
    CHECK(date_matches_weekday("2026-11-07", "శనివారం report తీసుకురండి"));
    CHECK(!date_matches_weekday("2026-11-08", "శనివారం report తీసుకురండి"));
    CHECK(date_matches_weekday("2026-11-09", "come back in two weeks"));      // nothing to compare
    CHECK(date_matches_weekday("2026-11-09", "Saturday or Monday"));          // ambiguous: not judged
}

TEST(task_date_on_wrong_weekday_is_check) {
    Extracted e = ex(R"({"tasks":[{"text":"Bring blood test report","due_date":"2026-11-14","quote":"Bring your blood test report on Saturday","start_sec":56},
        {"text":"Bring blood test report","due_date":"2026-11-10","quote":"Bring your blood test report on Saturday","start_sec":56}],
        "reminders":[{"text":"Bring report","repeat":"once","times":["09:00"],"start_date":"2026-11-08","quote":"Bring your blood test report on Saturday","start_sec":56}]})");
    auto v = build_suggestions(e, kPieces, 7, kRoutine);
    CHECK_EQ(v.size(), (size_t)3);
    json r = find_kind(v, "reminder");
    CHECK_EQ(r["payload"]["start_date"].get<std::string>(), std::string("2026-11-08"));
    CHECK(has(r["check_fields"], "start_date"));
    int ok = 0, check = 0;
    for (const auto &s : v) if (s["kind"] == "task") (s["check_status"] == "ok" ? ok : check)++;
    CHECK_EQ(ok, 1);     // 2026-11-14 is a Saturday
    CHECK_EQ(check, 1);  // 2026-11-10 is a Tuesday
}

TEST(medicine_dates_need_confirmation) {
    const char *src = R"({"reminders":[{"text":"Take Amlodipine 5 mg","when_text":"after dinner","repeat":"daily","start_date":"%s",
        "quote":"Take Amlodipine 5 mg once a day after dinner.","start_sec":28,"medicine":{"name":"Amlodipine","dose":"5 mg"}}]})";
    char buf[512];
    snprintf(buf, sizeof buf, src, "2026-11-07");
    auto v = build_suggestions(ex(buf), kPieces, 7, kRoutine, "2026-11-06");
    CHECK_EQ(v.size(), (size_t)1);
    CHECK(has(v[0]["check_fields"], "start_date"));  // not said: "Please check"
    snprintf(buf, sizeof buf, src, "2026-11-06");
    v = build_suggestions(ex(buf), kPieces, 7, kRoutine, "2026-11-06");
    CHECK(has(v[0]["check_fields"], "start_date"));  // the quote says nothing about a day
    const char *src2 = R"({"reminders":[{"text":"x","when_text":"after dinner","repeat":"daily","start_date":"2026-11-06",
        "quote":"Take Amlodipine 5 mg once a day after dinner. Start from today.","start_sec":28,"medicine":{"name":"Amlodipine","dose":"5 mg"}}]})";
    v = build_suggestions(ex(src2), kPieces, 7, kRoutine, "2026-11-06");
    CHECK_EQ(v[0]["check_status"].get<std::string>(), std::string("ok"));  // "today" said
}

TEST(vocab_terms_are_short) {
    auto v = build_suggestions(ex(R"({"vocab":["Salt less than one teaspoon a day","blood test","Telma AM 40"]})"), kPieces, 7, kRoutine);
    CHECK_EQ(v.size(), (size_t)2);
}

TEST(profile_without_quote_is_dropped) {
    auto v = build_suggestions(ex(R"({"profile":[{"change":"routine","detail":"dinner 20:00","quote":"","start_sec":0}]})"), kPieces, 7, kRoutine);
    CHECK(v.empty());
}

TEST(medicine_reminder_text_built_in_code) {
    Extracted e = ex(R"({"reminders":[{"text":"Take Amlodipine 10 mg","when_text":"after dinner","quote":"Take Amlodipine 5 mg once a day after dinner.",
        "start_sec":28,"medicine":{"name":"Amlodipine","dose":"5 mg"}}]})");
    auto v = build_suggestions(e, kPieces, 7, kRoutine);
    CHECK_EQ(v.size(), (size_t)1);
    CHECK_EQ(v[0]["check_status"].get<std::string>(), std::string("ok"));
    CHECK_EQ(v[0]["payload"]["text"].get<std::string>(), std::string("Take Amlodipine 5 mg"));
    auto te = build_suggestions(e, kPieces, 7, kRoutine, "", "te");
    CHECK(te[0]["payload"]["text"].get<std::string>().find("Amlodipine 5 mg") == 0);
}

TEST(medicine_text_without_medicine_object_is_check) {
    Extracted e = ex(R"({"reminders":[{"text":"Take 2 BP tablets after dinner","when_text":"after dinner","quote":"Take Amlodipine 5 mg once a day after dinner.","start_sec":28,"medicine":null}],
        "tasks":[{"text":"Buy Amlodipine 5 mg","quote":"Take Amlodipine 5 mg once a day after dinner.","start_sec":28}],
        "facts":[{"text":"Doctor gave a new medicine","quote":"Take Amlodipine 5 mg once a day after dinner.","start_sec":28},
                 {"text":"BP is a little high","quote":"Your BP is a little high","start_sec":0}]})");
    auto v = build_suggestions(e, kPieces, 7, kRoutine);
    CHECK_EQ(v.size(), (size_t)4);
    for (size_t i = 0; i < 3; ++i) {
        CHECK_EQ(v[i]["check_status"].get<std::string>(), std::string("check"));
        CHECK(has(v[i]["check_fields"], "text"));
    }
    CHECK_EQ(v[3]["check_status"].get<std::string>(), std::string("ok"));
}

TEST(medicine_reminder_without_time_is_check) {
    Extracted e = ex(R"({"reminders":[{"text":"","when_text":"","times":[],"quote":"Take Amlodipine 5 mg once a day after dinner.",
        "start_sec":28,"medicine":{"name":"Amlodipine","dose":"5 mg"}}]})");
    auto v = build_suggestions(e, kPieces, 7, kRoutine);
    CHECK_EQ(v.size(), (size_t)1);
    CHECK(has(v[0]["check_fields"], "times"));
}

TEST(weekday_resolves_to_nearest_upcoming) {
    const char *t = R"({"tasks":[{"text":"Bring report","due_date":"%s","quote":"%s","start_sec":56}]})";
    auto run = [&](const char *date, const char *quote) {
        char buf[512];
        snprintf(buf, sizeof buf, t, date, quote);
        auto v = build_suggestions(ex(buf), kPieces, 7, kRoutine, "2026-11-06");
        return v.empty() ? json() : v[0];
    };
    // today is Friday 2026-11-06
    CHECK_EQ(run("2026-11-07", "Bring your blood test report on Saturday")["check_status"].get<std::string>(), std::string("ok"));
    // A day a week late is put right from the words (2026-11-22: "by Monday" came out a week late), not only flagged.
    json late = run("2026-11-14", "Bring your blood test report on Saturday");
    CHECK_EQ(late["payload"]["due_date"].get<std::string>(), std::string("2026-11-07"));
    CHECK(!has(late["check_fields"], "due_date"));
    CHECK_EQ(run("2026-11-14", "Bring the report next Saturday")["check_status"].get<std::string>(), std::string("check"));  // quote not in pieces
    CHECK(!has(run("2026-11-14", "Bring the report next Saturday")["check_fields"], "due_date"));
    CHECK(!has(run("2026-11-14", "వచ్చే శనివారం report తీసుకురండి")["check_fields"], "due_date"));
    CHECK(has(run("2026-11-14", "శని report తీసుకురండి")["check_fields"], "due_date"));     // short Telugu form
    CHECK_EQ(run("2026-11-06", "Bring it Fri")["payload"]["due_date"].get<std::string>(), std::string("2026-11-13"));  // Friday without "today"
    CHECK(!has(run("2026-11-13", "Bring it Fri")["check_fields"], "due_date"));
    CHECK(!has(run("2026-11-06", "Bring it today, Friday")["check_fields"], "due_date"));
}

TEST(hits_must_name_the_medicine) {
    json hits = json::parse(R"([{"text":"Vitamin B12 1000 mcg once a week"},{"text":"Take Vitamin D3 60000 IU weekly"}])");
    json f = hits_naming(hits, {"Vitamin D3"});
    CHECK_EQ(f.size(), (size_t)1);
    CHECK(f[0]["text"].get<std::string>().find("D3") != std::string::npos);
    CHECK(hits_naming(hits, {}).empty());
}

TEST(medicine_list_is_not_an_answer) {
    json meds = json::parse(R"([{"name":"Amlodipine","dose":"5 mg","when_text":"after dinner","times":["20:00"],"repeat":"daily"}])");
    std::string en = medicine_list_answer(meds, "en");
    CHECK(en.find("Your saved medicines:") == 0);
    CHECK(en.find("Amlodipine 5 mg") != std::string::npos);
    CHECK(en.find("Your recordings don't name a medicine for this question.") != std::string::npos);
    CHECK(en.find("From your profile") == std::string::npos);
    std::string te = medicine_list_answer(meds, "te");
    CHECK(te.find("Amlodipine") != std::string::npos && te.find("Your saved") == std::string::npos);
    CHECK(medicine_list_answer(json::array(), "en").empty());
}

TEST(topic_summary_medicine_lines_filtered) {
    json s = sanitize_topic_summary(json::parse(R"({"summary":"BP was 150 over 95. The doctor prescribed Amlodipine 10 mg after dinner. Eat less salt.",
        "key_terms":[{"term":"BP","meaning":"blood pressure"},{"term":"Amlodipine","meaning":"a BP medicine"},{"term":"Dose","meaning":"5 mg"}],
        "quiz":[{"q":"How much salt?","a":"Under one teaspoon"},{"q":"Which tablet?","a":"Amlodipine"}]})"));
    bool dropped = filter_topic_summary(s, {"Amlodipine"});
    CHECK(dropped);
    CHECK_EQ(s["summary"].get<std::string>(), std::string("BP was 150 over 95. Eat less salt."));
    CHECK_EQ(s["key_terms"].size(), (size_t)1);
    CHECK_EQ(s["quiz"].size(), (size_t)1);
    json clean = sanitize_topic_summary(json::parse(R"({"summary":"ETFs track an index.","key_terms":[],"quiz":[]})"));
    CHECK(!filter_topic_summary(clean, {"Amlodipine"}));
    CHECK_EQ(clean["summary"].get<std::string>(), std::string("ETFs track an index."));
}

TEST(mentions_medicine_words) {
    CHECK(mentions_medicine("Take 2 BP tablets after dinner", {}));
    CHECK(mentions_medicine("new medicine from the doctor", {}));
    CHECK(mentions_medicine("రాత్రి మందు వేసుకోండి", {}));
    CHECK(!mentions_medicine("Bring the blood test report", {}));
}

TEST(untimed_one_off_reminder_becomes_task) {
    Extracted e = ex(R"({"reminders":[{"text":"Bring blood test report","when_text":"Saturday","times":[],"repeat":"once",
        "start_date":"2026-11-14","quote":"Bring your blood test report on Saturday","start_sec":56,"medicine":null}]})");
    auto v = build_suggestions(e, kPieces, 7, kRoutine, "2026-11-06");
    CHECK_EQ(v.size(), (size_t)1);
    CHECK_EQ(v[0]["kind"].get<std::string>(), std::string("task"));
    CHECK_EQ(v[0]["payload"]["due_date"].get<std::string>(), std::string("2026-11-14"));
    CHECK(has(v[0]["check_fields"], "due_date"));  // Saturday from Friday 11-06 is 11-07
    // a timed one-off stays a reminder
    Extracted t = ex(R"({"reminders":[{"text":"Call Ravi","times":["17:00"],"repeat":"once","quote":"Your BP is a little high","start_sec":0}]})");
    CHECK_EQ(build_suggestions(t, kPieces, 7, kRoutine, "2026-11-06")[0]["kind"].get<std::string>(), std::string("reminder"));
}

TEST(saved_medicine_name_in_text_is_check) {
    Extracted e = ex(R"({"reminders":[{"text":"Take Amlodipine twice","times":["09:00"],"repeat":"daily","quote":"Take Amlodipine 5 mg once a day after dinner.","start_sec":28,"medicine":null}],
        "tasks":[{"text":"Amlodipine 10 at night","quote":"Take Amlodipine 5 mg once a day after dinner.","start_sec":28}]})");
    auto v = build_suggestions(e, kPieces, 7, kRoutine, "", "en", {"Amlodipine"});
    CHECK_EQ(v.size(), (size_t)2);
    for (const auto &s : v) {
        CHECK_EQ(s["check_status"].get<std::string>(), std::string("check"));
        CHECK(has(s["check_fields"], "text"));
    }
    auto w = build_suggestions(e, kPieces, 7, kRoutine);  // without the saved name "twice" passes the word check
    CHECK(!has(w[0]["check_fields"], "text"));
}

TEST(routine_time_must_be_in_quote) {
    auto one = [](const char *quote, const char *time) {
        char buf[512];
        snprintf(buf, sizeof buf, R"({"profile":[{"change":"routine","detail":"Dinner","quote":"%s","start_sec":0,"routine":{"dinner":"%s"}}]})", quote, time);
        auto v = build_suggestions(ex(buf), kPieces, 7, kRoutine);
        return v.empty() ? json() : v[0];
    };
    CHECK(!has(one("Have dinner at 9 pm", "21:00")["check_fields"], "routine"));
    CHECK(!has(one("dinner at 21:00", "21:00")["check_fields"], "routine"));
    CHECK(!has(one("dinner by 9:30", "21:30")["check_fields"], "routine"));
    CHECK(!has(one("రాత్రి తొమ్మిది గంటలకు భోజనం", "21:00")["check_fields"], "routine"));
    CHECK(has(one("Have dinner at 8 pm", "21:00")["check_fields"], "routine"));
    CHECK(has(one("Have dinner later", "21:00")["check_fields"], "routine"));
}

TEST(converted_task_due_only_from_start_date) {
    Extracted e = ex(R"({"reminders":[{"text":"Bring report","times":[],"repeat":"once","until_date":"2026-11-07",
        "quote":"Bring your blood test report on Saturday","start_sec":56,"medicine":null}]})");
    auto v = build_suggestions(e, kPieces, 7, kRoutine, "2026-11-06");
    CHECK_EQ(v[0]["kind"].get<std::string>(), std::string("task"));
    CHECK(v[0]["payload"]["due_date"].is_null());
}

TEST(dates_need_support_in_quote) {
    Extracted t = ex(R"({"tasks":[{"text":"Less salt","due_date":"2026-11-20","quote":"less than one teaspoon of salt a day","start_sec":84}]})");
    auto v = build_suggestions(t, kPieces, 7, kRoutine, "2026-11-06");
    CHECK(has(v[0]["check_fields"], "due_date"));
    Extracted r = ex(R"({"reminders":[{"text":"No pickles","times":["09:00"],"repeat":"daily","until_date":"2026-11-20",
        "quote":"No pickles for two weeks","start_sec":84,"medicine":null}]})");
    auto w = build_suggestions(r, kPieces, 7, kRoutine, "2026-11-06");
    CHECK_EQ(w[0]["check_status"].get<std::string>(), std::string("ok"));
    CHECK(quote_supports_date("come back tomorrow"));
    CHECK(quote_supports_date("for 5 days"));
    CHECK(quote_supports_date("on 20/11"));
    CHECK(quote_supports_date("on the 15th of November"));
    CHECK(quote_supports_date("రేపు రండి"));
    CHECK(quote_supports_date("రెండు వారాలు"));
    CHECK(!quote_supports_date("take it after dinner"));
}

TEST(dose_differing_from_saved_medicine_is_check) {
    json saved = json::parse(R"([{"id":1,"name":"Amlodipine","dose":"2.5 mg"}])");
    Extracted e = ex(R"({"reminders":[{"text":"","when_text":"after dinner","quote":"Take Amlodipine 5 mg once a day after dinner.",
        "start_sec":28,"medicine":{"name":"Amlodipine","dose":"5 mg"}}]})");
    auto v = build_suggestions(e, kPieces, 7, kRoutine, "", "en", {"Amlodipine"}, saved);
    CHECK(has(v[0]["check_fields"], "dose"));
    json same = json::parse(R"([{"id":1,"name":"amlodipine","dose":"5mg"}])");
    CHECK_EQ(build_suggestions(e, kPieces, 7, kRoutine, "", "en", {"Amlodipine"}, same)[0]["check_status"].get<std::string>(), std::string("ok"));
}

TEST(routine_hour_respects_am_pm) {
    CHECK(quote_has_hour("dinner at 9 pm", "21:00"));
    CHECK(quote_has_hour("dinner at 9pm", "21:00"));
    CHECK(quote_has_hour("dinner at 21:00", "21:00"));
    CHECK(!quote_has_hour("dinner at 9 am", "21:00"));
    CHECK(!quote_has_hour("walk at 9 a.m.", "21:00"));
    CHECK(quote_has_hour("walk at 9 am", "09:00"));
    CHECK(!quote_has_hour("walk at 9 pm", "09:00"));
    CHECK(quote_has_hour("lunch at 12 pm", "12:00"));
    CHECK(quote_has_hour("dinner at 9", "21:00"));  // no am/pm: hour only
}

TEST(am_pm_after_minutes_is_read) {
    CHECK(!quote_has_hour("dinner at 9:30 am", "21:30"));
    CHECK(quote_has_hour("dinner at 9:30 pm", "21:30"));
    CHECK(quote_has_hour("dinner at 21:30", "21:30"));
    CHECK(quote_has_hour("dinner at 9:30", "21:30"));  // no am/pm: hour only
    CHECK(quote_has_hour("walk at 9:30 am", "09:30"));
    CHECK(!quote_has_hour("walk at 9:30 p.m.", "09:30"));
    CHECK(!quote_has_hour("dinner at 9:05", "17:00"));  // the minutes are never read as an hour
    // the routine check in build_suggestions sees the same thing
    Extracted e = ex(R"({"profile":[{"change":"routine","detail":"Dinner","quote":"dinner at 9:30 am","start_sec":0,"routine":{"dinner":"21:30"}}]})");
    CHECK(has(build_suggestions(e, kPieces, 7, kRoutine)[0]["check_fields"], "routine"));
}

TEST(profile_card_dose_differing_from_saved_is_check) {
    json saved = json::parse(R"([{"id":1,"name":"Amlodipine","dose":"2.5 mg"}])");
    Extracted e = ex(R"({"profile":[
        {"change":"add_medicine","detail":"","quote":"Take Amlodipine 5 mg once a day after dinner.","start_sec":28,
         "medicine":{"name":"Amlodipine","dose":"5 mg","when_text":"after dinner"}},
        {"change":"update_medicine","detail":"","quote":"Take Amlodipine 5 mg once a day after dinner.","start_sec":28,
         "medicine":{"name":"amlodipine","dose":"5 mg","when_text":"after dinner"}},
        {"change":"stop_medicine","detail":"","quote":"Take Amlodipine 5 mg once a day after dinner.","start_sec":28,
         "medicine":{"name":"Amlodipine","dose":"5 mg"}}]})");
    auto v = build_suggestions(e, kPieces, 7, kRoutine, "", "en", {"Amlodipine"}, saved);
    CHECK_EQ(v.size(), (size_t)3);
    CHECK_EQ(v[0]["check_status"].get<std::string>(), std::string("check"));
    CHECK(has(v[0]["check_fields"], "dose"));
    CHECK(has(v[1]["check_fields"], "dose"));
    CHECK(!has(v[2]["check_fields"], "dose"));  // stopping does not set a dose
    json same = json::parse(R"([{"id":1,"name":"amlodipine","dose":"5mg"}])");
    auto w = build_suggestions(e, kPieces, 7, kRoutine, "", "en", {"Amlodipine"}, same);
    CHECK_EQ(w[0]["check_status"].get<std::string>(), std::string("ok"));
    CHECK(!has(w[1]["check_fields"], "dose"));
}

TEST(twice_daily_with_one_time_is_check) {
    CHECK(twice_daily_mismatch("twice_daily", json::array({"20:00"})));
    CHECK(twice_daily_mismatch("twice_daily", json::array()));
    CHECK(twice_daily_mismatch("twice_daily", json::array({"08:00", "14:00", "20:00"})));
    CHECK(!twice_daily_mismatch("twice_daily", json::array({"08:00", "20:00"})));
    CHECK(!twice_daily_mismatch("daily", json::array({"20:00"})));
    const json pieces = json::parse(R"([{"start_sec":0,"text":"Take Amlodipine 5 mg twice daily after dinner."}])");
    Extracted e = ex(R"({"reminders":[{"text":"Take Amlodipine 5 mg","when_text":"after dinner","times":[],
        "repeat":"twice_daily","quote":"Take Amlodipine 5 mg twice daily after dinner.","start_sec":0,
        "medicine":{"name":"Amlodipine","dose":"5 mg","frequency":"twice daily","duration":""}}]})");
    auto v = build_suggestions(e, pieces, 7, kRoutine);
    CHECK_EQ(v.size(), (size_t)1);
    CHECK_EQ(v[0]["payload"]["times"], json::array({"20:00"}));
    CHECK_EQ(v[0]["check_status"].get<std::string>(), std::string("check"));
    CHECK(has(v[0]["check_fields"], "times"));
}

TEST(twice_daily_with_two_times_is_ok) {
    const json pieces = json::parse(R"([{"start_sec":0,"text":"Take Metformin 500 mg twice daily after breakfast and after dinner."}])");
    Extracted e = ex(R"({"reminders":[{"text":"Take Metformin 500 mg","when_text":"after breakfast and after dinner","times":[],
        "repeat":"twice_daily","quote":"Take Metformin 500 mg twice daily after breakfast and after dinner.","start_sec":0,
        "medicine":{"name":"Metformin","dose":"500 mg","frequency":"twice daily","duration":""}}]})");
    auto v = build_suggestions(e, pieces, 7, kRoutine);
    CHECK_EQ(v.size(), (size_t)1);
    CHECK_EQ(v[0]["payload"]["times"], json::array({"08:00", "20:00"}));
    CHECK(!has(v[0]["check_fields"], "times"));
}

// Backfilled old notes: pieces of at most 1,500 characters (Unicode scalars, see the iOS backfill).
// Even counted as one token per byte (3 per Telugu letter, far above Gemma's real rate) such a piece
// is about the largest extraction window (4,500 tokens). sve_extract windows by whole pieces: each
// lands whole in a window, none is cut, none is lost.
TEST(backfill_pieces_fit_extraction_windows) {
    json p = json::array();
    std::string te;
    for (int i = 0; i < 1490; ++i) te += "\xE0\xB0\x95";  // 1,490 Telugu letters
    for (int i = 0; i < 6; ++i) p.push_back({{"start_sec", i * 60}, {"text", te}});
    auto count = [](const std::string &s) { return (int)s.size(); };  // bytes >= tokens
    auto w = group_windows(p, 4500, count);
    CHECK_EQ(w.size(), (size_t)6);
    size_t n = 0;
    for (const auto &win : w) {
        int used = 0;
        for (const auto &x : win) used += count("[t=" + std::to_string(x["start_sec"].get<int>()) + "] " + x["text"].get<std::string>() + "\n");
        CHECK(used <= 4500);
        n += win.size();
    }
    CHECK_EQ(n, (size_t)6);
}

// Meeting action items (2026-11-22): the model writes the words, the core the day.
TEST(task_due_day_from_due_text) {
    auto v = build_suggestions(ex(R"({"tasks":[{"text":"order test phones","who":"Kiran","due_text":"by Monday","due_date":"2026-11-30","quote":"","start_sec":56}]})"),
                               kPieces, 7, kRoutine, "2026-11-22");
    CHECK(!v.empty());
    CHECK_EQ(v[0]["payload"]["due_date"].get<std::string>(), std::string("2026-11-23"));
    CHECK_EQ(v[0]["payload"]["who"].get<std::string>(), std::string("Kiran"));
    auto w = build_suggestions(ex(R"({"tasks":[{"text":"fix login bug","who":"Priya","due_text":"Wednesday కల్లా","due_date":null,"quote":"","start_sec":56}]})"),
                               kPieces, 7, kRoutine, "2026-11-22");
    CHECK_EQ(w[0]["payload"]["due_date"].get<std::string>(), std::string("2026-11-25"));
}

// The model left the quote out of meeting action items: the task's own words find their line.
TEST(task_without_quote_finds_its_line) {
    const json meeting = json::parse(R"([
      {"start_sec":0,"text":"Ravi: సరే అందరూ join అయ్యారా?"},
      {"start_sec":52,"text":"Ravi: Good. నేను Friday లోపు API document అందరికీ పంపిస్తాను."}
    ])");
    auto v = build_suggestions(ex(R"({"tasks":[{"text":"API document అందరికీ పంపిస్తాను","who":"Ravi","due_text":"Friday లోపు","due_date":"2026-11-27"}]})"),
                               meeting, 7, kRoutine, "2026-11-22");
    CHECK(!v.empty());
    CHECK_EQ(v[0]["check_status"].get<std::string>(), std::string("ok"));
    CHECK_EQ(v[0]["start_sec"].get<int64_t>(), (int64_t)52);
    CHECK(v[0]["quote"].get<std::string>().find("API document") != std::string::npos);
    // Words that are not in the transcript stay to be checked.
    auto w = build_suggestions(ex(R"({"tasks":[{"text":"order the phones","who":"Kiran","due_text":"","due_date":null}]})"),
                               meeting, 7, kRoutine, "2026-11-22");
    CHECK_EQ(w[0]["check_status"].get<std::string>(), std::string("check"));
}

// "Next meeting Friday 4 PM": a one-time reminder of the meeting, from this note, on the day and at the time said.
TEST(next_meeting_becomes_a_reminder_of_this_note) {
    const json meeting = json::parse(R"([
      {"start_sec":0,"text":"Ravi: సరే అందరూ join అయ్యారా?"},
      {"start_sec":214,"text":"Ravi: Okay. Next meeting Friday 4 PM, same time."}
    ])");
    auto v = build_suggestions(ex(R"({"next_meeting":{"text":"Release meeting","when_text":"Friday 4 PM","quote":"Next meeting Friday 4 PM, same time.","start_sec":214}})"),
                               meeting, 7, kRoutine, "2026-11-22");
    CHECK_EQ((int)v.size(), 1);
    CHECK_EQ(v[0]["kind"].get<std::string>(), std::string("reminder"));
    CHECK_EQ(v[0]["payload"]["text"].get<std::string>(), std::string("Release meeting"));
    CHECK_EQ(v[0]["payload"]["start_date"].get<std::string>(), std::string("2026-11-27"));
    CHECK_EQ(v[0]["payload"]["times"][0].get<std::string>(), std::string("16:00"));
    CHECK_EQ(v[0]["payload"]["repeat"].get<std::string>(), std::string("once"));
    CHECK_EQ(v[0]["note_id"].get<int64_t>(), (int64_t)7);
    CHECK_EQ(v[0]["check_status"].get<std::string>(), std::string("ok"));
    // No day said: nothing to remind of.
    CHECK(build_suggestions(ex(R"({"next_meeting":{"text":"Sync","when_text":"soon","quote":"","start_sec":0}})"),
                            meeting, 7, kRoutine, "2026-11-22").empty());
    CHECK(build_suggestions(ex(R"({"next_meeting":null})"), meeting, 7, kRoutine, "2026-11-22").empty());
    // No quote: the when words find the line.
    auto q = build_suggestions(ex(R"({"next_meeting":{"text":"Next meeting","when_text":"Friday 4 PM","start_sec":214}})"),
                               meeting, 7, kRoutine, "2026-11-22");
    CHECK_EQ(q[0]["check_status"].get<std::string>(), std::string("ok"));
    CHECK_EQ(q[0]["start_sec"].get<int64_t>(), (int64_t)214);
}

TEST(clock_time_from_words) {
    CHECK_EQ(clock_time("Friday 4 PM"), std::string("16:00"));
    CHECK_EQ(clock_time("at 4:30 pm"), std::string("16:30"));
    CHECK_EQ(clock_time("10 a.m. Monday"), std::string("10:00"));
    CHECK_EQ(clock_time("12 pm"), std::string("12:00"));
    CHECK_EQ(clock_time("12 am"), std::string("00:00"));
    CHECK_EQ(clock_time("Friday 16:00"), std::string("16:00"));
    CHECK_EQ(clock_time("శుక్రవారం సాయంత్రం 4 గంటలకు"), std::string("16:00"));
    CHECK_EQ(clock_time("ఉదయం 10 గంటలకు"), std::string("10:00"));
    CHECK_EQ(clock_time("మధ్యాహ్నం 2 గంటలకు"), std::string("14:00"));
    CHECK_EQ(clock_time("Friday at 4"), std::string(""));      // am or pm is not said
    CHECK_EQ(clock_time("13 pm"), std::string(""));
    CHECK_EQ(clock_time("in 5 mg"), std::string(""));
}
