#include "infinitylink/sender.h"
#include "flac.h"
#include "net.h"

#include <algorithm>
#include <chrono>
#include <iterator>

namespace ilink {

// Sent, not arriving (pinger): the endpoint holds under kDryMs while more than kStuckAheadMs is counted ahead of it, and
// that for kStuckMs. Seen once for real (20 s buffer, 15 s stuck in a tailnet's TCP); kStuckAheadMs is what a pipe that
// holds little still holds before the write blocks (two 64 KB socket buffers: ~1.5 s of CD audio).
static const int kDryMs = 400, kStuckAheadMs = 1000, kStuckMs = 3000;

// SPEC §5.7: the stream that goes as FLAC — its encoder, and the index of the next frame's first sample
struct Sender::Enc {
    uint32_t stream = 0;
    uint64_t first = 0;  // sample frames sent
    uint64_t in = 0;     // … and given: more than `first` = the encoder holds some
    int rate = 0, bits = 0;
#ifdef ILINK_FLAC
    std::unique_ptr<flac::Encoder> flac;
    // a FLAC stream of its own from here on (the Link stream's first, or the next one after sync())
    bool open(Conn& conn, std::atomic<uint64_t>& sent) {
        flac = std::make_unique<flac::Encoder>();
        return flac->open(rate, bits, [this, &conn, &sent](const uint8_t* b, size_t n, uint32_t frames) {
            const bool ok = conn.sendAudio(stream, first, b, n);
            first += frames;
            sent += n;
            return ok;
        });
    }
    bool write(const uint8_t* pcm, size_t frames) { in += frames; return flac->write(pcm, frames); }
    bool finish() { return flac->finish(); }
#else
    bool open(Conn&, std::atomic<uint64_t>&) { return false; }
    bool write(const uint8_t*, size_t) { return false; }
    bool finish() { return false; }
#endif
};

Sender::Sender() = default;
Sender::~Sender() { close(); }

bool Sender::hello(const std::string& hostPort, const SenderInfo& me, int proto, std::string& err, bool& protoRefused) {
    protoRefused = false;
    int fd = connectTcp(hostPort, kDefaultPort, err);
    if (fd < 0) return false;
    net::recvTimeout(fd, 5000);  // the welcome within 5 s: an endpoint that took the connection but never answers (wedged)
    conn_ = std::make_shared<Conn>(fd);
    gone_ = false;
    json::Value h = message("hello"), s;
    h.set("proto", json::num(proto));
    s.set("id", json::str(me.id));
    s.set("name", json::str(me.name));
    s.set("app", json::str(me.app));
    s.set("version", json::str(me.version));
    h.set("sender", s);
    h.set("binary", json::boolean(true));  // SPEC §3 0x03: library answers as data frames, not base64
    h.set("burst", json::boolean(true));   // SPEC §5.5: it honours burst_ms
    if (proto >= 2) h.set("role", json::str(me.role.empty() ? "stream" : me.role));
    else if (me.role == "library") h.set("role", json::str("library"));
    Frame f;
    if (!conn_->send(h)) { err = "the endpoint did not answer"; conn_->close(); return false; }
    // an endpoint from before 2026-11-09 can say something else first when it is being taken over (its capabilities)
    for (int i = 0; i < 8; i++) {
        if (!conn_->read(f) || f.type != kMessage) { err = "the endpoint did not answer"; conn_->close(); return false; }
        if (f.msg["t"].str() == "welcome" || f.msg["t"].str() == "error") break;
    }
    if (f.msg["t"].str() == "error") {
        protoRefused = f.msg["code"].str() == "proto";
        err = f.msg["message"].str();
        conn_->close();
        return false;
    }
    if (f.msg["t"].str() != "welcome") { err = "not an Infinity Link endpoint"; conn_->close(); return false; }
    net::recvTimeout(fd, 0);  // from now on the pinger watches for silence (pinger)
    welcome_ = f.msg;
    proto_ = (int)welcome_["proto"].i64(proto);
    return true;
}

bool Sender::connect(const std::string& hostPort, const SenderInfo& me, std::string& err) {
    close();
    bool refused = false;
    // version 2 first; an endpoint of version 1 says "proto" — then again as version 1 (a remote role needs 2)
    if (!hello(hostPort, me, kProtoMax, err, refused) && !(refused && me.role != "remote" && me.role != "control" && hello(hostPort, me, kProto, err, refused))) return false;
    {
        std::lock_guard<std::mutex> l(m_);
        native_.clear();
        if (welcome_.has("capabilities")) caps_ = welcome_["capabilities"];
        else {  // version 1: its welcome's native list
            caps_ = json::Value();
            caps_.set("native", welcome_["native"].isNull() ? json::Value() : welcome_["native"]);
            caps_.set("takeback", json::boolean(welcome_["handoff"].boolean()));
        }
        for (auto& x : caps_["native"].arr) native_.push_back(x.str());
        formats_ = welcome_["formats"];
        codecs_.clear();
        for (auto& x : welcome_["codecs"].arr) codecs_.push_back(x.str());
        results_.clear();
    }
    bufferMs_ = (int)std::clamp<long long>(welcome_["buffer_ms"].i64(2000), 200, 20000);
    burstMs_ = (int)std::clamp<long long>(welcome_["burst_ms"].i64(0), 0, bufferMs_ - 200);
    filling_ = true;
    reader_ = std::thread([this] { reader(); });
    pinger_ = std::thread([this] { pinger(); });
    return true;
}

json::Value Sender::capabilities() {
    std::lock_guard<std::mutex> l(m_);
    return caps_;
}

bool Sender::command(const std::string& action, const json::Value& fields, json::Value* result, int waitMs) {
    long long id;
    {
        std::lock_guard<std::mutex> l(m_);
        id = ++nextId_;
    }
    json::Value m = message("command");
    m.set("id", json::num(id));
    m.set("action", json::str(action));
    for (auto& kv : fields.obj) m.set(kv.first, kv.second);
    if (!control(m)) return false;
    std::unique_lock<std::mutex> l(m_);
    if (!cv_.wait_for(l, std::chrono::milliseconds(waitMs), [&] { return gone_ || results_.count(id); }) || !results_.count(id)) return false;
    json::Value r = results_[id];
    results_.erase(id);
    if (result) *result = r;
    return r["ok"].boolean();
}

void Sender::close() {
    gone_ = true;
    if (conn_) conn_->shutdown();
    cv_.notify_all();
    if (reader_.joinable()) reader_.join();
    if (pinger_.joinable()) pinger_.join();
    if (conn_) conn_->close();
    {
        std::lock_guard<std::mutex> e(encMu_);
        enc_.reset();
    }
    std::lock_guard<std::mutex> l(m_);
    sent_.clear();
    haveStatus_ = statusPlaying_ = false;
}

bool Sender::playsNative(const std::string& source) {
    std::lock_guard<std::mutex> l(m_);
    return std::find(native_.begin(), native_.end(), source) != native_.end();
}

json::Value Sender::formats() const {
    std::lock_guard<std::mutex> l(m_);
    return formats_;
}

bool Sender::supports(const Format& f) const {
    const json::Value g = formats()[f.kind];
    auto has = [](const json::Value& a, int x) { for (auto& v : a.arr) if (v.i64() == x) return true; return false; };
    if (f.channels != 2 || !has(g["rates"], f.rate)) return false;
    if (f.kind != "dsd") return has(g["bits"], f.bits);
    const auto l = dsdLayouts();
    return std::find(l.begin(), l.end(), f.layout) != l.end();
}

std::vector<std::string> Sender::dsdLayouts() const {
    std::vector<std::string> out;
    const json::Value f = formats();  // named: a range over a temporary's member reads freed memory
    for (auto& x : f["dsd"]["layouts"].arr) out.push_back(x.str());
    if (out.empty()) out.push_back("u8");  // an endpoint from before layouts: u8 only
    return out;
}

std::string Sender::codecFor(const Format& f) const {
#ifdef ILINK_FLAC
    std::lock_guard<std::mutex> l(m_);
    if (f.kind == "pcm" && f.channels == 2 && std::find(codecs_.begin(), codecs_.end(), "flac") != codecs_.end()) return "flac";
#else
    (void)f;
#endif
    return "pcm";
}

Sender::Wire Sender::wire() const { return {wireFlac_ ? "flac" : "pcm", wireSent_, wirePcm_}; }

bool Sender::start(uint32_t stream, const Format& asked, bool processed, const Track& t, double gainDb) {
    Format f = asked;
    {
        // SPEC §5.7: as FLAC when the host asked for it and libFLAC takes the format; else the samples as they are
        std::lock_guard<std::mutex> e(encMu_);
        enc_.reset();  // the stream before: its end() sent its last frame; flushed or stopped, what it held is dropped
        wireSent_ = wirePcm_ = 0;
        if (f.codec == "flac" && f.kind == "pcm" && f.channels == 2) {
            auto enc = std::make_unique<Enc>();
            enc->stream = stream, enc->rate = f.rate, enc->bits = f.bits;
            if (enc->open(*conn_, wireSent_)) enc_ = std::move(enc);
        }
        f.codec = enc_ ? "flac" : "pcm";
        wireFlac_ = enc_ != nullptr;
    }
    {
        std::lock_guard<std::mutex> l(m_);
        sent_[stream] = {0, f.frameRate(), f.bytesPerFrame(), f.frameGroup(), false};
        cur_ = stream;
        abort_ = false;
        while (sent_.size() > 8) sent_.erase(sent_.begin());
    }
    json::Value m = message("start");
    m.set("stream", json::num(stream));
    m.set("format", f.toJson());
    m.set("processed", json::boolean(processed));
    m.set("track", t.toJson());
    if (gainDb == gainDb) m.set("gain_db", json::real(gainDb));
    return control(m);
}

// audio sent but not played yet, across streams (SPEC §5.5)
double Sender::aheadMs() {
    std::lock_guard<std::mutex> l(m_);
    return aheadMsLocked();
}

double Sender::aheadMsLocked() const {
    // a status comes every 500 ms (SPEC §7): in between, while the endpoint plays, played moves on at the stream's rate —
    // else the sender stops at buffer_ms and bursts 500 ms at each status. At most 1 s on (an endpoint that stopped
    // reporting does not let it run away); counting from when the status arrived stays behind the real DAC, so the
    // endpoint never holds more than buffer_ms. Past the end of a stream that ended, the count goes on into the next one
    // from 0 when that one has the same format (gapless, SPEC §5.1) — a track change is no burst either; a new format
    // waits for the next status (the endpoint opens its DAC again).
    uint32_t heard = heardStream_;
    uint64_t played = played_;
    if (statusPlaying_)
        if (auto it = sent_.find(heard); it != sent_.end()) {
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - statusAt_).count();
            played += (uint64_t)(std::min(s, burstMs_ ? 2.0 : 1.0) * it->second.rate);  // a bursting endpoint reports each 1 s
            // ponytail: "the next one" = the next id in sent_ (ids grow; the wrap after 2^32 streams is not followed)
            for (auto nx = std::next(it); it->second.ended && played > it->second.frames && nx != sent_.end() &&
                                          nx->second.rate == it->second.rate && nx->second.bpf == it->second.bpf;
                 it = nx++) {
                played -= it->second.frames;
                heard = nx->first;
            }
        }
    double ms = 0;
    for (auto& [id, s] : sent_) {
        if (!s.rate) continue;
        if (haveStatus_ && id < heard && heard - id < 0x80000000u) continue;  // already heard
        uint64_t f = s.frames;
        if (haveStatus_ && id == heard) f = f > played ? f - played : 0;
        ms += 1000.0 * (double)f / s.rate;
    }
    return ms;
}

bool Sender::write(const uint8_t* data, size_t frames) {
    uint32_t stream;
    int rate, bpf;
    size_t group;
    {
        std::lock_guard<std::mutex> l(m_);
        auto it = sent_.find(cur_);
        if (it == sent_.end()) return false;
        stream = cur_;
        rate = it->second.rate;
        bpf = it->second.bpf;
        group = (size_t)it->second.group;
    }
    const size_t chunk = std::max(group, (size_t)rate / 25 / group * group);  // 40 ms, whole groups (DSD u32be)
    size_t done = 0;
    while (done < frames) {
        size_t n = std::min(chunk, frames - done);
        uint64_t first;
        {
            std::unique_lock<std::mutex> l(m_);
            // wait_for: what is ahead also shrinks with the clock (aheadMsLocked), not only when a status wakes it
            // burst_ms (SPEC §5.5): once full, nothing until that much has played — the radio rests in between
            auto room = [&] {
                if (gone_ || abort_) return true;
                const double ahead = aheadMsLocked();
                if (!filling_ && ahead <= bufferMs_ - burstMs_) filling_ = true;
                if (filling_ && ahead + 1000.0 * n / rate <= bufferMs_) return true;
                filling_ = burstMs_ == 0;
                return false;
            };
            while (!cv_.wait_for(l, std::chrono::milliseconds(20), room)) {}
            if (gone_ || abort_ || cur_ != stream) return false;
            first = sent_[stream].frames;
            sent_[stream].frames += n;
        }
        bool ok;
        writing_ = true;  // the pinger sends no ping behind it (one send lock: it would wait as long as this does)
        {
            std::lock_guard<std::mutex> e(encMu_);
            // SPEC §5.7: into the encoder — its frames go out as they fill up, each one audio chunk
            if (enc_ && enc_->stream == stream) ok = enc_->write(data + done * bpf, n);
            else if ((ok = conn_->sendAudio(stream, first, data + done * bpf, n * bpf))) wireSent_ += n * bpf;
        }
        writing_ = false;
        if (!ok) { gone_ = true; return false; }
        wirePcm_ += n * bpf;
        done += n;
    }
    return true;
}

bool Sender::end(uint32_t stream) {
    {
        std::lock_guard<std::mutex> e(encMu_);
        if (enc_ && enc_->stream == stream) {  // SPEC §5.7: its last, shorter frame first
            enc_->finish();
            enc_.reset();
        }
    }
    {
        std::lock_guard<std::mutex> l(m_);
        if (auto it = sent_.find(stream); it != sent_.end()) it->second.ended = true;  // its played may run on into the next
    }
    json::Value m = message("end");
    m.set("stream", json::num(stream));
    return control(m);
}

bool Sender::sync() {
    std::lock_guard<std::mutex> e(encMu_);
    if (!enc_ || enc_->in == enc_->first) return connected();
    // FLAC has no short frame but a stream's last one: this FLAC stream ends here and the next samples begin a new one
    // (SPEC §5.7 — the endpoint sees its fLaC and carries on in the same Link stream)
    if (enc_->finish() && enc_->open(*conn_, wireSent_)) return true;
    gone_ = true;
    return false;
}

bool Sender::flush() {
    {
        std::lock_guard<std::mutex> l(m_);
        sent_.clear();  // nothing sent is ahead any more
        filling_ = true;  // and the refill after a flush starts at once
        haveStatus_ = statusPlaying_ = false;
        abort_ = true;
    }
    cv_.notify_all();
    {
        std::lock_guard<std::mutex> e(encMu_);
        enc_.reset();
    }
    return control(message("flush"));
}
bool Sender::pause() {
    {
        std::lock_guard<std::mutex> l(m_);
        statusPlaying_ = false;  // it stops playing now, not at its next status
    }
    return control(message("pause"));
}
bool Sender::resume() { return control(message("resume")); }
bool Sender::stop() {
    {
        std::lock_guard<std::mutex> l(m_);
        sent_.clear();
        haveStatus_ = statusPlaying_ = false;
        abort_ = true;
    }
    cv_.notify_all();
    {
        std::lock_guard<std::mutex> e(encMu_);
        enc_.reset();
    }
    return control(message("stop"));
}
bool Sender::volume(int value) {
    json::Value m = message("volume");
    m.set("value", json::num(std::clamp(value, 0, 100)));
    return control(m);
}
bool Sender::mute(bool on) {
    json::Value m = message("volume");
    m.set("muted", json::boolean(on));
    return control(m);
}
bool Sender::art(uint32_t stream, const std::string& mime, const std::string& bytes) {
    if (bytes.empty()) return false;
    if (capabilities()["binary"].boolean())  // SPEC §3 0x03: the bytes as they are, up to the frame limit
        return connected() && conn_->sendData(kDataArt, stream, "", mime, 0, (int64_t)bytes.size(), bytes.data(), bytes.size());
    if (bytes.size() > 700 * 1024) return false;  // base64 grows it by a third: the frame stays < 1 MiB
    json::Value m = message("art");
    m.set("stream", json::num(stream));
    m.set("mime", json::str(mime));
    m.set("data", json::str(base64(bytes)));
    return control(m);
}

bool Sender::handoff(const json::Value& tracks, int index, long long positionMs) {
    json::Value m = message("handoff");
    m.set("tracks", tracks);
    m.set("index", json::num(index));
    m.set("position_ms", json::num(positionMs));
    return control(m);
}

bool Sender::takeBack() { return control(message("takeback")); }

bool Sender::nativeAttach() { return control(message("native_attach")); }

bool Sender::libraryLists(const std::string& gen) {
    json::Value m = message("library_lists");
    m.set("gen", json::str(gen));
    return control(m);
}

bool Sender::libraryRead(const std::string& id, int64_t offset, int length) {
    json::Value m = message("library_read");
    m.set("id", json::str(id));
    m.set("offset", json::num(offset));
    m.set("length", json::num(length));
    return control(m);
}

bool Sender::libraryLove(const std::string& id, bool on) {
    json::Value m = message("library_love");
    m.set("id", json::str(id));
    m.set("on", json::boolean(on));
    return control(m);
}

bool Sender::libraryPlays(const std::string& since, const json::Value& add) {
    json::Value m = message("library_plays");
    m.set("since", json::str(since));
    m.set("add", add);
    return control(m);
}

bool Sender::libraryArt(const std::string& id) {
    json::Value m = message("library_art");
    m.set("id", json::str(id));
    return control(m);
}

bool Sender::library(long long from, int count, const std::string& gen) {
    json::Value m = message("library");
    m.set("from", json::num(from));
    m.set("count", json::num(count));
    m.set("gen", json::str(gen));
    return control(m);
}

bool Sender::native(const json::Value& queue, int index, long long positionMs, bool keep) {
    json::Value m = message("native");
    if (keep) m.set("keep", json::boolean(true));
    m.set("queue", queue);
    m.set("index", json::num(index));
    m.set("position_ms", json::num(positionMs));
    return control(m);
}

bool Sender::nativeControl(const std::string& action, long long ms) {
    json::Value m = message("native_control");
    m.set("action", json::str(action));
    if (ms >= 0) m.set("ms", json::num(ms));
    return control(m);
}

bool Sender::meta(uint32_t stream, const Track& t) {
    json::Value m = message("meta");
    m.set("stream", json::num(stream));
    m.set("track", t.toJson());
    return control(m);
}
void Sender::abort() {
    {
        std::lock_guard<std::mutex> l(m_);
        abort_ = true;
    }
    cv_.notify_all();
}

void Sender::reader() {
    Frame f;
    while (!gone_ && conn_->read(f)) {
        if (f.type == kData) {  // a library answer's bytes (SPEC §3 0x03): onData, then the answer as JSON without them
            if (f.kind != kDataLibraryRead && f.kind != kDataLibraryArt) continue;
            json::Value m = message(f.kind == kDataLibraryRead ? "library_read" : "library_art");
            m.set("id", json::str(f.id));
            if (f.kind == kDataLibraryRead) m.set("offset", json::num(f.offset));
            else if (f.size >= 0) m.set("mime", json::str(f.mime));
            m.set("size", json::num(f.size));
            m.set("binary", json::boolean(true));
            if (onData) onData(f);
            if (onMessage) onMessage(m);
            continue;
        }
        if (f.type != kMessage) continue;
        const std::string t = f.msg["t"].str();
        if (t == "status") {
            std::lock_guard<std::mutex> l(m_);
            const uint32_t stream = (uint32_t)f.msg["stream"].i64();
            const uint64_t played = (uint64_t)f.msg["played"].i64();
            // counted on only while it really plays: "playing", something heard, further than the last status (a DAC
            // that stalls keeps saying playing)
            statusPlaying_ = f.msg["state"].str() == "playing" && played > 0 && !(haveStatus_ && stream == heardStream_ && played <= played_);
            statusAt_ = std::chrono::steady_clock::now();
            heardStream_ = stream;
            played_ = played;
            haveStatus_ = true;
            dry_ = f.msg["state"].str() == "playing" && f.msg.has("buffered_ms") && f.msg["buffered_ms"].i64() < kDryMs;
            cv_.notify_all();
        } else if (t == "ended") {
            std::lock_guard<std::mutex> l(m_);
            sent_.erase((uint32_t)f.msg["stream"].i64());
            cv_.notify_all();
        } else if (t == "ping") {
            conn_->send(message("pong"));
        } else if (t == "native_sources") {  // version 1: signed in / out, its library loaded
            std::lock_guard<std::mutex> l(m_);
            native_.clear();
            for (auto& x : f.msg["native"].arr) native_.push_back(x.str());
            caps_.set("native", f.msg["native"]);
        } else if (t == "capabilities") {  // SPEC §4.1: the whole object, always the latest
            std::lock_guard<std::mutex> l(m_);
            caps_ = json::Value();
            for (auto& kv : f.msg.obj) if (kv.first != "t") caps_.set(kv.first, kv.second);
            native_.clear();
            for (auto& x : caps_["native"].arr) native_.push_back(x.str());
        } else if (t == "formats") {  // SPEC §4.1: what it offers now (its quality cap moved) — for the next stream
            std::lock_guard<std::mutex> l(m_);
            if (f.msg["formats"]["pcm"].has("rates")) formats_ = f.msg["formats"];
            if (f.msg.has("codecs")) {  // SPEC §5.7: its lossless compression was switched on or off
                codecs_.clear();
                for (auto& x : f.msg["codecs"].arr) codecs_.push_back(x.str());
            }
        } else if (t == "result") {  // SPEC §8.1: a command's answer, for the one waiting
            std::lock_guard<std::mutex> l(m_);
            results_[f.msg["id"].i64()] = f.msg;
            while (results_.size() > 64) results_.erase(results_.begin());  // nobody waited for those
            cv_.notify_all();
        }
        if (onMessage) onMessage(f.msg);
        if (t == "stopped") break;
    }
    gone_ = true;
    cv_.notify_all();
}

void Sender::pinger() {
    // SPEC §4.2: a ping only after 10 s of sending nothing — audio, a command or a ping all count. An idle connection
    // (remote, library, control) costs a ping every 10 s instead of every 5 s; a stream that sends, none
    using namespace std::chrono;
    while (!gone_) {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait_for(l, seconds(1), [&] { return gone_.load(); });
        if (gone_) break;
        l.unlock();
        const long long now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        // sent, not arriving (SPEC §5.5): the endpoint says it plays, holds next to nothing and hears next to nothing new
        // while seconds of audio are counted ahead of it. The connection stands — its reports still come — and carries
        // nothing our way: a tunnel's TCP stood for 50 s with 15 s of music inside it (the phone app on mobile data,
        // 2026-11-19). Closed, so the sender starts again on a new one now, not when the old one lets go. A slow network
        // is not this: there the endpoint goes on hearing (a quarter of real time or more starts the count again).
        {
            std::lock_guard<std::mutex> g(m_);
            const auto it = sent_.find(heardStream_);
            const bool dry = dry_ && it != sent_.end() && aheadMsLocked() > kStuckAheadMs;
            if (!dry || !stuckAt_ || heardStream_ != stuckStream_ || played_ > stuckPlayed_ + (uint64_t)it->second.rate)
                stuckAt_ = dry ? now : 0, stuckStream_ = heardStream_, stuckPlayed_ = played_;
            else if (now - stuckAt_ >= kStuckMs) stuck_ = true;
        }
        if (stuck_) { conn_->shutdown(); break; }
        // … checked before the ping, and no ping behind an audio write that waits (one send lock): the socket is full then,
        // the ping would wait with it and this thread never came back to either rule
        if (!writing_ && now - conn_->sentAt() >= 10000) conn_->send(message("ping"));
        // nothing from it for 20 s — no status while it plays (every 0.5 s), no pong to our ping (every 10 s while idle): it
        // is gone (powered off, out of range, asleep) without closing. Closed, so a sender notices in seconds, not when the
        // system gives up on the socket minutes later (SPEC §4.2 allows 15 s; Infinity Core's gap audit 2026-11-13)
        if (conn_->readAt() && now - conn_->readAt() > 20000) { conn_->shutdown(); break; }
    }
}

}  // namespace ilink
