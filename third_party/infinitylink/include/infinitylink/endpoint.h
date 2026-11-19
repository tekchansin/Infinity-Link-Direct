// Infinity Link endpoint: listens, takes one sender at a time, plays its streams into a Sink at the Sink's pace. Only
// peers on a private network get in (SPEC §9; EndpointInfo::anyAddress turns that off).
#pragma once
#include <stdint.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "infinitylink/link.h"

namespace ilink {

// Where the endpoint plays: a DAC (ALSA, AAudio, the DAP's own path), a file, nothing. write() blocks at the device's
// pace. All calls come from the endpoint's player thread, except delay() and underruns(), which its reporter also reads.
struct Sink {
    virtual ~Sink() = default;
    virtual bool open(const Format& f, std::string& err) = 0;
    // frames taken; 0 = the device failed. DSD u32be: whole groups of 4 frames (the endpoint never splits one)
    virtual size_t write(const uint8_t* data, size_t frames) = 0;
    virtual void pause(bool on) { (void)on; }
    virtual void flush() {}    // drop what the device still holds (seek, skip)
    virtual void drain() {}    // wait until everything written has been heard
    virtual void close() = 0;  // let the device go
    // a stream starts (before open() when its format is new): whether the sender processed it (SPEC §5.1) and the gain it
    // asks for (SPEC §5.1 gain_db; NaN = none) — an endpoint with EndpointInfo::gain scales by it before its own DSP
    virtual void stream(bool processed, double gainDb) { (void)processed; (void)gainDb; }
    virtual uint64_t delay() { return 0; }      // frames written but not heard yet (any thread)
    virtual uint64_t underruns() { return 0; }  // the device ran dry this many times (any thread)
};

struct EndpointInfo {
    std::string id, name, model, vendor;
    std::string dsp;  // the endpoint's own sound processing, if any (SPEC §5.1)
    std::vector<int> pcmRates{44100, 48000, 88200, 96000, 176400, 192000};
    std::vector<int> pcmBits{16, 24, 32};
    std::vector<int> dsdRates;  // empty = no native DSD
    // the DSD layouts its Sink takes (SPEC §5.3): "u8" always; "u32be" too when the Sink packs through Packer or its DAC
    // wants DSD_U32_BE (then the bytes go as they came)
    std::vector<std::string> dsdLayouts{"u8"};
    std::string volumeKind = "fixed";  // hardware | software | fixed
    int volume = 100;
    int volumeSteps = 0;  // its own top step (SPEC §4 volume.steps; 63 on a DX270): 0 = not said. The wire stays 0..100
    bool muted = false;
    int bufferMs = 2000;
    int burstMs = 0;  // > 0: refill in bursts — the sender lets this much run down, then fills to bufferMs (SPEC §5.5)
    // … and for a sender that comes through a forwarder on this device (its address is 127.0.0.1: Link Anywhere's tailnet
    // forwarder) — a path that goes away for seconds at a time (mobile data changing cell: 3 s without a network, then
    // 6 s through a relay, S25 2026-11-19) needs more ahead than a LAN does. 0 = the two above
    int awayBufferMs = 0;
    int awayBurstMs = 0;
    bool handoff = false;  // it has a player of its own: a sender may hand it the queue (SPEC §7.2)
    bool gain = false;     // it applies a stream's gain_db (SPEC §5.1, capabilities.gain): Sink::stream gets it
    std::vector<std::string> native;  // what that player plays by itself under the sender's control (SPEC §8): qobuz, tidal, library
    bool anyAddress = false;  // take connections from any address, not only private networks (SPEC §9) — tests, odd networks
    std::string address;      // listen on this address only ("127.0.0.1": this machine alone); "" = every interface
    json::Value welcome() const;
};

class Endpoint {
public:
    Endpoint(EndpointInfo info, Sink& sink);
    ~Endpoint();
    bool listen(int port, std::string& err);  // binds every interface (or EndpointInfo::address); port 0 = a free one
    int port() const;                         // the port it listens on
    void run();                               // serves senders until stop()
    void stop();
    void localVolume(int value, bool muted);  // the endpoint's own control moved: the sender's slider follows
    void release();                           // the endpoint's own player took the device: the sender is told, audio dropped
    void remote(const std::string& action, long long ms = -1);  // the endpoint's own buttons → the sender (SPEC §7.1)
    // the endpoint's network, sent in every status (SPEC §6): type "wifi" | "wired", Wi-Fi signal in dBm (0 = not known).
    // On Linux (Infinity OS, a Pi) the library reads /proc/net/wireless by itself; a host that knows better (Android's
    // WifiManager) calls this, which turns that off.
    void network(const std::string& type, int signalDbm);
    // the host's own cap on what it offers (a phone's stream quality per network, SPEC §4.1 formats change): PCM rates
    // above maxRate and depths above maxBits are no longer offered, nor DSD unless dsd; 0 = no cap. A cap that leaves no
    // rate (no depth) keeps the lowest one. Every open connection hears the new formats at once; what it already plays
    // in an older format still plays — the sender changes at its next track.
    void limit(int maxRate, int maxBits, bool dsd);
    // Lossless compression (SPEC §5.7, codec "flac") — whom the endpoint offers it to: nobody (the default); a sender that
    // comes through a forwarder on this machine (peer 127.0.0.1: a tailnet — the music crosses the internet, a phone's
    // Link Anywhere); everybody. A library built without ILINK_FLAC offers it to nobody. The sender that plays now hears
    // a change at once (`formats`) and follows it from its next stream; a stream it already sends is decoded to its end.
    enum { kCompressOff = 0, kCompressForwarded = 1, kCompressAll = 2 };
    void compress(int whom);

    // what the endpoint plays now, for its own screen
    struct Now {
        bool connected = false;
        std::string sender;  // the sender's name
        std::string remote;  // the name of the sender on its remote connection (SPEC §8), "" = none — it controls this player
        std::string senderId;  // its id (hello sender.id)
        std::string state = "idle";
        Track track;
        bool processed = false;
        double gainDb = 0.0 / 0.0;  // the sender's volume leveling for this stream (SPEC §5.1 gain_db): NaN = none
        Format format;
        uint64_t played = 0;  // frames of the current stream heard
        int volume = 0;
        bool muted = false;
        unsigned artGen = 0;  // bumps when a new cover arrived (art() returns it)
        int bufferedMs = 0;   // queued here + in the device (status buffered_ms)
        uint64_t underruns = 0;  // the device ran dry (status underruns)
        std::string senderAddr;  // where the sender connected from ("127.0.0.1" = a forwarder on this machine, a tailnet)
        uint64_t received = 0;   // audio bytes read from senders since the endpoint started (a host's data meter)
        std::string codec = "pcm";  // how the stream heard now travels (SPEC §5.7): "flac" = compressed, lossless
    };
    Now now();
    // senders that follow its DSP state (SPEC §8.4: control connections + the remote one) — a host may build the parts
    // only they read (§8.5 controls, §8.6 profiles) only while there is one
    int watchers();
    std::string art(std::string& mime);  // the sender's latest cover bytes ("" = none) — copy it only when artGen moved

    // the host's hooks (called from the endpoint's threads)
    std::function<void(int value, bool muted)> onVolume;             // the sender moved the volume
    std::function<void(const Track& t, bool processed)> onStream;    // a stream starts playing
    std::function<void(const std::string& state)> onState;           // playing | paused | idle
    std::function<void(const std::string& line)> log;
    // the sender hands its queue over (SPEC §7.2 v1 message; v2: the `handoff` command, which the host's onCommand passes
    // here — set both for capabilities.handoff): true = this endpoint's own player takes it (and soon the DAC)
    std::function<bool(const json::Value& handoff)> onHandoff;
    // the sender takes the music back (SPEC §7.3): {tracks, index, position_ms} of this endpoint's own player, which
    // pauses; a Null value = nothing to give
    std::function<json::Value()> onTakeBack;
    // native play (SPEC §8): the sender's queue on this endpoint's own player, the sender keeps the buttons
    std::function<bool(const json::Value& native)> onNative;
    std::function<void(const std::string& action, long long ms)> onNativeControl;
    // its own library, a page at a time (SPEC §8.1): the request in, the answer's fields out (gen, total, from, tracks —
    // or gen, total, unchanged); the endpoint names the message
    std::function<json::Value(const json::Value& request)> onLibrary;
    // its favourites and playlists (SPEC §8.1): {gen} in → {gen, loved, playlists} or {gen, unchanged}
    std::function<json::Value(const json::Value& request)> onLibraryLists;
    // favourites and plays kept alike with the sender (SPEC §8.3, capabilities.library_sync — both or neither): the
    // request in ({id, on} / {since, add}), the answer's fields out ({ok, error?} / {plays, next, added})
    std::function<json::Value(const json::Value& request)> onLibraryLove;
    std::function<json::Value(const json::Value& request)> onLibraryPlays;
    // a sender that just connected asks what this endpoint's own player plays (SPEC §8): {tracks, index, position_ms,
    // state} — from then on that queue is a native one (status, the sender's buttons); no tracks = nothing plays
    std::function<json::Value()> onNativeAttach;
    // the cover of one of its library's tracks (SPEC §8.1): the bytes (JPEG / PNG, well under 700 KB), "" = none
    std::function<std::string(const std::string& id, std::string& mime)> onLibraryArt;
    // a piece of one of its library's files, for a sender that plays it elsewhere (SPEC §8.1): bytes from `offset`, at
    // most `length`; size = the whole file's (-1 = no such file / not lent)
    std::function<std::string(const std::string& id, int64_t offset, int length, int64_t& size)> onLibraryRead;
    // every ~500 ms; count ≥ 0 (the first one after `native`): how many tracks from the asked index it plays
    // extra: {format, output, dsp} (SPEC §8) — what it really plays, what its DAC gets, the DSP that applies
    void nativeStatus(const std::string& state, int index, long long positionMs, long long durationMs, int count = -1,
                      const json::Value& extra = json::Value());
    void nativeSources(const std::vector<std::string>& sources);  // signed in / out: capabilities go to every connection

    // ── protocol 2 (SPEC §8): the endpoint's own player on a `remote` connection
    // a command in ({id, action, …}) → the result's fields out ({ok, error, …}); unset = every command "unsupported"
    std::function<json::Value(const json::Value& command)> onCommand;
    // what its own player plays (SPEC §8.2: state, index, count, position_ms, duration_ms, queue_rev, format, output,
    // dsp; "queue" only when it changed) — kept, and sent to the remote connection now and when one opens
    void playerState(const json::Value& player);
    // ── DSP control (SPEC §8.4): `dsp_get` / `dsp_set` from a `control` or `remote` connection → the result's fields
    // ({ok, error, settings, …}); set = capabilities.dsp. A `control` connection never takes the endpoint over.
    std::function<json::Value(const json::Value& command)> onDsp;
    // ── file transfer (SPEC §8.7): `transfer` {on?} from a `control` or `remote` connection → {ok, on, url, user,
    // password, error}; set = capabilities.transfer
    std::function<json::Value(const json::Value& command)> onTransfer;
    // its DSP changed (from any side): kept, pushed as {"t":"dsp", …} to every control / remote connection now and when
    // one opens
    void dspState(const json::Value& state);
    // what the endpoint adds to the stream it plays (SPEC §7 status `dsp`, `leveling`): merged into every status
    void statusExtra(const json::Value& extra);

private:
    struct Item {
        enum Kind { Start, Audio, End } kind = Start;
        uint32_t stream = 0;
        Format format;
        Track track;
        bool processed = false;
        double gainDb = 0.0 / 0.0;  // NaN = none
        std::vector<uint8_t> bytes;
    };
    struct Heard { uint32_t stream; uint64_t from; int64_t to; bool endedSent; };  // to = -1 while open
    void session(std::shared_ptr<Conn> c);  // one sender's connection (its reader)
    bool serveLibrary(const std::shared_ptr<Conn>& c, const std::string& t, const json::Value& m);  // SPEC §8.1
    // library-only connections (hello role "library", SPEC §8.3): one per sender — two computers keep theirs side by
    // side; that sender's newer one replaces its older one; at most kMaxLibrary (the oldest goes), under m_
    std::vector<std::shared_ptr<Conn>> libConns_;
    static constexpr size_t kMaxLibrary = 8;
    std::shared_ptr<Conn> remoteConn_;  // protocol 2 role "remote" (SPEC §8), under m_
    std::string remoteName_;            // its sender's name, under m_
    json::Value lastPlayer_, lastQueue_;
    std::vector<std::shared_ptr<Conn>> ctlConns_;  // DSP control connections (hello role "control"), under m_
    json::Value lastDsp_, statusExtra_;            // the latest `dsp` state; fields every status carries, under m_
    json::Value commandResult(const json::Value& command, bool dspOnly);  // a `command` → its `result`  // the latest `player` (without its queue) and the latest queue, under m_
    json::Value welcomeLocked(int proto, bool burst, bool flac = false, bool away = false) const;
    bool awayLocked(const Conn& c) const;  // this sender gets awayBufferMs / awayBurstMs
    bool flacForLocked(const Conn& c) const;  // compress(): this sender is offered codec flac
    int compress_ = kCompressOff;             // under m_
    std::string noFlac_;   // the sender whose FLAC did not decode: pcm for it from then on (under m_)
    std::string codec_ = "pcm";  // Now::codec: the stream at the sink (player thread, under m_)
    json::Value formatsLocked() const;  // info_'s formats under limit() — what welcome and `formats` offer
    int limitRate_ = 0, limitBits_ = 0;  // limit(), under m_ (0 = none); a start still needs only info_'s formats
    bool limitDsd_ = true;
    uint64_t received_ = 0;  // Now::received, under m_
    json::Value capabilitiesLocked() const;
    void capabilities();  // SPEC §4.1: to every open connection (version 1: native_sources)
    void endSession(const std::shared_ptr<Conn>& c);
    void player();                          // queue → sink
    void reporter();                        // status every 500 ms
    void dropQueue();                       // m_ held
    void dropOtherArt(uint32_t stream, const Track& t, bool started);  // m_ held, before track_ becomes t (SPEC §5.6)
    void tookArt(uint32_t stream, bool same = false);                  // m_ held: a cover came, for the track shown now
    void say(const json::Value& msg);       // to the current sender, if any
    void note(const std::string& s) { if (log) log(s); }
    double queuedMsLocked() const;

    EndpointInfo info_;
    Sink& sink_;
    int lfd_ = -1;
    std::string artData_, artMime_;
    unsigned artGen_ = 0;
    uint32_t artStream_ = 0;  // the stream the cover came for (SPEC §5.6)
    std::string artAlbum_, artUrl_;  // the album / cover URL of the track it is the cover of
    std::string netType_;
    int netDbm_ = 0;
    bool netByHost_ = false;
    std::chrono::steady_clock::time_point netAt_{};
    std::atomic<bool> quit_{false};
    std::mutex m_;
    std::condition_variable cv_;
    std::shared_ptr<Conn> conn_;   // the sender playing now
    std::thread session_;
    // the queue: a ring of Items that doubles when full — no allocation per chunk once it has grown (std::deque on
    // libstdc++ allocated a node per Item); a popped slot keeps its moved-from Item until a push moves over it
    struct Queue {
        std::vector<Item> v;
        size_t head = 0, n = 0;
        size_t bytes = 0;  // what the queued Items hold (their samples, track texts, the slots themselves)
        static size_t cost(const Item& it);
        bool empty() const { return n == 0; }
        size_t size() const { return n; }
        const Item& operator[](size_t i) const { return v[(head + i) % v.size()]; }
        void push(Item&& it);
        Item pop();
        void clear();
    } queue_;
    // audio buffers the player is done with, for the reader to read the next chunk into: no allocation per chunk, under m_
    std::vector<std::vector<uint8_t>> spare_;
    static constexpr size_t kSpare = 16;  // a burst of chunks still finds one; more would only hold memory
    // the most a sender may have queued here (bytes): twice what buffer_ms + 2 s are at the fastest format offered, plus
    // 16 MiB. The reader stops at half of it like at buffer_ms; paused (it must still read `resume`) or flooding with
    // starts / ends, a sender past it is dropped — else it grew without end (fuzz-test, 1 GiB, 2026-11-12)
    size_t maxQueued_ = 0;
    bool overfull() const { return queue_.bytes >= maxQueued_; }  // m_ held
    uint64_t gen_ = 0;             // bumps on flush / stop / a new sender: the player drops what it holds
    bool paused_ = false, stopAsked_ = false, flushAsked_ = false, sinkPaused_ = false;
    uint32_t lastStarted_ = 0, flushedStream_ = 0;  // audio of a flushed stream is dropped
    bool haveFlushed_ = false;
    // one per connection: its thread, done once it returned (the next accept joins it — a DAP runs for days and senders
    // reconnect), its connection while it lasts (stop() shuts every one, hello or not)
    struct Session {
        std::thread t;
        std::shared_ptr<std::atomic<bool>> done;
        std::weak_ptr<Conn> conn;
    };
    std::vector<Session> sessions_;
    // what the player wrote (player thread + reporter under m_)
    Format open_;
    bool sinkOpen_ = false;
    uint64_t written_ = 0;
    std::deque<Heard> heard_;
    std::string state_ = "idle";
    std::thread player_, reporter_;
    std::string senderName_;
    Track track_;            // the stream heard now (start / meta)
    uint32_t trackStream_ = 0;  // … the stream it came with (its start reached the sink): now() counts that one's frames
    bool processed_ = false;
    double gainDb_ = 0.0 / 0.0;  // its gain_db (Now)
};

}  // namespace ilink
