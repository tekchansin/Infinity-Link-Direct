// The DAC's own layouts (docs/ENDPOINTS.md "one packer"): protocol samples in, the device's container out. Bytes are
// moved and PCM is widened, never a value changed — every endpoint uses this instead of writing its own.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "infinitylink/link.h"

namespace ilink {

enum class Container {
    S16_LE, S24_3LE, S24_LE, S32_LE,                               // PCM (S24_LE: 24 bits in the low 3 of 4 bytes)
    DSD_U8, DSD_U16_LE, DSD_U16_BE, DSD_U32_LE, DSD_U32_BE,        // native DSD (BE: oldest byte first)
    DOP_S24_3LE, DOP_S32_LE,                                       // DSD over PCM (DoP 1.1)
};

const char* containerName(Container c);
bool isDsd(Container c);   // a native DSD layout
bool isDop(Container c);
int containerBytes(Container c);  // bytes per channel in one device frame
// DSD: protocol frames are 8 bits a channel in either layout (SPEC §5.3), so these do not depend on Format::layout
int framesPer(Container c);       // protocol frames in one device frame: DSD_U32 4, DSD_U16 and DoP 2, else 1
bool canCarry(const Format& f, Container c);  // f fits c without a value changing (DSD: either layout, regrouped)
int deviceRate(const Format& f, Container c);  // the device's frame rate for f

class Packer {
public:
    void reset(const Format& f, Container c);  // a new stream: carried bytes dropped, the DoP marker starts again
    // protocol frames in → whole device frames appended to `out` (a part frame waits for the next call); device frames.
    // DSD u32be into DSD_U32_BE (and u8 into DSD_U8) is one copy
    size_t pack(const uint8_t* in, size_t frames, std::vector<uint8_t>& out);
    Container container() const { return c_; }

private:
    Format f_;
    Container c_ = Container::S32_LE;
    std::vector<uint8_t> carry_;  // DSD: protocol frames short of a device frame (or of a u32be group)
    bool fa_ = false;             // DoP: the next marker is 0xFA (else 0x05)
};

}  // namespace ilink
