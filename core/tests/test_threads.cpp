#include "check.h"
#include "sve_threads.h"

using namespace sve;

// Prefill and the audio encoder: up to 8 cores, never fewer than decoding (N5's rule, sve_llm load).
TEST(batch_threads_up_to_eight_never_below_decode) {
    CHECK_EQ(batch_threads(6, 4), 6);    // the iPhone 16e
    CHECK_EQ(batch_threads(4, 4), 4);
    CHECK_EQ(batch_threads(16, 4), 8);   // capped at 8
    CHECK_EQ(batch_threads(8, 10), 10);  // never fewer than decoding
}

TEST(decode_threads_default_is_up_to_four) {
    CHECK_EQ(decode_threads(6, 0), 4);
    CHECK_EQ(decode_threads(2, 0), 2);
    CHECK_EQ(decode_threads(0, 0), 1);   // unknown core count
    CHECK_EQ(decode_threads(6, 2), 2);   // asked for: kept
}

// sve_set_threads: n_batch 0 means the default rule for the decode count asked for (the heat ladder's full and
// eased levels); a count given is kept (minimal: 2 and 2).
TEST(runtime_threads_resolve) {
    ThreadPair a = resolve_threads(6, 4, 0);
    CHECK_EQ(a.decode, 4); CHECK_EQ(a.batch, 6);
    ThreadPair m = resolve_threads(6, 2, 2);
    CHECK_EQ(m.decode, 2); CHECK_EQ(m.batch, 2);
    ThreadPair d = resolve_threads(6, 2, 0);
    CHECK_EQ(d.decode, 2); CHECK_EQ(d.batch, 6);
    ThreadPair z = resolve_threads(6, 0, 0);   // 0 decode: the load-time default
    CHECK_EQ(z.decode, 4); CHECK_EQ(z.batch, 6);
}
