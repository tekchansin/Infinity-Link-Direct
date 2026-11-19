// SPEC §5.7: a PCM stream → FLAC frames (libFLAC's stream encoder). See flac.h.
#ifdef ILINK_FLAC
#include "flac.h"

#include <FLAC/stream_encoder.h>

namespace ilink {
namespace flac {

// ponytail: libFLAC's own default level at every rate (LPC order 8, 4096-sample blocks at 44.1 kHz). A sender too weak
// for it at its highest rates runs behind real time and the endpoint drops out — lower it per rate then (0–2 = fixed
// predictors, about a third of the work, a few percent larger).
constexpr int kLevel = 5;

struct Encoder::Impl {
    FLAC__StreamEncoder* e = nullptr;
    Out out;
    int bytes = 0;               // a sample's
    std::vector<uint8_t> head;   // the stream's header, until the first frame takes it along
    std::vector<FLAC__int32> in;

    static FLAC__StreamEncoderWriteStatus wrote(const FLAC__StreamEncoder*, const FLAC__byte b[], size_t n, uint32_t samples, uint32_t,
                                                void* self) {
        Impl& p = *static_cast<Impl*>(self);
        if (!p.out) return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;  // dropped: nothing more goes out
        if (!samples) {  // fLaC, STREAMINFO, the vendor comment
            p.head.insert(p.head.end(), b, b + n);
            return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
        }
        bool ok;
        if (p.head.empty()) ok = p.out(b, n, samples);
        else {
            p.head.insert(p.head.end(), b, b + n);
            ok = p.out(p.head.data(), p.head.size(), samples);
            std::vector<uint8_t>().swap(p.head);
        }
        return ok ? FLAC__STREAM_ENCODER_WRITE_STATUS_OK : FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
    }
};

Encoder::Encoder() : p_(new Impl) {}
Encoder::~Encoder() {
    p_->out = nullptr;
    if (p_->e) FLAC__stream_encoder_delete(p_->e);  // it finishes first: the write above refuses that last frame
}

bool Encoder::open(int rate, int bits, Out out) {
    if (p_->e || (bits != 16 && bits != 24 && bits != 32)) return false;
    FLAC__StreamEncoder* e = FLAC__stream_encoder_new();
    if (!e) return false;
    // about 90 ms a frame at every rate (4096 samples at 44.1 / 48 kHz, FLAC's usual size there), within the streamable
    // subset's limits (4608 up to 48 kHz, 16384 above)
    uint32_t block = 4096;
    for (int r = 48000; r < rate && block < 16384; r *= 2) block *= 2;
    p_->out = std::move(out);
    p_->bytes = bits / 8;
    // an unusual rate the streamable subset cannot name (705.6 / 768 kHz) fails in init: that stream goes as pcm
    const bool ok = FLAC__stream_encoder_set_channels(e, 2) && FLAC__stream_encoder_set_bits_per_sample(e, (uint32_t)bits) &&
                    FLAC__stream_encoder_set_sample_rate(e, (uint32_t)rate) && FLAC__stream_encoder_set_compression_level(e, kLevel) &&
                    FLAC__stream_encoder_set_blocksize(e, block) &&
                    FLAC__stream_encoder_init_stream(e, Impl::wrote, nullptr, nullptr, nullptr, p_.get()) == FLAC__STREAM_ENCODER_INIT_STATUS_OK;
    if (!ok) {
        p_->out = nullptr;
        p_->head.clear();
        FLAC__stream_encoder_delete(e);
        return false;
    }
    p_->e = e;
    return true;
}

bool Encoder::write(const uint8_t* pcm, size_t frames) {
    if (!p_->e) return false;
    const int b = p_->bytes;
    p_->in.resize(frames * 2);
    for (size_t i = 0; i < frames * 2; i++, pcm += b) {
        // little-endian, the top byte carries the sign
        int32_t v = (int32_t)(int8_t)pcm[b - 1];
        for (int k = b - 2; k >= 0; k--) v = (int32_t)((uint32_t)v << 8 | pcm[k]);
        p_->in[i] = v;
    }
    return FLAC__stream_encoder_process_interleaved(p_->e, p_->in.data(), (uint32_t)frames);
}

bool Encoder::finish() { return p_->e && FLAC__stream_encoder_finish(p_->e); }

}  // namespace flac
}  // namespace ilink
#endif
