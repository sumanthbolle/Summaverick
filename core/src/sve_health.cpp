#include "sve_health.h"

#include "sve_medcheck.h"
#include "sve_suggest.h"
#include "sve_textmatch.h"

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
        "painkiller", "painkillers",
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
    "నొప్పి", "జ్వరం", "జ్వరము", "దగ్గు", "వాంతి", "వాంతులు", "విరేచనాలు", "జలుబు", "కళ్ళు తిరుగు", "ఇన్ఫెక్షన్",
    "వాపు", "దురద", "ఆయాసం", "లక్షణం", "లక్షణాలు",
    "పారాసిటమాల్", "పారాసెటమాల్", "డోలో", "క్రోసిన్", "మెట్ఫార్మిన్", "ఇన్సులిన్", "యాంటీబయాటిక్",
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

HealthFlag health_keywords(const std::string &q, const std::vector<std::string> &stored_medicine_names) {
    if (mentions_medicine(q, stored_medicine_names)) return HealthFlag::health;
    for (const auto &n : stored_medicine_names)
        if (!trim_copy(n).empty() && name_in(n, q)) return HealthFlag::health;
    const std::string clean = nfc_clean(q);
    return has_any(med_tokens(clean, false), clean, kLatinHealth(), kTeluguHealth) ? HealthFlag::health
                                                                                   : HealthFlag::none;
}

bool is_health_memory(const std::string &text, const std::vector<std::string> &med_names) {
    if (health_keywords(text, med_names) == HealthFlag::health) return true;
    const std::string clean = nfc_clean(text);
    return has_any(med_tokens(clean, false), clean, kLatinCare(), kTeluguCare);
}

}  // namespace sve
