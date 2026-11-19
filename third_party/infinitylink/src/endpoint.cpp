#include "infinitylink/endpoint.h"

#include <string.h>

#include "flac.h"
#include "net.h"

#include <algorithm>
#include <chrono>

namespace ilink {

json::Value Endpoint::capabilitiesLocked() const {  // SPEC §4.1
    json::Value c, nat;
    nat.type = json::Value::Arr;
    bool lib = false;
    for (auto& x : info_.native) { nat.push(json::str(x)); lib = lib || x == "library"; }
    c.set("native", nat);
    c.set("library", json::boolean(lib && onLibrary != nullptr));
    c.set("library_sync", json::boolean(lib && onLibraryLove && onLibraryPlays));  // SPEC §8.3 library_love / library_plays
    c.set("takeback", json::boolean(onTakeBack != nullptr));
    c.set("handoff", json::boolean(onHandoff != nullptr && onCommand != nullptr));  // §8.1 command handoff (the host's onCommand)
    c.set("dsp", json::boolean(onDsp != nullptr));  // SPEC §8.4
    c.set("transfer", json::boolean(onTransfer != nullptr));  // SPEC §8.7
    c.set("gain", json::boolean(info_.gain));  // SPEC §5.1 gain_db
    c.set("binary", json::boolean(true));      // SPEC §3 0x03: it reads the sender's art as a data frame
    // who plays on it now (a stream or remote sender's name, "" = free): a sender that was taken over comes back once free
    c.set("in_use", json::str(conn_ ? senderName_ : remoteConn_ ? remoteName_ : std::string()));
    return c;
}

static json::Value codecsWith(bool flac) {  // SPEC §4.1 codecs
    json::Value c;
    c.push(json::str("pcm"));
    if (flac) c.push(json::str("flac"));
    return c;
}

bool Endpoint::flacForLocked(const Conn& c) const {
#ifdef ILINK_FLAC
    return (compress_ == kCompressAll || (compress_ == kCompressForwarded && c.peer == "127.0.0.1")) &&
           (noFlac_.empty() || c.senderId != noFlac_);
#else
    (void)c;
    return false;
#endif
}

bool Endpoint::awayLocked(const Conn& c) const { return info_.awayBufferMs > 0 && c.peer == "127.0.0.1"; }

void Endpoint::compress(int whom) {  // SPEC §5.7
    std::shared_ptr<Conn> c;
    json::Value m = message("formats");
    {
        std::lock_guard<std::mutex> l(m_);
        if (compress_ == whom) return;
        compress_ = whom;
        c = conn_;
        if (!c || c->proto < 2) return;  // version 1 has no `formats` message: its next connection hears it
        m.set("formats", formatsLocked());
        m.set("codecs", codecsWith(flacForLocked(*c)));
    }
    c->send(m);
}

json::Value Endpoint::welcomeLocked(int proto, bool burst, bool flac, bool away) const {
    json::Value w = info_.welcome();
    if (flac) w.set("codecs", codecsWith(true));
    const int buf = away ? info_.awayBufferMs : info_.bufferMs, bst = away ? info_.awayBurstMs : info_.burstMs;
    if (away) w.set("buffer_ms", json::num(buf)), w.set("burst_ms", json::num(bst));  // through a forwarder: its own depth
    // a sender that does not refill in bursts (an older one) gets the usual 2 s: it would keep the whole deep buffer
    // topped up, and its sound changes would take that long to be heard (SPEC §5.5)
    if (!burst && bst > 0) w.set("buffer_ms", json::num(std::min(buf, 2000))), w.set("burst_ms", json::num(0));
    w.set("proto", json::num(proto));
    w.set("formats", formatsLocked());
    if (proto >= 2) w.set("capabilities", capabilitiesLocked());
    return w;
}

json::Value Endpoint::formatsLocked() const {
    // keep what is at or under the cap; none left = the lowest (it still plays something)
    auto under = [](const std::vector<int>& v, int cap) {
        if (cap <= 0 || v.empty()) return v;
        std::vector<int> out;
        for (int x : v) if (x <= cap) out.push_back(x);
        if (out.empty()) out.push_back(*std::min_element(v.begin(), v.end()));
        return out;
    };
    EndpointInfo i;  // only its formats are read
    i.pcmRates = under(info_.pcmRates, limitRate_);
    i.pcmBits = under(info_.pcmBits, limitBits_);
    if (limitDsd_) i.dsdRates = info_.dsdRates;
    i.dsdLayouts = info_.dsdLayouts;
    return i.welcome()["formats"];
}

void Endpoint::limit(int maxRate, int maxBits, bool dsd) {  // SPEC §4.1 formats change
    std::vector<std::shared_ptr<Conn>> to;
    json::Value m = message("formats");
    {
        std::lock_guard<std::mutex> l(m_);
        if (limitRate_ == maxRate && limitBits_ == maxBits && limitDsd_ == dsd) return;
        limitRate_ = maxRate, limitBits_ = maxBits, limitDsd_ = dsd;
        m.set("formats", formatsLocked());
        for (auto& c : {conn_, remoteConn_}) if (c) to.push_back(c);
        for (auto& c : libConns_) to.push_back(c);
        for (auto& c : ctlConns_) to.push_back(c);
    }
    for (auto& c : to) if (c->proto >= 2) c->send(m);  // version 1 has no such message: its formats stay the welcome's
}

void Endpoint::capabilities() {
    std::vector<std::shared_ptr<Conn>> to;
    json::Value caps, v1 = message("native_sources"), nat;
    {
        std::lock_guard<std::mutex> l(m_);
        for (auto& c : {conn_, remoteConn_}) if (c) to.push_back(c);
        for (auto& c : libConns_) to.push_back(c);
        for (auto& c : ctlConns_) to.push_back(c);
        caps = capabilitiesLocked();
    }
    json::Value m = message("capabilities");
    for (auto& kv : caps.obj) m.set(kv.first, kv.second);
    v1.set("native", caps["native"]);
    for (auto& c : to) c->send(c->proto >= 2 ? m : v1);
}

void Endpoint::playerState(const json::Value& p) {
    json::Value m = message("player");
    std::shared_ptr<Conn> r;
    {
        std::lock_guard<std::mutex> l(m_);
        lastPlayer_ = json::Value();
        for (auto& kv : p.obj) {
            if (kv.first == "queue") lastQueue_ = kv.second;
            else lastPlayer_.set(kv.first, kv.second);
            m.set(kv.first, kv.second);
        }
        r = remoteConn_;
    }
    if (r) r->send(m);
}

void Endpoint::dspState(const json::Value& d) {
    json::Value m = message("dsp");
    for (auto& kv : d.obj) m.set(kv.first, kv.second);
    std::vector<std::shared_ptr<Conn>> to;
    {
        std::lock_guard<std::mutex> l(m_);
        lastDsp_ = d;
        to = ctlConns_;
        if (remoteConn_) to.push_back(remoteConn_);
    }
    for (auto& c : to) c->send(m);
}

int Endpoint::watchers() {
    std::lock_guard<std::mutex> l(m_);
    return (int)ctlConns_.size() + (remoteConn_ ? 1 : 0);
}

void Endpoint::statusExtra(const json::Value& extra) {
    std::lock_guard<std::mutex> l(m_);
    statusExtra_ = extra;
}

json::Value Endpoint::commandResult(const json::Value& cmd, bool dspOnly) {
    json::Value r = message("result"), a;
    r.set("id", cmd["id"]);
    const bool dsp = cmd["action"].str().rfind("dsp_", 0) == 0;
    if (cmd["action"].str() == "transfer" && onTransfer) a = onTransfer(cmd);  // SPEC §8.7: control or remote
    else if (dsp && onDsp) a = onDsp(cmd);
    else if (!dsp && !dspOnly && onCommand) a = onCommand(cmd);
    else a.set("ok", json::boolean(false)), a.set("error", json::str("unsupported"));
    for (auto& kv : a.obj) r.set(kv.first, kv.second);
    if (!r.has("ok")) r.set("ok", json::boolean(false));
    return r;
}

json::Value EndpointInfo::welcome() const {
    auto ints = [](const std::vector<int>& v) {
        json::Value a;
        a.type = json::Value::Arr;
        for (int x : v) a.push(json::num(x));
        return a;
    };
    json::Value w = message("welcome"), ep, formats, pcm, vol, codecs;
    w.set("proto", json::num(kProto));
    ep.set("id", json::str(id));
    ep.set("name", json::str(name));
    ep.set("model", json::str(model));
    ep.set("vendor", json::str(vendor));
    w.set("endpoint", ep);
    pcm.set("rates", ints(pcmRates));
    pcm.set("bits", ints(pcmBits));
    pcm.set("channels", ints({2}));
    formats.set("pcm", pcm);
    if (!dsdRates.empty()) {
        json::Value dsd;
        dsd.set("rates", ints(dsdRates));
        json::Value lay;
        for (auto& x : dsdLayouts) lay.push(json::str(x));
        dsd.set("layouts", lay);  // SPEC §5.3
        formats.set("dsd", dsd);
    }
    w.set("formats", formats);
    codecs.push(json::str("pcm"));
    w.set("codecs", codecs);
    vol.set("kind", json::str(volumeKind));
    vol.set("value", json::num(volume));
    vol.set("muted", json::boolean(muted));
    if (volumeSteps > 0) vol.set("steps", json::num(volumeSteps));
    w.set("volume", vol);
    w.set("buffer_ms", json::num(bufferMs));
    if (burstMs > 0) w.set("burst_ms", json::num(burstMs));
    if (!dsp.empty()) w.set("dsp", json::str(dsp));
    if (handoff) w.set("handoff", json::boolean(true));
    if (!native.empty()) {
        json::Value n;
        n.type = json::Value::Arr;
        for (auto& x : native) n.push(json::str(x));
        w.set("native", n);
    }
    return w;
}

Endpoint::Endpoint(EndpointInfo info, Sink& sink) : info_(std::move(info)), sink_(sink) {
    spare_.reserve(kSpare);
    double bps = 0;  // the fastest format it offers, bytes a second (2 channels)
    for (int r : info_.pcmRates)
        for (int b : info_.pcmBits) bps = std::max(bps, 2.0 * r * (b / 8));
    for (int r : info_.dsdRates) bps = std::max(bps, 2.0 * r / 8);
    // … of the deepest buffer it gives (awayBufferMs: 20 s on a phone), and never more than 160 MB: reading stops at half of
    // it, so a 768 kHz stream is held to what fits (~13 s) instead of 20 s of it in memory (270 MB)
    maxQueued_ = std::min((size_t)(2 * bps * (std::max({info_.bufferMs, info_.burstMs, info_.awayBufferMs}) + 2000) / 1000) + (16u << 20), (size_t)160 << 20);
    player_ = std::thread([this] { player(); });
    reporter_ = std::thread([this] { reporter(); });
}

Endpoint::~Endpoint() {
    stop();
    if (player_.joinable()) player_.join();
    if (reporter_.joinable()) reporter_.join();
    std::vector<Session> s;
    {
        std::lock_guard<std::mutex> l(m_);
        s.swap(sessions_);
    }
    for (auto& x : s) if (x.t.joinable()) x.t.join();
}

bool Endpoint::listen(int port, std::string& err) {
    net::init();
    lfd_ = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (lfd_ < 0) { err = net::error(); return false; }
    net::setInt(lfd_, SOL_SOCKET, SO_REUSEADDR, 1);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (!info_.address.empty() && inet_pton(AF_INET, info_.address.c_str(), &a.sin_addr) != 1) {
        err = "bad address " + info_.address;
        net::closeSocket(lfd_);
        lfd_ = -1;
        return false;
    }
    a.sin_port = htons((uint16_t)port);
    if (bind(lfd_, (sockaddr*)&a, sizeof a) != 0 || ::listen(lfd_, 4) != 0) {
        err = net::error();
        net::closeSocket(lfd_);
        lfd_ = -1;
        return false;
    }
    return true;
}

int Endpoint::port() const {
    sockaddr_in a{};
    socklen_t n = sizeof a;
    return lfd_ >= 0 && getsockname(lfd_, (sockaddr*)&a, &n) == 0 ? ntohs(a.sin_port) : 0;
}

void Endpoint::run() {
    while (!quit_ && lfd_ >= 0) {
        sockaddr_storage a{};
        socklen_t n = sizeof a;
        int fd = (int)accept(lfd_, (sockaddr*)&a, &n);
        if (fd < 0) {
            if (quit_) break;
            continue;
        }
        char ip[INET6_ADDRSTRLEN] = "?";
        const bool v6 = a.ss_family == AF_INET6;
        inet_ntop(a.ss_family, v6 ? (void*)&((sockaddr_in6*)&a)->sin6_addr : (void*)&((sockaddr_in*)&a)->sin_addr, ip, sizeof ip);
        if (!info_.anyAddress && !net::privateAddress((sockaddr*)&a)) {  // SPEC §9: private networks only
            note(std::string("refused ") + ip + ": not a private network address");
            net::closeSocket(fd);
            continue;
        }
        net::setInt(fd, IPPROTO_TCP, TCP_NODELAY, 1);
        net::noSigpipe(fd);
        auto c = std::make_shared<Conn>(fd);
        c->peer = ip;
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::vector<std::thread> ended;  // the sessions that returned: joined here, outside the lock (stop() runs from a signal)
        {
            std::lock_guard<std::mutex> l(m_);
            for (auto it = sessions_.begin(); it != sessions_.end();)
                if (*it->done) ended.push_back(std::move(it->t)), it = sessions_.erase(it);
                else ++it;
            sessions_.push_back({std::thread([this, c, done] { session(c); *done = true; }), done, c});
        }
        for (auto& t : ended) t.join();
    }
}

void Endpoint::stop() {
    quit_ = true;
    if (lfd_ >= 0) { ::shutdown(lfd_, SHUT_RDWR); net::closeSocket(lfd_); lfd_ = -1; }
    std::lock_guard<std::mutex> l(m_);
    // every connection — the sender's, library, remote and control ones, one that has not said hello yet: else the
    // destructor's join waits for a sender that keeps its connection (a Core keeps its library one while the DAP is there)
    for (auto& s : sessions_)
        if (auto c = s.conn.lock()) c->shutdown();
    cv_.notify_all();
}

size_t Endpoint::Queue::cost(const Item& it) {
    const Track& t = it.track;
    return sizeof(Item) + it.bytes.size() + t.title.size() + t.artist.size() + t.album.size() + t.art.size() + t.codec.size();
}
void Endpoint::Queue::push(Item&& it) {
    bytes += cost(it);
    if (n == v.size()) {  // full: twice the room, the queue in order from 0 (rare — once to the deepest it gets)
        std::vector<Item> w(std::max<size_t>(16, 2 * v.size()));
        for (size_t i = 0; i < n; i++) w[i] = std::move(v[(head + i) % v.size()]);
        v.swap(w);
        head = 0;
    }
    v[(head + n++) % v.size()] = std::move(it);
}
Endpoint::Item Endpoint::Queue::pop() {
    bytes -= cost(v[head]);
    Item it = std::move(v[head]);
    head = (head + 1) % v.size();
    n--;
    return it;
}
void Endpoint::Queue::clear() {
    for (size_t i = 0; i < n; i++) v[(head + i) % v.size()] = Item();  // their buffers go now, not when overwritten
    head = n = bytes = 0;
}

// a cover is for the track it came with (SPEC §5.6): another album, or a track with its own cover URL, drops it — else a
// local file's bytes stayed over a streamed track that has only a URL (2026-11-12, DAP storage → TIDAL showed the old
// cover). The same album keeps it until its own comes (no blank between its tracks). Asked by meta (which comes first when
// a new song is picked — a sender may still name the old stream in it) and by a start reaching the sink; the start of the
// cover's own stream says which track it is the cover of (a sender may send the cover before that start)
void Endpoint::tookArt(uint32_t stream, bool same) {
    artStream_ = stream, artAlbum_ = track_.album, artUrl_ = track_.art;
    if (!same) artGen_++;  // the same picture again (a sender's new stream after a seek): no new cover for the host to load
}
void Endpoint::dropOtherArt(uint32_t stream, const Track& t, bool started) {
    if (artData_.empty()) return;
    if (started && stream == artStream_) artAlbum_ = t.album, artUrl_ = t.art;
    else if (t.album != artAlbum_ || t.art != artUrl_) artData_.clear(), artGen_++;
}

void Endpoint::dropQueue() {
    queue_.clear();
    gen_++;
    cv_.notify_all();
}

double Endpoint::queuedMsLocked() const {
    double ms = 0;
    for (size_t i = 0; i < queue_.size(); i++)
        if (const Item& it = queue_[i]; it.kind == Item::Audio && open_.rate > 0) ms += 1000.0 * (double)(it.bytes.size() / std::max(1, open_.bytesPerFrame())) / open_.frameRate();
    return ms;
}

void Endpoint::say(const json::Value& msg) {
    std::shared_ptr<Conn> c;
    {
        std::lock_guard<std::mutex> l(m_);
        c = conn_;
    }
    if (c) c->send(msg);
}

void Endpoint::localVolume(int value, bool muted) {
    {
        std::lock_guard<std::mutex> l(m_);
        info_.volume = value;
        info_.muted = muted;
    }
    json::Value v = message("volume");
    v.set("value", json::num(value));
    v.set("muted", json::boolean(muted));
    say(v);
}

void Endpoint::nativeStatus(const std::string& state, int index, long long positionMs, long long durationMs, int count,
                            const json::Value& extra) {
    json::Value s = message("native_status");
    for (auto& kv : extra.obj) s.set(kv.first, kv.second);
    if (count >= 0) s.set("count", json::num(count));
    s.set("state", json::str(state));
    s.set("index", json::num(index));
    s.set("position_ms", json::num(positionMs));
    s.set("duration_ms", json::num(durationMs));
    say(s);
}

void Endpoint::nativeSources(const std::vector<std::string>& sources) {
    {
        std::lock_guard<std::mutex> l(m_);
        if (info_.native == sources) return;
        info_.native = sources;
    }
    capabilities();  // every connection learns it now (SPEC §4.1) — not only at its next welcome
}

std::string Endpoint::art(std::string& mime) {
    std::lock_guard<std::mutex> l(m_);
    mime = artMime_;
    return artData_;
}

void Endpoint::network(const std::string& type, int signalDbm) {
    std::lock_guard<std::mutex> l(m_);
    netByHost_ = true;
    netType_ = type;
    netDbm_ = signalDbm;
}

// Linux: the first wireless interface's signal ("wlan0: 0000   54.  -56.  -256 …": level in dBm); "" = none
static bool linuxWifi(int& dbm) {
    FILE* f = fopen("/proc/net/wireless", "r");
    if (!f) return false;
    char line[256];
    bool found = false;
    for (int i = 0; fgets(line, sizeof line, f); i++) {
        if (i < 2) continue;  // two header lines
        char name[32];
        unsigned status;
        float link, level;
        if (sscanf(line, " %31[^:]: %x %f %f", name, &status, &link, &level) == 4) {
            dbm = (int)level;
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

void Endpoint::release() {
    json::Value r = message("released");
    r.set("reason", json::str("local"));
    say(r);
    std::lock_guard<std::mutex> l(m_);
    dropQueue();
    stopAsked_ = true;
}

void Endpoint::remote(const std::string& action, long long ms) {
    json::Value r = message("remote");
    r.set("action", json::str(action));
    if (ms >= 0) r.set("ms", json::num(ms));
    say(r);
}

Endpoint::Now Endpoint::now() {
    std::lock_guard<std::mutex> l(m_);
    Now n;
    n.connected = conn_ != nullptr;
    n.sender = senderName_;
    n.remote = remoteConn_ ? remoteName_ : std::string();
    n.senderId = conn_ ? conn_->senderId : std::string();
    n.state = state_;
    n.track = track_;
    n.processed = processed_;
    n.gainDb = gainDb_;
    n.format = open_;
    n.volume = info_.volume;
    n.muted = info_.muted;
    n.artGen = artGen_;
    n.senderAddr = conn_ ? conn_->peer : std::string();
    n.received = received_;
    n.codec = state_ == "idle" ? "pcm" : codec_;
    if (sinkOpen_ && !heard_.empty()) {
        uint64_t d = std::min<uint64_t>(sink_.delay(), written_), at = written_ - d;
        n.bufferedMs = (int)(queuedMsLocked() + (open_.rate ? 1000.0 * d / open_.frameRate() : 0));
        n.underruns = sink_.underruns();
        const Heard* h = &heard_.front();
        for (auto& x : heard_) if (x.from <= at) h = &x;
        // 0 until the DAC reaches the stream's first frame (after a flush only that stream is known, and what the sink holds
        // is not played yet): unsigned, the difference was 2^64 − a few — the phone's lock screen got a place of 384307168202262 ms
        n.played = at > h->from ? at - h->from : 0;
        // the track above is the newest stream's from the moment it reaches the sink — a few hundred ms before the DAC
        // plays its first frame. Until then the frames heard are still the stream's before it: counted as this track's
        // place, a new song opened at the old one's full length (the phone's lock screen: 0:24 into a song not yet heard,
        // and its bar never came back, 2026-11-19). 0 for that moment
        if (h->stream != trackStream_ && heard_.back().stream == trackStream_) n.played = 0;
        if (h->to >= 0) n.played = std::min<uint64_t>(n.played, (uint64_t)(h->to - (int64_t)h->from));
    }
    return n;
}

void Endpoint::endSession(const std::shared_ptr<Conn>& c) {
    bool left = false;
    {
        std::lock_guard<std::mutex> l(m_);
        if (conn_ == c) {  // the sender left: its music stops with it
            conn_.reset();
            senderName_.clear();
            track_ = Track();
            gainDb_ = 0.0 / 0.0;
            dropQueue();
            spare_.clear();  // a hi-res sender's buffers do not stay held while nobody plays
            stopAsked_ = true;
            left = true;
        }
    }
    if (left) capabilities();  // in_use changed: a sender that was taken over may come back
}

// SPEC §8.1: the library's messages — on the sender's connection or on a library-only one (role "library")
bool Endpoint::serveLibrary(const std::shared_ptr<Conn>& c, const std::string& t, const json::Value& m) {
    if (t == "library_love" || t == "library_plays") {  // favourites / plays kept alike with the sender (SPEC §8.3)
        auto& f = t == "library_love" ? onLibraryLove : onLibraryPlays;
        json::Value a = f ? f(m) : json::Value();
        json::Value r = message(t.c_str());
        if (t == "library_love") r.set("id", m["id"]);
        if (a.isNull()) { r.set("ok", json::boolean(false)); r.set("error", json::str("not answered")); }
        for (auto& kv : a.obj) r.set(kv.first, kv.second);
        c->send(r);
        return true;
    }
    if (t == "library_lists") {  // its favourites and playlists
        json::Value a = onLibraryLists ? onLibraryLists(m) : json::Value();
        json::Value r = message("library_lists");
        for (auto& kv : a.obj) r.set(kv.first, kv.second);
        c->send(r);
        return true;
    }
    if (t == "library_read") {  // a piece of one of its files
        const std::string id = m["id"].str();
        const int64_t off = m["offset"].i64();
        const int len = (int)std::min<int64_t>(std::max<int64_t>(m["length"].i64(), 0), 512 * 1024);
        int64_t size = -1;
        std::string bytes = onLibraryRead && off >= 0 ? onLibraryRead(id, off, len, size) : "";
        if (c->binary) {  // SPEC §3 0x03: the bytes as they are
            c->sendData(kDataLibraryRead, 0, id, "", off, size, bytes.data(), bytes.size());
            return true;
        }
        json::Value r = message("library_read");
        r.set("id", json::str(id));
        r.set("offset", json::num(off));
        r.set("size", json::num(size));
        r.set("data", json::str(base64(bytes)));
        c->send(r);
        return true;
    }
    if (t == "library_art") {  // a cover of its library
        std::string mime;
        const std::string id = m["id"].str();
        std::string bytes = onLibraryArt ? onLibraryArt(id, mime) : "";
        if (c->binary) {  // SPEC §3 0x03; an image too big for one frame = none
            if (mime.empty()) mime = "image/jpeg";
            if (bytes.empty() || 1 + kDataHead + id.size() + mime.size() + bytes.size() > kMaxFrame) bytes.clear(), mime.clear();
            c->sendData(kDataLibraryArt, 0, id, mime, 0, bytes.empty() ? -1 : (int64_t)bytes.size(), bytes.data(), bytes.size());
            return true;
        }
        json::Value r = message("library_art");
        r.set("id", json::str(id));
        if (!bytes.empty() && bytes.size() < 700 * 1024) {
            r.set("mime", json::str(mime.empty() ? "image/jpeg" : mime));
            r.set("data", json::str(base64(bytes)));
        }
        c->send(r);
        return true;
    }
    if (t == "library") {  // its own files, a page at a time
        json::Value a = onLibrary ? onLibrary(m) : json::Value();
        json::Value r = message("library");
        for (auto& kv : a.obj) r.set(kv.first, kv.second);
        if (!a.has("gen")) r.set("total", json::num(0));
        c->send(r);
        return true;
    }
    return false;
}

void Endpoint::session(std::shared_ptr<Conn> c) {
    net::nameThread("link-session");  // until its hello says which kind it is
    Frame f;
    if (!c->read(f) || f.type != kMessage || f.msg["t"].str() != "hello") { c->close(); return; }
    const long long want = f.msg["proto"].i64();
    if (want < kProto) {
        json::Value e = message("error");
        e.set("code", json::str("proto"));
        e.set("message", json::str("this endpoint speaks Infinity Link " + std::to_string(kProto) + " to " + std::to_string(kProtoMax)));
        c->send(e);
        c->close();
        return;
    }
    c->proto = (int)std::min<long long>(want, kProtoMax);  // SPEC §4.1: the highest both speak
    c->role = f.msg["role"].str("stream");
    c->senderId = f.msg["sender"]["id"].str();
    c->binary = f.msg["binary"].boolean();  // SPEC §3 0x03: library answers as data frames
    c->burst = f.msg["burst"].boolean();    // SPEC §5.5 burst_ms
    const std::string by = f.msg["sender"]["name"].str();
    auto kick = [&by](const std::shared_ptr<Conn>& o) {  // SPEC §1: taken over — and by whom (§7)
        json::Value s = message("stopped");
        s.set("reason", json::str("taken"));
        if (!by.empty()) s.set("by", json::str(by));
        o->send(s);
        o->shutdown();
    };
    if (c->proto >= 2 && c->role == "control") {  // SPEC §8.4: its DSP — any number of them, never taking it over
        json::Value dspNow;
        {
            std::lock_guard<std::mutex> l(m_);
            ctlConns_.push_back(c);
            // the welcome goes under m_: every other thread takes m_ to find this connection, so nothing can reach it first
            // (capabilities() from a connection just taken over did, about 1 in 8). ponytail: a write under m_, one small frame on a new socket
            c->send(welcomeLocked(c->proto, c->burst));
            if (!lastDsp_.isNull()) {
                dspNow = message("dsp");
                for (auto& kv : lastDsp_.obj) dspNow.set(kv.first, kv.second);
            }
        }
        if (!dspNow.isNull()) c->send(dspNow);
        net::nameThread("link-control");
        note("control: " + f.msg["sender"]["name"].str());
        while (!quit_ && c->read(f)) {
            if (f.type != kMessage) continue;
            const std::string t = f.msg["t"].str();
            if (t == "ping") c->send(message("pong"));
            else if (t == "command") c->send(commandResult(f.msg, true));
        }
        std::lock_guard<std::mutex> l(m_);
        ctlConns_.erase(std::remove(ctlConns_.begin(), ctlConns_.end(), c), ctlConns_.end());
        return;
    }
    if (c->proto >= 2 && c->role == "remote") {  // SPEC §8: the endpoint's own player, commands + its state
        std::shared_ptr<Conn> prev, other;
        json::Value last, dspNow;
        {
            std::lock_guard<std::mutex> l(m_);
            prev = remoteConn_;
            remoteConn_ = c;
            if (conn_ && conn_->senderId != c->senderId) other = conn_;  // another sender's stream: taken over too
            c->send(welcomeLocked(c->proto, c->burst));
            if (!lastPlayer_.isNull()) {
                last = message("player");
                for (auto& kv : lastPlayer_.obj) last.set(kv.first, kv.second);
                if (!lastQueue_.isNull()) last.set("queue", lastQueue_);  // what plays already — even another app's
            }
            if (!lastDsp_.isNull()) {
                dspNow = message("dsp");
                for (auto& kv : lastDsp_.obj) dspNow.set(kv.first, kv.second);
            }
        }
        if (prev && prev != c) kick(prev);
        if (other) kick(other);
        if (!last.isNull()) c->send(last);
        if (!dspNow.isNull()) c->send(dspNow);
        {
            std::lock_guard<std::mutex> l(m_);
            remoteName_ = f.msg["sender"]["name"].str();
        }
        capabilities();  // in_use changed
        net::nameThread("link-remote");
        note("remote: " + f.msg["sender"]["name"].str());
        while (!quit_ && c->read(f)) {
            if (f.type != kMessage) continue;
            const std::string t = f.msg["t"].str();
            if (t == "ping") { c->send(message("pong")); continue; }
            if (t == "command") c->send(commandResult(f.msg, false));
        }
        {
            std::lock_guard<std::mutex> l(m_);
            if (remoteConn_ == c) remoteConn_.reset(), remoteName_.clear();  // its player plays on (SPEC §8.2)
        }
        capabilities();  // in_use changed
        return;
    }
    if (c->role == "library") {  // SPEC §8.3: browse / read its library beside whoever plays — never takes over
        std::vector<std::shared_ptr<Conn>> gone;
        {
            std::lock_guard<std::mutex> l(m_);
            for (auto it = libConns_.begin(); it != libConns_.end();)  // one per sender: its older one goes
                if ((*it)->senderId == c->senderId) gone.push_back(*it), it = libConns_.erase(it);
                else ++it;
            if (libConns_.size() >= kMaxLibrary) gone.push_back(libConns_.front()), libConns_.erase(libConns_.begin());
            libConns_.push_back(c);
            c->send(welcomeLocked(c->proto, c->burst));
        }
        for (auto& o : gone) o->shutdown();
        net::nameThread("link-library");
        note("library: " + f.msg["sender"]["name"].str());
        while (!quit_ && c->read(f)) {
            if (f.type != kMessage) continue;
            const std::string t = f.msg["t"].str();
            if (t == "ping") c->send(message("pong"));
            else serveLibrary(c, t, f.msg);
        }
        std::lock_guard<std::mutex> l(m_);
        libConns_.erase(std::remove(libConns_.begin(), libConns_.end(), c), libConns_.end());
        return;
    }
    std::shared_ptr<Conn> old, remote;
    {
        std::lock_guard<std::mutex> l(m_);
        old = conn_;
        conn_ = c;
        if (!artData_.empty()) artData_.clear(), artGen_++;  // the old sender's cover: never over the newcomer's tracks
        if (remoteConn_ && remoteConn_->senderId != c->senderId) remote = remoteConn_;  // another sender's remote: taken over
        senderName_ = f.msg["sender"]["name"].str();
        dropQueue();
        // the old sender's stream stops with it: its sink closes (the DAC goes back) and a newcomer opens its own on its
        // start — a flush only left the DAC open on silence, "playing", for as long as an idle newcomer (a Core coming
        // back, Home Assistant) stayed connected without a stream (DX340MAX 2026-11-08)
        stopAsked_ = true;
        paused_ = false;
        c->send(welcomeLocked(c->proto, c->burst, flacForLocked(*c), awayLocked(*c)));
    }
    if (old && old != c) kick(old);  // SPEC §1: the new sender takes over
    if (remote) kick(remote);
    capabilities();  // in_use changed
    net::nameThread("link-audio");
    note("sender: " + f.msg["sender"]["name"].str());
    // SPEC §5.7: a stream that comes as FLAC is decoded right here — what is queued, paced and played is its samples, as
    // if they had come as pcm
#ifdef ILINK_FLAC
    uint32_t flacStream = 0;
    bool flacOn = false;
    flac::Decoder dec;
    std::vector<uint8_t> pcm;
#endif
    while (!quit_ && c->read(f)) {
        if (f.type == kAudio) {
            const size_t wire = f.audio.size();  // what crossed the network (Now::received)
#ifdef ILINK_FLAC
            if (flacOn && f.stream == flacStream) {
                if (!dec.decode(f.audio.data(), f.audio.size(), pcm)) {
                    {
                        std::lock_guard<std::mutex> l(m_);
                        noFlac_ = c->senderId;
                    }
                    note("a FLAC stream that does not decode: dropped — pcm for this sender from now on");
                    break;
                }
                f.audio.swap(pcm);
            }
#endif
            std::unique_lock<std::mutex> l(m_);
            if (conn_ != c) break;
            if (haveFlushed_ && f.stream == flushedStream_) continue;
            // a sender far ahead of SPEC §5.5: stop reading until the queue drains (TCP holds it back)
            cv_.wait(l, [&] {
                return quit_ || conn_ != c || paused_ ||
                       (queuedMsLocked() < (awayLocked(*c) ? info_.awayBufferMs : info_.bufferMs) + 2000 && queue_.bytes < maxQueued_ / 2);
            });
            if (overfull()) { l.unlock(); note("sender sends far more than buffer_ms ahead: dropped"); break; }
            Item it;
            it.kind = Item::Audio;
            it.stream = f.stream;
            received_ += wire + 13;  // + the frame's own head (type, stream, first frame)
            it.bytes = std::move(f.audio);
            queue_.push(std::move(it));
            if (!spare_.empty()) { f.audio = std::move(spare_.back()); spare_.pop_back(); }  // the next chunk goes into a used buffer
            cv_.notify_all();
            continue;
        }
        if (f.type == kData) {  // the cover as bytes (SPEC §5.6, §3 0x03)
            if (f.kind != kDataArt || f.audio.empty()) continue;
            std::lock_guard<std::mutex> l(m_);
            if (conn_ != c) break;
            const bool same = artData_.size() == f.audio.size() && memcmp(artData_.data(), f.audio.data(), f.audio.size()) == 0;
            artData_.assign((const char*)f.audio.data(), f.audio.size());
            artMime_ = f.mime.empty() ? "image/jpeg" : f.mime;
            tookArt(f.stream, same);
            // ponytail: the image's buffer goes, so an audio buffer the size of a cover never lands in spare_ (a DAP's
            // RAM); one allocation per cover
            f.audio = std::vector<uint8_t>();
            continue;
        }
        if (f.type != kMessage) continue;  // SPEC §3: unknown frame types are skipped
        const json::Value& m = f.msg;
        std::string t = m["t"].str();
        if (t == "ping") { c->send(message("pong")); continue; }
        std::unique_lock<std::mutex> l(m_);
        if (conn_ != c) break;
        if ((t == "start" || t == "end") && overfull()) { l.unlock(); note("sender queues far more than buffer_ms: dropped"); break; }
        if (t == "start") {
            Item it;
            it.kind = Item::Start;
            it.stream = (uint32_t)m["stream"].i64();
            it.format = Format::fromJson(m["format"]);
            it.track = Track::fromJson(m["track"]);
            it.processed = m["processed"].boolean();
            if (info_.gain && m.has("gain_db") && it.format.kind == "pcm") it.gainDb = std::clamp(m["gain_db"].num(0), -60.0, 24.0);
            bool ok = it.format.valid() && it.format.channels == 2;
            const auto& rates = it.format.kind == "dsd" ? info_.dsdRates : info_.pcmRates;
            ok = ok && std::find(rates.begin(), rates.end(), it.format.rate) != rates.end();
            if (it.format.kind == "pcm") ok = ok && std::find(info_.pcmBits.begin(), info_.pcmBits.end(), it.format.bits) != info_.pcmBits.end();
            else ok = ok && std::find(info_.dsdLayouts.begin(), info_.dsdLayouts.end(), it.format.layout) != info_.dsdLayouts.end();
            // SPEC §5.7: how its samples travel — as they are, or as FLAC where this library decodes it (offered or not:
            // the offer may have been switched off since the sender read it)
            const bool flac = ok && it.format.codec == "flac" && it.format.kind == "pcm";
            if (it.format.codec != "pcm" && !flac) ok = false;
#ifdef ILINK_FLAC
            if (flac && !dec.open(it.format.bits)) ok = false;
            if (ok) flacOn = flac, flacStream = it.stream;
#else
            if (flac) ok = false;
#endif
            if (!ok) {
                l.unlock();
                json::Value e = message("error");
                e.set("code", json::str("format"));
                e.set("message", json::str("this endpoint does not play " + json::dump(m["format"])));
                c->send(e);
                continue;
            }
            lastStarted_ = it.stream;
            haveFlushed_ = false;
            queue_.push(std::move(it));
        } else if (t == "end") {
            Item it;
            it.kind = Item::End;
            it.stream = (uint32_t)m["stream"].i64();
            queue_.push(std::move(it));
        } else if (t == "flush") {
            dropQueue();
            flushAsked_ = true;
            haveFlushed_ = true;
            flushedStream_ = lastStarted_;
        } else if (t == "pause") {
            paused_ = true;
        } else if (t == "resume") {
            paused_ = false;
        } else if (t == "stop") {
            dropQueue();
            stopAsked_ = true;
            paused_ = false;
        } else if (t == "volume") {
            if (info_.volumeKind == "fixed") continue;
            if (m.has("value")) info_.volume = std::clamp((int)m["value"].i64(), 0, 100);
            if (m.has("muted")) info_.muted = m["muted"].boolean();
            else if (m.has("value")) info_.muted = false;  // a new level unmutes, like turning a muted amplifier's knob
            int v = info_.volume;
            bool mu = info_.muted;
            l.unlock();
            if (onVolume) onVolume(v, mu);
            continue;
        } else if (t == "handoff") {  // the sender's queue for this endpoint's own player (SPEC §7.2)
            l.unlock();
            const bool ok = onHandoff && onHandoff(m);
            json::Value r = message("handoff");
            r.set("accepted", json::boolean(ok));
            c->send(r);
            continue;
        } else if (t == "native") {  // SPEC §8: the endpoint's own player plays the queue, the sender controls it
            l.unlock();
            const bool ok = onNative && onNative(m);
            json::Value r = message("native");
            r.set("accepted", json::boolean(ok));
            c->send(r);
            continue;
        } else if (t == "native_attach") {  // SPEC §8: what its own player plays, it carries on
            l.unlock();
            json::Value a = onNativeAttach ? onNativeAttach() : json::Value();
            json::Value r = message("native_attach");
            json::Value none;
            none.type = json::Value::Arr;
            r.set("tracks", a.has("tracks") ? a["tracks"] : none);
            r.set("index", json::num(a["index"].i64(-1)));
            r.set("position_ms", json::num(a["position_ms"].i64()));
            r.set("state", json::str(a["state"].str("paused")));
            c->send(r);
            continue;
        } else if (t.rfind("library", 0) == 0) {  // SPEC §8.1
            l.unlock();
            serveLibrary(c, t, m);
            continue;
        } else if (t == "native_control") {
            l.unlock();
            if (onNativeControl) onNativeControl(m["action"].str(), m["ms"].i64(-1));
            continue;
        } else if (t == "takeback") {  // the sender wants the music back (SPEC §7.3)
            l.unlock();
            json::Value q = onTakeBack ? onTakeBack() : json::Value();
            json::Value r = message("takeback");
            r.set("tracks", q["tracks"]);
            r.set("index", json::num(q["index"].i64()));
            r.set("position_ms", json::num(q["position_ms"].i64()));
            c->send(r);
            continue;
        } else if (t == "art") {  // a cover as bytes (SPEC §5.6)
            std::string bytes = unbase64(m["data"].str());
            if (!bytes.empty()) {
                const bool same = bytes == artData_;
                artData_.swap(bytes);
                artMime_ = m["mime"].str("image/jpeg");
                tookArt((uint32_t)m["stream"].i64(), same);
            }
            continue;
        } else if (t == "meta") {
            Track tr = Track::fromJson(m["track"]);
            dropOtherArt((uint32_t)m["stream"].i64(), tr, false);  // a new song's meta comes before its start reaches the sink
            track_ = tr;
            l.unlock();
            if (onStream) onStream(tr, false);
            continue;
        }
        cv_.notify_all();
    }
    endSession(c);
    c->close();
}

void Endpoint::player() {
    net::nameThread("link-player");
    std::unique_lock<std::mutex> l(m_);
    auto setState = [&](const char* s) {
        if (state_ == s) return;
        state_ = s;
        if (onState) { l.unlock(); onState(s); l.lock(); }
    };
    while (!quit_) {
        if (stopAsked_ || flushAsked_) {
            bool close = stopAsked_;
            stopAsked_ = flushAsked_ = false;
            if (sinkOpen_) {
                const bool wasPaused = sinkPaused_;
                l.unlock();
                sink_.flush();
                // a stop while paused: the device goes unpaused (a sink keeps its pause across close + open — a test sink's clock
                // stood still and the next album stayed at 0:00, 2026-11-10); a flush while paused stays paused (SPEC §5.4)
                if (close && wasPaused) sink_.pause(false);
                if (close) sink_.close();
                l.lock();
                if (close) sinkOpen_ = false;
                sinkPaused_ = false;
            }
            heard_.clear();
            if (close) setState("idle");
            continue;
        }
        if (paused_) {
            if (sinkOpen_ && !sinkPaused_) { sinkPaused_ = true; l.unlock(); sink_.pause(true); l.lock(); }
            setState("paused");
            cv_.wait(l);
            continue;
        }
        if (queue_.empty()) {
            if (sinkOpen_ && sink_.delay() == 0 && !heard_.empty() && heard_.back().to >= 0) setState("idle");
            cv_.wait_for(l, std::chrono::milliseconds(100));
            continue;
        }
        Item it = queue_.pop();
        cv_.notify_all();  // room for the reader
        uint64_t g = gen_;
        if (it.kind == Item::Start) {
            l.unlock();
            sink_.stream(it.processed, it.gainDb);
            l.lock();
            if (!sinkOpen_ || open_ != it.format) {  // a new format: what is queued plays out first (SPEC §5.1)
                const bool was = sinkOpen_, wasPaused = sinkPaused_;
                l.unlock();
                // a paused sink is not drained (it would wait for ever — paused, then a new album at another rate stuck at 0:00,
                // 2026-11-10): what it holds was left by that pause, so it goes, and the device opens unpaused
                if (was && wasPaused) { sink_.pause(false); sink_.flush(); sink_.close(); }
                else if (was) { sink_.drain(); sink_.close(); }
                std::string err;
                bool ok = sink_.open(it.format, err);
                l.lock();
                sinkOpen_ = ok;
                sinkPaused_ = false;
                if (!ok) {
                    l.unlock();
                    json::Value e = message("error");
                    e.set("code", json::str("device"));
                    e.set("message", json::str(err));
                    say(e);
                    l.lock();
                    continue;
                }
                open_ = it.format;
                heard_.clear();
            }
            heard_.push_back({it.stream, written_, -1, false});
            while (heard_.size() > 8) heard_.pop_front();
            dropOtherArt(it.stream, it.track, true);
            track_ = it.track;  // ponytail: the stream that starts playing at the sink, a little before the DAC hears it
            trackStream_ = it.stream;
            processed_ = it.processed;
            gainDb_ = it.gainDb;
            codec_ = it.format.codec;
            setState("playing");
            if (onStream) { l.unlock(); onStream(it.track, it.processed); l.lock(); }
        } else if (it.kind == Item::End) {
            for (auto& h : heard_) if (h.stream == it.stream && h.to < 0) h.to = (int64_t)written_;
        } else if (sinkOpen_ && !heard_.empty() && heard_.back().stream == it.stream) {
            const int bpf = open_.bytesPerFrame();
            const size_t frames = it.bytes.size() / (size_t)bpf, grp = (size_t)open_.frameGroup();
            const size_t piece = std::max(grp, (size_t)std::max(1, open_.frameRate() / 50) / grp * grp);  // 20 ms: a flush lands fast
            size_t done = 0;
            while (done < frames && g == gen_ && !quit_) {
                if (paused_) {
                    if (!sinkPaused_) { sinkPaused_ = true; l.unlock(); sink_.pause(true); l.lock(); }
                    setState("paused");
                    cv_.wait(l, [&] { return !paused_ || g != gen_ || quit_; });
                    continue;
                }
                if (sinkPaused_) { sinkPaused_ = false; l.unlock(); sink_.pause(false); l.lock(); setState("playing"); }
                size_t n = std::min(frames - done, piece);
                l.unlock();
                size_t k = sink_.write(it.bytes.data() + done * bpf, n);
                l.lock();
                if (!k) break;
                written_ += k;
                done += k;
            }
        }
        if (it.kind == Item::Audio && spare_.size() < kSpare) spare_.push_back(std::move(it.bytes));  // for the reader
    }
}

void Endpoint::reporter() {
    net::nameThread("link-report");
    int tick = 0;
    while (!quit_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::vector<json::Value> out;
        std::shared_ptr<Conn> c;
        {
            std::lock_guard<std::mutex> l(m_);
            c = conn_;
            if (!c || !sinkOpen_ || heard_.empty()) continue;
            if (c->burst && info_.burstMs > 0 && (++tick & 1)) continue;  // to a bursting sender a status a second (SPEC §7)
            uint64_t d = std::min<uint64_t>(sink_.delay(), written_), at = written_ - d;
            const Heard* now = &heard_.front();
            for (auto& h : heard_) if (h.from <= at) now = &h;
            for (auto& h : heard_)
                if (h.to >= 0 && !h.endedSent && at >= (uint64_t)h.to) {
                    h.endedSent = true;
                    json::Value e = message("ended");
                    e.set("stream", json::num(h.stream));
                    out.push_back(e);
                }
            uint64_t played = at > now->from ? at - now->from : 0;  // as now(): never 2^64 − a few before its first frame is heard
            if (now->to >= 0) played = std::min<uint64_t>(played, (uint64_t)(now->to - (int64_t)now->from));
            json::Value s = message("status");
            s.set("stream", json::num(now->stream));
            s.set("played", json::num((long long)played));
            s.set("buffered_ms", json::num((long long)(queuedMsLocked() + (open_.rate ? 1000.0 * d / open_.frameRate() : 0))));
            s.set("underruns", json::num((long long)sink_.underruns()));
            s.set("state", json::str(state_));
            for (auto& kv : statusExtra_.obj) s.set(kv.first, kv.second);  // what it adds to this stream (dsp, leveling)
            if (!netByHost_ && std::chrono::steady_clock::now() - netAt_ > std::chrono::seconds(2)) {  // read every 2 s
                netAt_ = std::chrono::steady_clock::now();
                int dbm = 0;
                if (linuxWifi(dbm)) netType_ = "wifi", netDbm_ = dbm;
            }
            if (!netType_.empty()) {
                json::Value n;
                n.set("type", json::str(netType_));
                if (netDbm_) n.set("signal_dbm", json::num(netDbm_));
                s.set("network", n);
            }
            out.insert(out.begin(), s);
        }
        for (auto& m : out) c->send(m);
    }
}

}  // namespace ilink
