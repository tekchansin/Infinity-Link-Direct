#include "infinitylink/link.h"

#include <chrono>
#include <stdlib.h>
#include <string.h>

#include "net.h"

namespace ilink {

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string base64(const std::string& in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t v = (uint8_t)in[i] << 16 | (uint8_t)in[i + 1] << 8 | (uint8_t)in[i + 2];
        for (int k = 18; k >= 0; k -= 6) out += kB64[(v >> k) & 63];
    }
    if (i < in.size()) {
        uint32_t v = (uint8_t)in[i] << 16 | (i + 1 < in.size() ? (uint8_t)in[i + 1] << 8 : 0);
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += i + 1 < in.size() ? kB64[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}
std::string unbase64(const std::string& in) {
    int map[256];
    for (int& x : map) x = -1;
    for (int k = 0; k < 64; k++) map[(uint8_t)kB64[k]] = k;
    std::string out;
    out.reserve(in.size() / 4 * 3);
    uint32_t acc = 0;
    int bits = 0;
    for (char ch : in) {
        if (ch == '=') break;
        const int d = map[(uint8_t)ch];
        if (d < 0) return "";
        acc = acc << 6 | (uint32_t)d;
        bits += 6;
        if (bits >= 8) { bits -= 8; out += (char)((acc >> bits) & 0xFF); }
    }
    return out;
}

json::Value message(const char* type) {
    json::Value v;
    v.set("t", json::str(type));
    return v;
}

json::Value Format::toJson() const {
    json::Value v;
    v.set("kind", json::str(kind));
    v.set("rate", json::num(rate));
    if (kind != "dsd") v.set("bits", json::num(bits));
    v.set("channels", json::num(channels));
    if (kind == "dsd" && layout != "u8") v.set("layout", json::str(layout));  // u8 = absent: as before (SPEC §5.3)
    if (codec != "pcm") v.set("codec", json::str(codec));                      // pcm = absent: as before (SPEC §5.7)
    return v;
}
Format Format::fromJson(const json::Value& v) {
    Format f;
    f.kind = v["kind"].str("pcm");
    f.rate = (int)v["rate"].i64();
    f.bits = (int)v["bits"].i64();
    f.channels = (int)v["channels"].i64(2);
    if (f.kind == "dsd") f.layout = v["layout"].str("u8");
    f.codec = v["codec"].str("pcm");
    return f;
}

json::Value Track::toJson() const {
    json::Value v;
    v.set("title", json::str(title));
    v.set("artist", json::str(artist));
    v.set("album", json::str(album));
    if (!art.empty()) v.set("art", json::str(art));
    if (durationMs) v.set("duration_ms", json::num(durationMs));
    if (positionMs > 0) v.set("position_ms", json::num(positionMs));
    if (!why.empty()) v.set("why", json::str(why));
    if (!codec.empty()) {
        json::Value f;
        f.set("codec", json::str(codec));
        if (srcRate) f.set("rate", json::num(srcRate));
        if (srcBits) f.set("bits", json::num(srcBits));
        v.set("format", f);
    }
    return v;
}
Track Track::fromJson(const json::Value& v) {
    Track t;
    t.title = v["title"].str();
    t.artist = v["artist"].str();
    t.album = v["album"].str();
    t.art = v["art"].str();
    t.durationMs = v["duration_ms"].i64();
    t.positionMs = v["position_ms"].i64() > 0 ? v["position_ms"].i64() : 0;
    t.why = v["why"].str();
    t.codec = v["format"]["codec"].str();
    t.srcRate = (int)v["format"]["rate"].i64();
    t.srcBits = (int)v["format"]["bits"].i64();
    return t;
}

void Conn::close() {
    if (fd_ >= 0) { ::shutdown(fd_, SHUT_RDWR); net::closeSocket(fd_); }
    fd_ = -1;
}
void Conn::shutdown() {
    if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
}

bool Conn::writeAll(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    sentAt_ = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    while (n) {
        long k = net::sendBytes(fd_, b, n);
        if (k <= 0) return false;
        b += k;
        n -= (size_t)k;
    }
    return true;
}
bool Conn::readAll(void* p, size_t n) {
    uint8_t* b = static_cast<uint8_t*>(p);
    while (n) {
        long k = net::recvBytes(fd_, b, n);
        if (k <= 0) return false;
        b += k;
        n -= (size_t)k;
    }
    return true;
}

static void putBe32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint32_t getBe32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void putBe64(uint8_t* p, int64_t v) { putBe32(p, (uint32_t)((uint64_t)v >> 32)); putBe32(p + 4, (uint32_t)v); }
static int64_t getBe64(const uint8_t* p) { return (int64_t)((uint64_t)getBe32(p) << 32 | getBe32(p + 4)); }

bool Conn::send(const json::Value& msg) {
    std::string body = json::dump(msg);
    if (body.size() + 1 > kMaxFrame) return false;
    uint8_t head[5];
    putBe32(head, (uint32_t)body.size() + 1);
    head[4] = kMessage;
    std::lock_guard<std::mutex> l(sendMu_);
    return fd_ >= 0 && writeAll(head, 5) && writeAll(body.data(), body.size());
}

bool Conn::sendAudio(uint32_t stream, uint64_t first, const void* samples, size_t bytes) {
    if (bytes + 13 > kMaxFrame) return false;
    uint8_t head[17];
    putBe32(head, (uint32_t)(bytes + 13));
    head[4] = kAudio;
    putBe32(head + 5, stream);
    putBe32(head + 9, (uint32_t)(first >> 32));
    putBe32(head + 13, (uint32_t)first);
    std::lock_guard<std::mutex> l(sendMu_);
    return fd_ >= 0 && writeAll(head, 17) && writeAll(samples, bytes);
}

// SPEC §3 0x03: kind u8 · stream u32 · id_len u16 + id · mime_len u8 + mime · offset i64 · size i64 · the bytes
bool Conn::sendData(uint8_t kind, uint32_t stream, const std::string& id, const std::string& mime, int64_t offset, int64_t size,
                    const void* bytes, size_t n) {
    if (id.size() > 0xFFFF || mime.size() > 0xFF || 1 + kDataHead + id.size() + mime.size() + n > kMaxFrame) return false;
    uint8_t head[12], tail[17];
    putBe32(head, (uint32_t)(1 + kDataHead + id.size() + mime.size() + n));
    head[4] = kData;
    head[5] = kind;
    putBe32(head + 6, stream);
    head[10] = (uint8_t)(id.size() >> 8), head[11] = (uint8_t)id.size();
    tail[0] = (uint8_t)mime.size();
    putBe64(tail + 1, offset);
    putBe64(tail + 9, size);
    std::lock_guard<std::mutex> l(sendMu_);
    return fd_ >= 0 && writeAll(head, 12) && writeAll(id.data(), id.size()) && writeAll(tail, 1) &&
           writeAll(mime.data(), mime.size()) && writeAll(tail + 1, 16) && writeAll(bytes, n);
}

bool Conn::read(Frame& f) {
    uint8_t head[5];
    if (fd_ < 0 || !readAll(head, 5)) return false;
    readAt_ = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    uint32_t len = getBe32(head);
    if (len < 1 || len > kMaxFrame) return false;
    f.type = head[4];
    f.msg = json::Value();
    if (f.type == kAudio) {  // the samples go straight into f.audio: resize keeps its capacity, no copy, no allocation
        uint8_t ah[12];
        if (len < 13 || !readAll(ah, 12)) return false;
        f.stream = getBe32(ah);
        f.first = (uint64_t)getBe32(ah + 4) << 32 | getBe32(ah + 8);
        f.text.clear();
        f.audio.resize(len - 13);
        return f.audio.empty() || readAll(f.audio.data(), f.audio.size());
    }
    if (f.type == kData) {  // like audio: the header's strings and the bytes go into the Frame's own buffers
        uint8_t h[16];
        if (len < 1 + kDataHead || !readAll(h, 7)) return false;
        f.kind = h[0];
        f.stream = getBe32(h + 1);
        f.id.resize((size_t)h[5] << 8 | h[6]);
        if (len < 1 + kDataHead + f.id.size() || (!f.id.empty() && !readAll(&f.id[0], f.id.size())) || !readAll(h, 1)) return false;
        f.mime.resize(h[0]);
        const size_t head = 1 + kDataHead + f.id.size() + f.mime.size();
        if (len < head || (!f.mime.empty() && !readAll(&f.mime[0], f.mime.size())) || !readAll(h, 16)) return false;
        f.offset = getBe64(h);
        f.size = getBe64(h + 8);
        f.text.clear();
        f.audio.resize(len - head);
        return f.audio.empty() || readAll(f.audio.data(), f.audio.size());
    }
    f.audio.clear();
    f.text.resize(len - 1);
    if (!f.text.empty() && !readAll(&f.text[0], f.text.size())) return false;
    if (f.type == kMessage) json::parse(f.text, f.msg);
    return true;  // an unknown type: the caller skips it (SPEC §3)
}

int connectTcp(const std::string& hostPort, int port, std::string& err) {
    std::string host = hostPort;
    size_t c = host.rfind(':');
    if (c != std::string::npos && host.find(':') == c) {  // "host:port" (one colon: not an IPv6 literal)
        port = atoi(host.c_str() + c + 1);
        host = host.substr(0, c);
    }
    net::init();
    addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    std::string ps = std::to_string(port);
    if (int rc = getaddrinfo(host.c_str(), ps.c_str(), &hints, &res); rc != 0) {
#ifdef _WIN32
        err = gai_strerrorA(rc);  // a UNICODE build maps gai_strerror to the wide one
#else
        err = gai_strerror(rc);
#endif
        return -1;
    }
    int fd = -1;
    for (addrinfo* a = res; a; a = a->ai_next) {
        fd = (int)socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        if (net::connectWithin(fd, a->ai_addr, (size_t)a->ai_addrlen, 5000)) break;  // a LAN peer answers within ms
        net::closeSocket(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) { err = "cannot connect to " + host + ":" + ps; return -1; }
    net::setInt(fd, IPPROTO_TCP, TCP_NODELAY, 1);
    net::noSigpipe(fd);
    return fd;
}

}  // namespace ilink
