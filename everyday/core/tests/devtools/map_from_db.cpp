#include "sve_store.h"
#include "sve_brainmap.h"
#include "sve_extract.h"
#include <iostream>
using nlohmann::json;
int main(int argc, char **argv) {
    sve::Store st(argv[1]);
    std::cerr << "dropped " << st.drop_mentioned_only_people() << "\n";
    std::cerr << "moved " << st.reclassify_non_people(sve::looks_like_person_name, sve::looks_like_description) << "\n";
    json f = json::object();
    json mem = st.memory_items_with_topics(f);
    std::cerr << "memories " << mem.size() << " people " << st.topics("person").size() << " topics " << st.topics("topic").size() << "\n";
    json m = sve::build_brain_map(mem, st.topics("person"), st.topics("topic"), json::array(), 0, 6, st.relations({{"status","accepted"}}), st.note_topic_links());
    std::cout << m.dump(1).substr(0, 100000) << "\n";
}
