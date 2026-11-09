#pragma once
// Health detection for chat questions and memories, in English and Telugu (including mixed and
// romanised spellings). Errs on the side of flagging. Pure C++; no llama, mtmd or SQLite.
#include <string>
#include <vector>

namespace sve {

enum class HealthFlag { none, health };

// Flags a stored medicine name, a common medicine name, or a dose, timing, side-effect or symptom
// word ("tablet", "మాత్ర", "డోస్", "ఎన్ని సార్లు", "side effect", "నొప్పి", "జ్వరం", ...).
HealthFlag health_keywords(const std::string &q, const std::vector<std::string> &stored_medicine_names);

// The question gate and note-title check of Ask: never general knowledge when true. Narrower than is_health_memory
// on words with everyday meanings: report, scan, sugar, dr, bp, nurse, test result, పరీక్ష, రిపోర్ట్, స్కాన్ count only with
// a companion (blood, రక్త, medical, health, doctor, hospital, ct, mri, sugar level); "how often", "how many times",
// ఎన్నిసార్లు only with day, daily or take; "pain point" and "rash decision" do not count. "cold" and "treat" count only
// with an advice shape (how do I, what should I, for a…, ఏం చేయాలి).
bool health_gate(const std::string &text, const std::vector<std::string> &med_names);

// health_keywords, or a doctor, hospital, symptom, test result or dose word
// (doctor/డాక్టర్/వైద్యుడు, hospital/ఆసుపత్రి, report/test result/పరీక్ష, BP, sugar/షుగర్, scan/స్కాన్, ...).
bool is_health_memory(const std::string &text, const std::vector<std::string> &med_names);

}  // namespace sve
