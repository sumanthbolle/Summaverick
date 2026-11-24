#include "sve_store.h"
#include "sve_context.h"
#include <iostream>
int main(int, char **argv) {
    sve::Store st(argv[1]);
    auto vocab = sve::saved_vocab(st);
    std::string prev;
    int dropped = 0, changed = 0, n = 0;
    for (const auto &p : st.pieces(18)) {
        std::string t = p.value("text", ""), c = sve::clean_piece(t, prev, vocab);
        n++;
        if (c.empty() && !t.empty()) dropped++; else if (c != t) changed++;
        std::cout << p.value("start_sec", 0) << " " << (c.empty() ? "DROPPED" : c == t ? "kept" : "CLEANED") << " | " << c.substr(0, 90) << "\n";
        if (!t.empty()) prev = t;
    }
    std::cerr << n << " pieces: " << dropped << " dropped, " << changed << " cleaned\n";
}
