// Infinity Link sender: connects to an endpoint, sends streams and keeps at most buffer_ms ahead of what it has played
// (SPEC §5.5) — counting played on between the endpoint's statuses, so it sends a chunk at a time, not 500 ms bursts.
// Messages from the endpoint (status, ended, released, stopped, volume, error) arrive on onMessage.
#pragma once
#include <stdint.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "infinitylink/link.h"

namespace ilink {

// role (SPEC §4.1): "stream" (default) · "remote" (the endpoint's own player, §8 — version 2 only) · "library" (§8.3:
// browse / read its library beside whoever plays — never takes the endpoint over) · "control" (§8.4: its DSP — version
// 2 only, never takes it over; open it only where capabilities / TXT say dsp)
struct SenderInfo { std::string id, name, app, version, role; };

class Sender {
public:
    Sender();
    ~Sender();
    bool connect(const std::string& hostPort, const SenderInfo& me, std::string& err);  // hello → welcome
    void close();
    bool connected() const { return conn_ && conn_->open() && !gone_; }
    const json::Value& welcome() const { return welcome_; }
    int proto() const { return proto_; }  // the version agreed (2, or 1 with an older endpoint)
    // SPEC §4.1: what its own player can do — {native, library, takeback}, always the latest (capabilities messages;
    // with a version 1 endpoint: its welcome's native, kept by native_sources)
    json::Value capabilities();
    bool playsNative(const std::string& source);  // capabilities().native has it
    // SPEC §8.1 (version 2, a remote connection): sends {action, …fields} with a new id and waits up to waitMs for its
    // result (into *result when given). True = the endpoint answered ok; false = it said no (result.error), or no answer
    bool command(const std::string& action, const json::Value& fields = json::Value(), json::Value* result = nullptr, int waitMs = 3000);
    // what the endpoint offers now (SPEC §4.1): the welcome's formats, replaced by each `formats` message it sends
    json::Value formats() const;
    bool supports(const Format& f) const;  // the endpoint offers it now (DSD: its rate and its layout)
    // the DSD layouts it takes (SPEC §5.3 welcome formats.dsd.layouts; absent = u8): "u32be" listed = a sender whose
    // samples are already DSD_U32_BE sends them as they are (Format::layout)
    std::vector<std::string> dsdLayouts() const;
    // SPEC §5.7: how a stream of this format would travel to it now — "flac" (lossless compression) for PCM when the
    // endpoint lists it (welcome / formats `codecs`) and this library was built with it (ILINK_FLAC), else "pcm". A host
    // puts it in Format::codec before start(); a live stream keeps "pcm" (the encoder holds up to one frame, ~90 ms, back)
    std::string codecFor(const Format& f) const;
    // the stream being sent: how it travels, the bytes of audio sent for it and the bytes of samples they carry
    struct Wire { std::string codec = "pcm"; uint64_t sent = 0, pcm = 0; };
    Wire wire() const;

    // gainDb (SPEC §5.1 gain_db): the endpoint scales the stream by it — only to one whose capabilities say gain; NaN = none
    // f.codec "flac" (codecFor): write() then takes the same samples and sends them compressed; a format libFLAC does not
    // take goes as pcm after all (wire() says which)
    bool start(uint32_t stream, const Format& f, bool processed, const Track& t, double gainDb = 0.0 / 0.0);
    // Sends the samples in chunks of at most 40 ms; blocks while the endpoint has buffer_ms queued. False = gone, or
    // the stream was flushed / stopped meanwhile (abort() from another thread ends a wait at once).
    bool write(const uint8_t* data, size_t frames);
    bool end(uint32_t stream);
    // SPEC §5.7: what the encoder of a flac stream still holds (up to one frame, ~90 ms) goes out now — every sample
    // written so far reaches the endpoint. For a host that has nothing more to write for the moment and sends no end()
    // (the last track of a queue runs out, a radio stream waits): without it the endpoint never gets — and never plays —
    // those last samples. Nothing held, or a pcm stream: does nothing. False = the connection went.
    bool sync();
    bool flush();
    bool pause();
    bool resume();
    bool stop();
    bool volume(int value);
    bool mute(bool on);
    bool meta(uint32_t stream, const Track& t);
    // the queue goes to the endpoint's own player (SPEC §7.2; only when welcome()["handoff"]): tracks, where, how far
    bool handoff(const json::Value& tracks, int index, long long positionMs);
    bool takeBack();  // ask for the endpoint's own queue back (SPEC §7.3); the answer comes to onMessage ("takeback")
    // its library, a page (SPEC §8.1; the answer comes to onMessage, "library"): gen = what the sender has from it
    bool library(long long from, int count, const std::string& gen);
    bool nativeAttach();
    bool libraryArt(const std::string& id);
    bool libraryRead(const std::string& id, int64_t offset, int length);  // a piece of one of its files (SPEC §8.1)
    // SPEC §8.3, capabilities.library_sync: a track's favourite there; plays both ways (add: [[id, epoch s]…]). Answers to onMessage
    bool libraryLove(const std::string& id, bool on);
    bool libraryPlays(const std::string& since, const json::Value& add);
    bool libraryLists(const std::string& gen);  // its favourites and playlists (SPEC §8.1); the answer to onMessage  // a cover of its library (SPEC §8.1); the answer comes to onMessage  // what the endpoint's own player plays (SPEC §8); the answer comes to onMessage ("native_attach")
    // native play (SPEC §8, only for the sources welcome()["native"] lists): the endpoint's own player plays these, the
    // sender controls it; native_status messages come to onMessage
    // keep: the queue was edited — the endpoint carries on with the track that plays when it is queue[index]
    bool native(const json::Value& queue, int index, long long positionMs, bool keep = false);
    bool nativeControl(const std::string& action, long long ms = -1);  // play | pause | next | previous | seek | stop
    // the cover itself (a local file's: no URL to give) — JPEG / PNG (SPEC §5.6): as a data frame when capabilities say
    // binary (up to ~1 MiB), else base64 JSON (up to 700 KB)
    bool art(uint32_t stream, const std::string& mime, const std::string& bytes);
    void abort();  // a write() waiting for room returns false now
    double aheadMs();  // audio sent and not played yet (SPEC §5.5), counted on between statuses — a live sender may keep less than buffer_ms
    // the connection was given up because what was sent did not arrive (SPEC §5.5) — connected() is false by then
    bool stuck() const { return stuck_; }

    std::function<void(const json::Value& msg)> onMessage;  // the endpoint's messages (reader thread)
    // SPEC §3 0x03: a library_read / library_art answer that came as a data frame. Called on the reader thread just
    // before onMessage gets the same answer as JSON — {"t": "library_read", id, offset, size} / {"t": "library_art", id,
    // mime, size}, with "binary": true and no "data". The bytes are f.audio, valid only during this call: copy what you
    // keep. An endpoint without binary answers in base64 JSON as before ("data", no "binary").
    std::function<void(const Frame& f)> onData;

private:
    bool control(const json::Value& m) { return connected() && conn_->send(m); }
    double aheadMsLocked() const;
    void reader();
    void pinger();

    std::shared_ptr<Conn> conn_;
    json::Value welcome_;
    std::atomic<bool> gone_{false};
    std::thread reader_, pinger_;
    mutable std::mutex m_;
    std::vector<std::string> native_;  // under m_
    json::Value caps_;                 // under m_
    json::Value formats_;              // under m_ (formats())
    std::vector<std::string> codecs_;  // what it takes (welcome / formats `codecs`, SPEC §5.7), under m_
    // the stream being sent as FLAC (SPEC §5.7): its encoder, under encMu_ — never taken together with m_
    struct Enc;
    std::unique_ptr<Enc> enc_;
    std::mutex encMu_;
    std::atomic<bool> wireFlac_{false};
    std::atomic<uint64_t> wireSent_{0}, wirePcm_{0};  // wire()
    std::map<long long, json::Value> results_;  // command results by id, under m_
    long long nextId_ = 0;
    int proto_ = 1;
    bool hello(const std::string& hostPort, const SenderInfo& me, int proto, std::string& err, bool& protoRefused);
    std::condition_variable cv_;
    // what was sent per stream, and what the endpoint last said it played
    // rate = frames a second; group = frames that travel as one (DSD u32be: 4); ended = its `end` was sent
    struct Sent { uint64_t frames = 0; int rate = 0; int bpf = 0; int group = 1; bool ended = false; };
    std::map<uint32_t, Sent> sent_;
    uint32_t cur_ = 0, heardStream_ = 0;
    uint64_t played_ = 0;
    bool haveStatus_ = false, abort_ = false;
    // the last status said playing and had moved on: played_ counts on from statusAt_ at the stream's rate
    bool statusPlaying_ = false;
    std::chrono::steady_clock::time_point statusAt_{};
    int bufferMs_ = 2000;
    int burstMs_ = 0;      // the endpoint's burst_ms (SPEC §5.5): 0 = keep it topped up
    bool filling_ = true;  // a burst is on: send until bufferMs_ is ahead, then wait until burstMs_ of it has played
    // sent, not arriving (pinger): the last status said playing with next to nothing buffered (dry_), and since stuckAt_
    // (steady ms, 0 = not so) it has heard less than a second more of stuckStream_ while seconds are counted ahead of it
    bool dry_ = false;
    long long stuckAt_ = 0;
    uint32_t stuckStream_ = 0;
    uint64_t stuckPlayed_ = 0;
    std::atomic<bool> stuck_{false};
    std::atomic<bool> writing_{false};  // write() is handing audio to the connection (or waits for it to take it)
};

}  // namespace ilink
