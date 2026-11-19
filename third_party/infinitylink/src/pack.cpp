#include "infinitylink/pack.h"

#include <algorithm>

namespace ilink {

const char* containerName(Container c) {
    switch (c) {
        case Container::S16_LE: return "S16_LE";
        case Container::S24_3LE: return "S24_3LE";
        case Container::S24_LE: return "S24_LE";
        case Container::S32_LE: return "S32_LE";
        case Container::DSD_U8: return "DSD_U8";
        case Container::DSD_U16_LE: return "DSD_U16_LE";
        case Container::DSD_U16_BE: return "DSD_U16_BE";
        case Container::DSD_U32_LE: return "DSD_U32_LE";
        case Container::DSD_U32_BE: return "DSD_U32_BE";
        case Container::DOP_S24_3LE: return "DoP S24_3LE";
        case Container::DOP_S32_LE: return "DoP S32_LE";
    }
    return "?";
}

bool isDsd(Container c) { return c >= Container::DSD_U8 && c <= Container::DSD_U32_BE; }
bool isDop(Container c) { return c == Container::DOP_S24_3LE || c == Container::DOP_S32_LE; }

int containerBytes(Container c) {
    switch (c) {
        case Container::S16_LE: case Container::DSD_U16_LE: case Container::DSD_U16_BE: return 2;
        case Container::S24_3LE: case Container::DOP_S24_3LE: return 3;
        case Container::DSD_U8: return 1;
        default: return 4;
    }
}

int framesPer(Container c) {
    if (c == Container::DSD_U32_LE || c == Container::DSD_U32_BE) return 4;
    if (c == Container::DSD_U16_LE || c == Container::DSD_U16_BE || isDop(c)) return 2;
    return 1;
}

bool canCarry(const Format& f, Container c) {
    if (f.kind == "dsd") return (f.layout == "u8" || f.layout == "u32be") && (isDsd(c) || isDop(c));  // any layout regroups
    if (f.kind != "pcm" || isDsd(c) || isDop(c)) return false;
    if (f.bits == 32) return c == Container::S32_LE;
    if (f.bits == 24) return c != Container::S16_LE;
    return f.bits == 16;
}

int deviceRate(const Format& f, Container c) { return f.kind == "dsd" ? f.rate / (8 * framesPer(c)) : f.rate; }

void Packer::reset(const Format& f, Container c) {
    f_ = f;
    c_ = c;
    carry_.clear();
    fa_ = false;
}

size_t Packer::pack(const uint8_t* in, size_t frames, std::vector<uint8_t>& out) {
    const int ch = f_.channels;
    if (f_.kind == "pcm") {
        const int ib = f_.bits / 8, ob = containerBytes(c_);
        const size_t n = frames * (size_t)ch, at = out.size();
        out.resize(at + n * (size_t)ob);
        uint8_t* o = out.data() + at;
        for (size_t i = 0; i < n; i++, in += ib, o += ob) {
            // the sample left-justified in 4 bytes (lowest byte first): 16 → x x b0 b1, 24 → x b0 b1 b2
            uint8_t s[4] = {0, 0, 0, 0};
            for (int k = 0; k < ib; k++) s[4 - ib + k] = in[k];
            switch (c_) {
                case Container::S16_LE: o[0] = s[2]; o[1] = s[3]; break;
                case Container::S24_3LE: o[0] = s[1]; o[1] = s[2]; o[2] = s[3]; break;
                case Container::S24_LE: o[0] = s[1]; o[1] = s[2]; o[2] = s[3]; o[3] = (s[3] & 0x80) ? 0xFF : 0; break;
                default: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = s[3]; break;
            }
        }
        return frames;
    }
    // DSD (SPEC §5.3): u8 = L R L R …, a byte each; u32be = 4 bytes of L, 4 of R (4 frames). A unit = whole input groups
    // and whole device frames of `per` frames each (per and the group are 1, 2 or 4: the larger is a multiple of the other)
    const int per = framesPer(c_), grp = f_.frameGroup();
    const size_t unit = (size_t)std::max(per, grp), ub = unit * (size_t)ch;  // frames, bytes in a unit
    const bool same = (per == 1 && grp == 1) || (grp == 4 && c_ == Container::DSD_U32_BE);  // the device's own layout
    if (same && carry_.empty() && frames % unit == 0) {  // as it is: one copy
        out.insert(out.end(), in, in + frames * (size_t)ch);
        return frames / (size_t)per;
    }
    carry_.insert(carry_.end(), in, in + frames * (size_t)ch);
    const size_t units = carry_.size() / ub, devPerUnit = unit / (size_t)per;
    const int ob = containerBytes(c_);
    const size_t at = out.size();
    out.resize(at + units * devPerUnit * (size_t)ch * (size_t)ob);
    uint8_t* o = out.data() + at;
    for (size_t u = 0; u < units; u++) {
        const uint8_t* g = carry_.data() + u * ub;
        for (size_t d = 0; d < devPerUnit; d++) {
            for (int c = 0; c < ch; c++, o += ob) {
                auto old = [&](int k) {  // the k-th oldest byte of channel c in this device frame
                    const size_t i = d * (size_t)per + (size_t)k;  // its frame within the unit
                    return grp == 4 ? g[i / 4 * 4 * (size_t)ch + (size_t)c * 4 + i % 4] : g[i * (size_t)ch + (size_t)c];
                };
                switch (c_) {
                    case Container::DSD_U8: o[0] = old(0); break;
                    case Container::DSD_U16_BE: o[0] = old(0); o[1] = old(1); break;
                    case Container::DSD_U16_LE: o[0] = old(1); o[1] = old(0); break;
                    case Container::DSD_U32_BE: for (int k = 0; k < 4; k++) o[k] = old(k); break;
                    case Container::DSD_U32_LE: for (int k = 0; k < 4; k++) o[k] = old(3 - k); break;
                    case Container::DOP_S24_3LE: o[0] = old(1); o[1] = old(0); o[2] = fa_ ? 0xFA : 0x05; break;
                    case Container::DOP_S32_LE: o[0] = 0; o[1] = old(1); o[2] = old(0); o[3] = fa_ ? 0xFA : 0x05; break;
                    default: break;
                }
            }
            if (isDop(c_)) fa_ = !fa_;  // one marker per frame, the same in every channel
        }
    }
    carry_.erase(carry_.begin(), carry_.begin() + (ptrdiff_t)(units * ub));
    return units * devPerUnit;
}

}  // namespace ilink
