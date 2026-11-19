// Lossless compression of a PCM stream (SPEC §5.7, codec "flac"): libFLAC behind two small classes. Compiled in only
// with ILINK_FLAC — the host then gives libFLAC 1.4 or newer (32-bit samples): an endpoint builds flac_dec.cpp and links
// libFLAC's decoder, a sender builds flac_enc.cpp and links its encoder.
#pragma once
#ifdef ILINK_FLAC
#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <memory>
#include <vector>

namespace ilink {
namespace flac {

// The stream's samples in (2 channels, little-endian, 16 / 24 / 32 bits a sample as SPEC §5.1), FLAC frames out.
class Encoder {
public:
    // one audio chunk: a whole FLAC frame of `frames` sample frames — the first one behind the stream's header (fLaC,
    // STREAMINFO). False = stop (the connection went).
    using Out = std::function<bool(const uint8_t* bytes, size_t n, uint32_t frames)>;
    Encoder();
    ~Encoder();  // what it still holds is dropped (a flush): nothing more goes out
    bool open(int rate, int bits, Out out);         // false = libFLAC does not take this format: the stream goes as pcm
    bool write(const uint8_t* pcm, size_t frames);  // out() for every frame that fills up
    bool finish();                                  // the last, shorter frame goes out (the stream's end)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// FLAC frames in, the stream's samples out.
class Decoder {
public:
    Decoder();
    ~Decoder();
    bool open(int bits);  // a new stream of this depth
    // one audio chunk (whole frames; one that begins with fLaC begins a FLAC stream — the first, or the next one in the
    // same Link stream) → its samples (2 channels, little-endian, `bits` deep) in `pcm`, cleared first. False = not FLAC,
    // another format than the stream's, or more samples than a pcm chunk may hold (kMaxFrame): the stream is broken.
    bool decode(const uint8_t* data, size_t n, std::vector<uint8_t>& pcm);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace flac
}  // namespace ilink
#endif
