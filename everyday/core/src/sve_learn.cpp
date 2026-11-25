#include "sve_learn.h"

namespace sve {

namespace {

std::string canon(const json &v) {
    if (v.is_null()) return "";
    if (v.is_string()) return v.get<std::string>();
    if (v.is_array()) {
        bool all_str = true;
        for (const auto &e : v) if (!e.is_string()) { all_str = false; break; }
        if (all_str) {
            std::string out;
            for (size_t i = 0; i < v.size(); ++i) {
                if (i) out += ", ";
                out += v[i].get<std::string>();
            }
            return out;
        }
    }
    return v.dump();
}

std::string field_of(const json &o, const char *f) {
    if (!o.is_object()) return "";
    auto it = o.find(f);
    return it == o.end() ? "" : canon(*it);
}

json as_obj(const json &v) {
    if (v.is_string()) {
        json p = json::parse(v.get<std::string>(), nullptr, false);
        return p.is_discarded() ? json::object() : p;
    }
    return v;
}

bool preference_exists(Store &store, const std::string &text) {
    for (const char *status : {"pending", "accepted"}) {
        for (const auto &s : store.suggestions(status)) {
            if (s.value("kind", "") != "fact") continue;
            json pl = as_obj(s.value("payload", json::object()));
            if (pl.is_object() && pl.value("text", "") == text) return true;
        }
    }
    for (const auto &m : store.memory("preference"))
        if (m.value("text", "") == text) return true;
    return false;
}

} // namespace

void record_edit(Store &store, const std::string &kind, const json &before, const json &after) {
    static const char *kFields[] = {"text", "times", "when_text", "due_date", "repeat"};
    for (const char *field : kFields) {
        std::string from = field_of(before, field), to = field_of(after, field);
        if (from == to) continue;
        store.log_decision(kind, field, from, to);

        int same = 0;
        for (const auto &d : store.decisions(kind, field, 1000000))
            if (d.value("from_value", "") == from && d.value("to_value", "") == to) ++same;
        if (same < 2) continue;

        std::string shown_from = from;
        if (std::string(field) == "times") {
            std::string w = field_of(before, "when_text");
            if (!w.empty()) shown_from = w;
        }
        std::string text = "When I say \"" + shown_from + "\" I mean " + to;
        if (preference_exists(store, text)) continue;
        json s = {{"kind", "fact"},
                  {"payload", {{"memory_kind", "preference"}, {"text", text}}},
                  {"quote", text},
                  {"check_status", "ok"}};
        store.add_suggestion(s);
    }
}

} // namespace sve
