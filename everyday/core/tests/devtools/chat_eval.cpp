// Runs the app's Ask (sve_assistant_chat) and to-do pass (sve_extract) on a short conversation, with a follow-up.
#include "sve_core.h"
#include "third_party/nlohmann/json.hpp"
#include <cstdio>
#include <unistd.h>
#include <fstream>
#include <regex>
#include <string>
using json = nlohmann::json;
static void quiet(int, const char *, void *) {}
static json take(char *p) { json j = p ? json::parse(p, nullptr, false) : json(); if (p) sve_free_string(p); return j; }
int main(int argc, char **argv) {
    sve_set_log(quiet, nullptr);
    std::remove("/tmp/sve_chatrun.db");
    sve_ctx *c = sve_init(argv[3]);
    if (!c || sve_load_llm(c, argv[1], 8192, 8, 99) != 0) return 1;
    sve_profile_set(c, argc > 4 ? argv[4] : R"({"name":"Sumanth","lang":"en"})");
    std::ifstream in(argv[2]);
    json pieces = json::array();
    std::string line, transcript;
    std::regex re(R"(^\[t=(\d+)\]\s*(.*)$)");
    while (std::getline(in, line)) {
        std::smatch m;
        if (!std::regex_match(line, m, re)) continue;
        pieces.push_back({{"start_sec", std::stoi(m[1])}, {"text", m[2].str()}});
        transcript += m[2].str() + "\n";
    }
    int64_t id = sve_note_create(c, "Product discussion", "en", nullptr, transcript.c_str(), 60);
    sve_note_set_pieces(c, id, pieces.dump().c_str());
    json msgs = json::array({{{"role", "user"}, {"text", "What do I need to do?"}}});
    json a = take(sve_assistant_chat(c, msgs.dump().c_str(), "en", nullptr, nullptr, nullptr, nullptr));
    printf("Q1 What do I need to do?\nA1 [%s] %s\n", a.value("kind", "").c_str(), a.value("answer", "").c_str());
    msgs.push_back({{"role", "assistant"}, {"text", a.value("answer", "")}});
    msgs.push_back({{"role", "user"}, {"text", "Draft a message asking him for an update."}});
    json b = take(sve_assistant_chat(c, msgs.dump().c_str(), "en", nullptr, nullptr, nullptr, nullptr));
    printf("Q2 Draft a message asking him for an update.\nA2 [%s] %s\n", b.value("kind", "").c_str(), b.value("answer", "").c_str());
    sve_extract(c, id, nullptr, nullptr);
    for (const auto &s : take(sve_suggestions_json(c, "pending")))
        if (s.value("kind", "") == "task") printf("TASK %s | who=%s | due=%s\n", s["payload"].value("text", "").c_str(),
                                                  s["payload"]["who"].is_string() ? s["payload"]["who"].get<std::string>().c_str() : "-",
                                                  s["payload"]["due_date"].is_string() ? s["payload"]["due_date"].get<std::string>().c_str() : "-");
    fflush(stdout);
    _exit(0);
}
