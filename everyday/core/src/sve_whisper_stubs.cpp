// whisper.cpp (core/src/whisper.cpp) references the ANEForge (Neural Engine) encoder. It is off in this app (it
// loads only when ANEFORGE_ENCODER is set), so these stubs keep the linker happy.
#include "aneforge/whisper-aneforge.h"
#include <cstdint>

extern "C" whisper_aneforge_context *whisper_aneforge_init(const char *) { return nullptr; }
extern "C" void whisper_aneforge_encode(whisper_aneforge_context *, int64_t, int64_t, const float *, float *) {}
extern "C" void whisper_aneforge_free(whisper_aneforge_context *) {}
