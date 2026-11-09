#include "sve_health.h"

#include "sve_medcheck.h"
#include "sve_suggest.h"
#include "sve_textmatch.h"

#include <algorithm>
#include <cstring>

namespace sve {

namespace {

using Words = std::vector<std::vector<std::string>>;

// Latin-script words and phrases, matched as whole token runs (lowercase, after med_tokens).
Words phrases(std::initializer_list<const char *> list) {
    Words out;
    for (const char *p : list) out.push_back(med_tokens(p, false));
    return out;
}

const Words &kLatinHealth() {
    static const Words w = phrases({
        // common medicine names
        "paracetamol", "acetaminophen", "ibuprofen", "aspirin", "dolo", "crocin", "calpol", "combiflam", "disprin",
        "metformin", "insulin", "glimepiride", "amlodipine", "telmisartan", "losartan", "atorvastatin",
        "rosuvastatin", "thyroxine", "levothyroxine", "thyronorm", "eltroxin", "pantoprazole", "pantop", "omeprazole",
        "rabeprazole", "ranitidine", "amoxicillin", "azithromycin", "ciprofloxacin", "cetirizine", "montelukast",
        "diclofenac", "aceclofenac", "ecosprin", "clopidogrel", "warfarin", "antibiotic", "antibiotics",
        "painkiller", "painkillers", "paracetmol", "paracitamol", "benadryl", "meftal", "saridon", "digene", "avil",
        "allegra", "augmentin", "tramadol", "naproxen", "alprazolam", "sildenafil", "pan 40", "antacid", "antacids",
        "vitamin", "vitamins", "ointment", "ointments",
        // dose and timing
        "tablet", "tablets", "pill", "pills", "capsule", "capsules", "medicine", "medicines",
        "medication", "medications", "dose", "doses", "dosage", "overdose", "mg", "mcg", "syrup", "injection",
        "injections", "prescription", "prescribed", "how many times", "how often", "before food", "after food",
        "before meals", "after meals", "empty stomach",
        // side effects
        "side effect", "side effects", "sideeffect", "sideeffects", "allergy", "allergic",
        // symptoms
        "fever", "pain", "pains", "painful", "headache", "ache", "aches", "cough", "vomit", "vomiting", "nausea",
        "dizzy", "dizziness", "giddiness", "diarrhea", "diarrhoea", "loose motions", "rash", "itching", "infection",
        "swelling", "bleeding", "breathless", "breathlessness", "sore throat", "chest pain", "symptom", "symptoms",
        "nosebleed", "acidity", "constipation",
        // conditions and advice ("cold" and "treat" are ambiguous: health_gate only)
        "treatment", "treatments", "cure", "cures", "curable", "remedy", "remedies", "malaria", "dengue", "pregnancy", "pregnant",
        "cholesterol", "thyroid", "asthma", "diabetes", "health",
        // romanised Telugu
        "mandu", "mandulu", "mathra", "mathralu", "matra", "matralu", "goli", "golilu", "dosu",
        "jwaram", "jvaram", "noppi", "tala noppi", "talanoppi", "daggu", "vanthi", "vanthulu", "vaanthi",
        "jalubu", "enni sarlu", "ennisarlu",
    });
    return w;
}

// Telugu-script stems, matched as substrings (Telugu adds endings to the word itself).
const std::vector<std::string> kTeluguHealth = {
    "మందు", "మాత్ర", "టాబ్లెట్", "ట్యాబ్లెట్", "టేబ్లెట్", "క్యాప్సూల్", "డోస్", "డోసు", "సిరప్", "ఇంజెక్షన్",
    "ఎన్ని సార్లు", "ఎన్నిసార్లు", "భోజనానికి ముందు", "భోజనం తర్వాత", "పరగడుపున", "ఖాళీ కడుపు",
    "సైడ్ ఎఫెక్ట్", "దుష్ప్రభావం", "దుష్ప్రభావాలు", "అలెర్జీ", "అలర్జీ",
    "నొప్పి", "జ్వర", "దగ్గు", "వాంతి", "వాంతులు", "విరేచనాలు", "జలుబు", "కళ్ళు తిరుగు", "ఇన్ఫెక్షన్",
    "వాపు", "దురద", "ఆయాసం", "లక్షణం", "లక్షణాలు",
    "రక్తపోటు", "మధుమేహం", "చికిత్స", "వైద్యం", "గర్భం", "విరేచనం", "ఉబ్బరం",
    "పారాసిటమాల్", "పారాసెటమాల్", "పారసిటమాల్", "పేరాసిటమాల్", "డోలో", "క్రోసిన్", "మెట్ఫార్మిన్", "ఇన్సులిన్", "యాంటీబయాటిక్",
};

// Words a health-related memory may have beyond health_keywords: doctor, hospital, test results, dose.
const Words &kLatinCare() {
    static const Words w = phrases({
        "doctor", "doctors", "dr", "physician", "hospital", "hospitals", "clinic", "nurse", "symptom", "symptoms",
        "report", "reports", "test result", "test results", "blood test", "bp", "blood pressure", "sugar",
        "diabetes", "dose", "doses", "scan", "scans", "xray", "x ray", "mri", "ecg",
        "aaspatri", "aasupatri", "daktar",
    });
    return w;
}

const std::vector<std::string> kTeluguCare = {
    "డాక్టర్", "డాక్టరు", "వైద్యుడు", "వైద్యురాలు", "వైద్యులు", "ఆసుపత్రి", "ఆస్పత్రి", "హాస్పిటల్", "క్లినిక్",
    "లక్షణం", "లక్షణాలు", "రిపోర్ట్", "రిపోర్టు", "పరీక్ష", "బీపీ", "షుగర్", "డోస్", "స్కాన్",
};

bool has_any(const std::vector<std::string> &tokens, const std::string &clean, const Words &latin,
             const std::vector<std::string> &telugu) {
    for (const auto &p : latin)
        if (!p.empty() && contains_run(tokens, p)) return true;
    for (const auto &w : telugu)
        if (clean.find(nfc_clean(w)) != std::string::npos) return true;
    return false;
}

}  // namespace

namespace {

// A space where Latin letters or digits meet Telugu: "feverకి" -> "fever కి", "tabletలు" -> "tablet లు".
std::string split_scripts(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i > 0) {
            const unsigned char a = (unsigned char)s[i - 1], b = (unsigned char)s[i];
            const bool a_ascii = (a >= '0' && a <= '9') || (a >= 'a' && a <= 'z') || (a >= 'A' && a <= 'Z');
            const bool b_ascii = (b >= '0' && b <= '9') || (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z');
            if ((a_ascii && b >= 0xE0) || (a >= 0x80 && b_ascii)) out += ' ';
        }
        out += s[i];
    }
    return out;
}

// Romanised stems (jvaraniki, noppiki, dagguki) and the *ache words (toothache, backache), not cache or moustache.
bool stem_word(const std::vector<std::string> &tokens) {
    static const char *const kPrefix[] = {"jvaram", "jvaran", "jwaram", "jwaran", "noppi", "daggu"};
    static const char *const kNotAche[] = {"cache", "moustache", "mustache", "panache", "apache", "attache", "geocache"};
    for (const auto &t : tokens) {
        for (const char *p : kPrefix)
            if (t.compare(0, strlen(p), p) == 0) return true;
        if (t.size() > 4 && t.compare(t.size() - 4, 4, "ache") == 0) {
            bool no = false;
            for (const char *x : kNotAche) no = no || t == x;
            if (!no) return true;
        }
    }
    return false;
}

std::string lower_ascii(std::string s) {
    for (auto &c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return s;
}

void erase_all(std::string &s, const char *w) {
    for (size_t at = s.find(w); at != std::string::npos; at = s.find(w, at)) s.replace(at, strlen(w), " ");
}

bool has_sub(const std::string &s, std::initializer_list<const char *> list) {
    for (const char *w : list) if (s.find(w) != std::string::npos) return true;
    return false;
}

// Always count in the gate.
const Words &kLatinGateStrong() {
    static const Words w = phrases({"doctor", "doctors", "physician", "hospital", "hospitals", "clinic", "symptom", "symptoms",
                                    "diabetes", "blood pressure", "high bp", "low bp", "xray", "x ray", "mri", "ecg",
                                    "aaspatri", "aasupatri", "daktar"});
    return w;
}
const std::vector<std::string> kTeluguGateStrong = {"డాక్టర్", "డాక్టరు", "వైద్యుడు", "వైద్యురాలు", "వైద్యులు", "ఆసుపత్రి", "ఆస్పత్రి", "హాస్పిటల్", "క్లినిక్",
                                                    "బీపీ", "షుగర్"};
// Count in the gate only with a companion word.
const Words &kLatinGateCompanion() {
    static const Words w = phrases({"report", "reports", "scan", "scans", "sugar", "dr", "bp", "nurse", "test result", "test results", "blood test"});
    return w;
}
const std::vector<std::string> kTeluguGateCompanion = {"పరీక్ష", "రిపోర్ట్", "రిపోర్టు", "స్కాన్"};

}  // namespace

HealthFlag health_keywords(const std::string &q_in, const std::vector<std::string> &stored_medicine_names) {
    const std::string q = split_scripts(q_in);
    if (mentions_medicine(q, stored_medicine_names)) return HealthFlag::health;
    for (const auto &n : stored_medicine_names)
        if (!trim_copy(n).empty() && name_in(n, q)) return HealthFlag::health;
    const std::string clean = nfc_clean(without_only_words(q));
    const std::vector<std::string> whole = med_tokens(clean, false);
    return has_any(whole, clean, kLatinHealth(), kTeluguHealth) || has_any(med_tokens(clean, true), clean, kLatinHealth(), {}) ||
                   stem_word(whole)
               ? HealthFlag::health
               : HealthFlag::none;
}

bool health_gate(const std::string &text, const std::vector<std::string> &med_names) {
    std::string s = lower_ascii(split_scripts(text));
    const std::vector<std::string> all = med_tokens(nfc_clean(s), false);
    const auto word = [&](std::initializer_list<const char *> list) {
        for (const char *w : list) if (std::find(all.begin(), all.end(), w) != all.end()) return true;
        return false;
    };
    erase_all(s, "pain point");
    erase_all(s, "rash decision");
    if (!word({"day", "days", "daily", "take", "takes", "taking"}) && !has_sub(s, {"రోజు", "వేసుకో", "తీసుకో"}))
        for (const char *w : {"how many times", "how often", "ఎన్నిసార్లు", "ఎన్ని సార్లు"}) erase_all(s, w);
    if (health_keywords(s, med_names) == HealthFlag::health) return true;
    const std::string clean = nfc_clean(without_only_words(s));
    const std::vector<std::string> tokens = med_tokens(clean, false);
    if (has_any(tokens, clean, kLatinGateStrong(), kTeluguGateStrong)) return true;
    const bool companion = word({"blood", "medical", "health", "doctor", "doctors", "hospital", "ct", "mri"}) ||
                           has_sub(s, {"sugar level", "రక్త"});
    if (companion && has_any(tokens, clean, kLatinGateCompanion(), kTeluguGateCompanion)) return true;
    if (has_sub(s, {"what should i take", "what can i take", "what to take"})) return true;
    // Ambiguous words with an advice shape: not "cold war" or "treat of a show".
    erase_all(s, "cold war");
    erase_all(s, "cold call");
    erase_all(s, "cold weather");
    erase_all(s, "cold storage");
    const std::vector<std::string> left = med_tokens(nfc_clean(s), false);
    bool ambiguous = false;
    for (const char *w : {"cold", "colds", "treat", "treats", "treating"}) ambiguous = ambiguous || std::find(left.begin(), left.end(), w) != left.end();
    return ambiguous && has_sub(s, {"how do i", "how can i", "how to", "what should i", "what can i", "what to", "should i", "for a ", "for my ",
                                    "home remedy", "ఏం చేయాలి", "ఏమి చేయాలి"});
}

bool is_health_memory(const std::string &text, const std::vector<std::string> &med_names) {
    if (health_keywords(text, med_names) == HealthFlag::health) return true;
    const std::string clean = nfc_clean(text);
    return has_any(med_tokens(clean, false), clean, kLatinCare(), kTeluguCare);
}

}  // namespace sve
