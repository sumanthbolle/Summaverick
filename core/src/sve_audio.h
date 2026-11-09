// Internal: WAV reading, piece planning and the transcription prompt. Pure (no model), so the
// rules can be unit-tested and are shared by the whole-file and the incremental transcription.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sve {

constexpr size_t kAudioRate = 16000;
// Gemma 4 takes at most ~30 s per clip. Pieces are cut at the quietest point in the last 3 s before 28 s.
constexpr size_t kPieceTarget = 28 * kAudioRate;
constexpr size_t kPieceSearch = 3 * kAudioRate;
constexpr size_t kPieceMin = kAudioRate / 2;   // slivers under 0.5 s are not transcribed
constexpr size_t kQuietWindow = 1600;          // 0.1 s energy window

struct WavInfo {
    int channels = 1;
    int rate = 0;
    int bits = 0;
    uint64_t data_offset = 0;   // first byte of the samples
    uint64_t frames = 0;        // whole frames available (on disk, for a file still being written)
};

enum class WavProbe { ok, incomplete, bad };

// Reads the header. Tolerates a file still being written: when the data chunk size is 0, 0xFFFFFFFF
// or larger than what is on disk, the bytes on disk are used (whole frames only). `incomplete`: the
// header or the data chunk is not there yet. `bad`: not a 16 kHz 16-bit PCM WAV.
WavProbe wav_probe(const std::string &path, WavInfo &info);
// Frames [start, start + count) mixed to mono floats in [-1, 1). Clamped to info.frames.
bool wav_read(const std::string &path, const WavInfo &info, uint64_t start, uint64_t count, std::vector<float> &out);
// Whole file to mono floats (16 kHz 16-bit only).
bool read_wav16k(const std::string &path, std::vector<float> &pcm);

struct PiecePlan {
    enum Kind { wait, end, piece } kind = end;
    size_t start = 0, stop = 0;   // [start, stop) in samples, for `piece`
};

// The next piece from sample `start` of an audio of `total` samples. pcm[i] is sample offset + i and
// must cover [start, min(total, start + kPieceTarget)). Not final: `wait` until 28 s + 3 s are there
// after start. Final: `end` when under 0.5 s is left, and the last piece takes what is left.
PiecePlan plan_next_piece(const std::vector<float> &pcm, size_t offset, size_t start, size_t total, bool is_final);

// The user turn for one audio piece: the media marker, the instruction, the vocabulary line (may be
// empty) and, when there is one, the last 400 bytes of the previous piece for names and spellings.
std::string transcription_instruction(const std::string &marker, const std::string &vocab, const std::string &prev);
extern const char *const kTranscriberSystem;

// A piece whose mean token probability (conf) is under this is transcribed once more (N7).
constexpr double kRetryConf = 0.80;

} // namespace sve

// True when a piece heard with this conf gets its one second hearing: conf < 0.80. NaN (no token) is never retried.
bool sve_should_retry_piece(double conf);
// The one second hearing runs only off the live path (ruling R-N7-g): live transcription is already ~1.1x real time
// on the CPU, so a recording's pieces (sve_transcribe_next_piece with is_final 0) are heard once. After Stop (is_final
// 1), a rerun and imported audio (sve_transcribe_audio_pieces) re-hear a piece when sve_should_retry_piece.
bool sve_rehear_piece(double conf, bool live);
