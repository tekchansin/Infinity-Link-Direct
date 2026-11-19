// SPEC §5.7: FLAC frames → the stream's samples (libFLAC's stream decoder). See flac.h.
#ifdef ILINK_FLAC
#include "flac.h"

#include <string.h>

#include <FLAC/stream_decoder.h>

#include "infinitylink/link.h"

namespace ilink {
namespace flac {

struct Decoder::Impl {
    FLAC__StreamDecoder* d = nullptr;
    int bits = 0;
    const uint8_t* in = nullptr;  // the chunk being decoded: what is left of it
    size_t left = 0;
    std::vector<uint8_t>* out = nullptr;
    bool bad = false;

    // the chunk's bytes, then "the end": the next chunk starts with flush(), which makes the decoder look for a frame again
    static FLAC__StreamDecoderReadStatus read(const FLAC__StreamDecoder*, FLAC__byte b[], size_t* n, void* self) {
        Impl& p = *static_cast<Impl*>(self);
        if (!p.left) { *n = 0; return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM; }
        *n = *n < p.left ? *n : p.left;
        memcpy(b, p.in, *n);
        p.in += *n;
        p.left -= *n;
        return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    }
    static FLAC__StreamDecoderWriteStatus wrote(const FLAC__StreamDecoder*, const FLAC__Frame* f, const FLAC__int32* const ch[], void* self) {
        Impl& p = *static_cast<Impl*>(self);
        const size_t b = (size_t)p.bits / 8, n = f->header.blocksize;
        // the stream's own format only; and never more than a pcm chunk holds — a few bytes of FLAC can claim 65535
        // samples of silence, a megabyte of them would be gigabytes here
        if (f->header.channels != 2 || (int)f->header.bits_per_sample != p.bits || p.out->size() + n * 2 * b > kMaxFrame) {
            p.bad = true;
            return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        }
        const size_t at = p.out->size();
        p.out->resize(at + n * 2 * b);
        uint8_t* o = p.out->data() + at;
        for (size_t i = 0; i < n; i++)
            for (int c = 0; c < 2; c++) {
                const uint32_t v = (uint32_t)ch[c][i];
                for (size_t k = 0; k < b; k++) *o++ = (uint8_t)(v >> (8 * k));
            }
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }
    // lost sync, a bad header, a checksum that does not match: whole frames in order never do that
    static void failed(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus, void* self) { static_cast<Impl*>(self)->bad = true; }
};

Decoder::Decoder() : p_(new Impl) {}
Decoder::~Decoder() {
    if (p_->d) FLAC__stream_decoder_delete(p_->d);
}

bool Decoder::open(int bits) {
    if (p_->d) FLAC__stream_decoder_delete(p_->d);
    p_->d = FLAC__stream_decoder_new();
    p_->bits = bits;
    p_->bad = false;
    p_->left = 0;
    if (!p_->d) return false;
    if (FLAC__stream_decoder_init_stream(p_->d, Impl::read, nullptr, nullptr, nullptr, nullptr, Impl::wrote, nullptr, Impl::failed, p_.get()) ==
        FLAC__STREAM_DECODER_INIT_STATUS_OK)
        return true;
    FLAC__stream_decoder_delete(p_->d);
    p_->d = nullptr;
    return false;
}

bool Decoder::decode(const uint8_t* data, size_t n, std::vector<uint8_t>& pcm) {
    pcm.clear();
    if (!p_->d || p_->bad) return false;
    p_->in = data, p_->left = n, p_->out = &pcm;
    // a chunk that begins with fLaC begins a FLAC stream — the first, or the next one after the sender sent what its
    // encoder held (a frame never begins with those bytes: its sync code is 0xFFF8…). Any other chunk: the last one ended
    // at "the end", look for the next frame
    if (n >= 4 && !memcmp(data, "fLaC", 4)) {
        if (!FLAC__stream_decoder_reset(p_->d)) return false;
    } else if (FLAC__stream_decoder_get_state(p_->d) == FLAC__STREAM_DECODER_END_OF_STREAM && !FLAC__stream_decoder_flush(p_->d)) return false;
    for (;;) {
        const FLAC__StreamDecoderState s = FLAC__stream_decoder_get_state(p_->d);
        if (s == FLAC__STREAM_DECODER_END_OF_STREAM) break;  // the chunk is used up
        if (s > FLAC__STREAM_DECODER_END_OF_STREAM || p_->bad || !FLAC__stream_decoder_process_single(p_->d)) return false;
    }
    return !p_->bad;
}

}  // namespace flac
}  // namespace ilink
#endif
