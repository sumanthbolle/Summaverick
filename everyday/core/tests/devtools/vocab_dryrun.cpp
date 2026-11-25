#define private public
#include "sve_store.h"
#undef private
#include "sve_context.h"
#include <iostream>
#include <sstream>
int main(int, char **argv) {
    sve::Store st(argv[1]);
    auto terms = sve::saved_vocab(st);
    std::vector<std::string> meds;
    for (const auto &m : st.medicines()) meds.push_back(m.value("name", ""));
    int changed = 0, lines = 0;
    for (const auto &n : st.query_rows("SELECT id, transcript FROM notes WHERE transcript IS NOT NULL", {})) {
        std::istringstream in(n.value("transcript", ""));
        std::string line;
        while (std::getline(in, line)) {
            lines++;
            std::string out = sve::vocab_correct(line, terms, meds);
            if (out != line) {
                changed++;
                std::istringstream a(line), b(out); std::vector<std::string> wa, wb; std::string x;
                while (a >> x) wa.push_back(x); while (b >> x) wb.push_back(x);
                size_t i = 0; while (i < wa.size() && i < wb.size() && wa[i] == wb[i]) i++;
                size_t ja = wa.size(), jb = wb.size(); while (ja > i && jb > i && wa[ja-1] == wb[jb-1]) { ja--; jb--; }
                std::string from, to; for (size_t k = i; k < ja; k++) from += wa[k] + " "; for (size_t k = i; k < jb; k++) to += wb[k] + " ";
                std::cout << "note " << n["id"] << ": [" << from << "] -> [" << to << "]\n";
            }
        }
    }
    std::cout << changed << " of " << lines << " lines changed\n";
}
