// Infinity Link — the wire format (SPEC.md §3–§5): frames over one TCP connection, JSON messages and audio chunks.
#pragma once
#include <stdint.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "infinitylink/json.h"

namespace ilink {

constexpr int kProto = 1;     // the version every endpoint speaks (a version 2 sender falls back to it)
constexpr int kProtoMax = 2;  // the newest this library speaks (SPEC §10)
constexpr int kDefaultPort = 4720;
constexpr uint32_t kMaxFrame = 1u << 20;  // 1 MiB, type byte included
enum : uint8_t { kMessage = 0x01, kAudio = 0x02, kData = 0x03 };
// a data frame's kind (SPEC §3 0x03): the bytes of a library_read / library_art answer, or of the sender's art
enum : uint8_t { kDataLibraryRead = 1, kDataLibraryArt = 2, kDataArt = 3 };
constexpr size_t kDataHead = 24;  // a data frame's payload before id, mime and the bytes (kind, stream, lengths, offset, size)

struct Format {
    std::string kind = "pcm";  // pcm | dsd
    int rate = 0;              // pcm: sample rate; dsd: bit rate per channel
    int bits = 0;              // pcm: 16 | 24 | 32 (24 = 3 bytes packed); dsd: unused
    int channels = 2;
    // dsd (SPEC §5.3): "u8" = a byte per channel by turns; "u32be" = 4 bytes of channel 0 (oldest first), 4 of channel 1 …
    // — the DAP's DSD_U32_BE. Frames count 8 bits a channel in both; a u32be stream moves in groups of 4 of them
    std::string layout = "u8";
    // how the samples travel (SPEC §5.7): "pcm" as they are, or "flac" — a lossless packing of PCM that the endpoint
    // undoes before anything else sees it. Not part of ==: the same samples reach the device either way (one stays open)
    std::string codec = "pcm";
    int bytesPerFrame() const { return kind == "dsd" ? channels : channels * (bits / 8); }
    int frameRate() const { return kind == "dsd" ? rate / 8 : rate; }  // frames a second (DSD: a frame = 8 bits a channel)
    int frameGroup() const { return kind == "dsd" && layout == "u32be" ? 4 : 1; }  // chunks hold whole groups of frames
    bool valid() const {
        return rate > 0 && channels > 0 &&
               (kind == "dsd" ? layout == "u8" || layout == "u32be" : kind == "pcm" && (bits == 16 || bits == 24 || bits == 32));
    }
    bool operator==(const Format& o) const { return kind == o.kind && rate == o.rate && bits == o.bits && channels == o.channels && layout == o.layout; }
    bool operator!=(const Format& o) const { return !(*this == o); }
    json::Value toJson() const;
    static Format fromJson(const json::Value& v);
};

struct Track {
    std::string title, artist, album, art;
    long long durationMs = 0;
    std::string codec;              // the file as the sender reads it (SPEC §5.1): "FLAC", "DSF" …; "" = not given
    int srcRate = 0, srcBits = 0;   // its own rate and depth (DSD: the DSD rate, 1) — what the endpoint can show
    long long positionMs = 0;       // where in the track this stream begins (SPEC §5.1 track.position_ms): after a seek
    std::string why;                // why the sender streams it, not the endpoint itself (SPEC §5.1 track.why): only_here …
    json::Value toJson() const;
    static Track fromJson(const json::Value& v);
};

// One received frame: a message (json), an audio chunk (stream, first, bytes) or data (kind, stream, id, mime, offset,
// size, bytes). Read frame after frame into the same Frame, it allocates nothing for audio or data once `audio` has grown
// to the largest chunk (a reader that moves `audio` away gives it a used buffer back — Endpoint keeps a few).
struct Frame {
    uint8_t type = 0;
    json::Value msg;
    std::string text;  // a message's JSON as it came (the payload of a frame of unknown type too)
    uint32_t stream = 0;
    uint64_t first = 0;
    std::vector<uint8_t> audio;  // an audio chunk's samples, a data frame's bytes
    // a data frame (SPEC §3 0x03): kind (kData…), the track id, the image's mime, offset and size as §3 says
    uint8_t kind = 0;
    std::string id, mime;
    int64_t offset = 0, size = 0;
};

// A connected socket that speaks frames. Sends are serialized (several threads may send); reads come from one thread.
class Conn {
public:
    explicit Conn(int fd = -1) : fd_(fd) {}
    ~Conn() { close(); }
    Conn(const Conn&) = delete;
    Conn& operator=(const Conn&) = delete;
    bool open() const { return fd_ >= 0; }
    int fd() const { return fd_; }
    void close();     // the socket goes; a blocked read returns false
    void shutdown();  // wakes a thread blocked in read() without closing the descriptor under it
    bool send(const json::Value& msg);
    bool sendAudio(uint32_t stream, uint64_t first, const void* samples, size_t bytes);
    // a data frame (SPEC §3 0x03); false = it would not fit one frame (id ≤ 65535 bytes, mime ≤ 255) or the socket failed
    bool sendData(uint8_t kind, uint32_t stream, const std::string& id, const std::string& mime, int64_t offset, int64_t size,
                  const void* bytes, size_t n);
    bool read(Frame& f);  // blocks; false = closed or broken
    // steady-clock ms of the last frame sent (SPEC §4.2: a sender pings only when it has sent nothing for a while)
    long long sentAt() const { return sentAt_.load(); }
    // … and of the last frame read: a peer silent for 15 s may be closed (SPEC §4.2)
    long long readAt() const { return readAt_.load(); }

    // what the endpoint learnt from its hello (SPEC §4.1): the version agreed, the role, the sender's id, whether it reads
    // data frames (hello binary: library answers come as 0x03, not base64)
    int proto = 1;
    std::string role = "stream", senderId;
    bool binary = false;
    bool burst = false;  // hello burst: it refills in bursts (SPEC §5.5) — the endpoint may offer it its deep buffer
    std::string peer;    // the address it connected from (an endpoint's accept): "127.0.0.1" = a forwarder on this machine

private:
    bool writeAll(const void* p, size_t n);
    bool readAll(void* p, size_t n);
    int fd_;
    std::mutex sendMu_;
    std::atomic<long long> sentAt_{0};
    std::atomic<long long> readAt_{0};
};

// "host" or "host:port" → a connected TCP socket (-1 + err on failure)
int connectTcp(const std::string& host, int port, std::string& err);
json::Value message(const char* type);
std::string base64(const std::string& bytes);
std::string unbase64(const std::string& text);  // "" on bad input  // {"t": type}

}  // namespace ilink
