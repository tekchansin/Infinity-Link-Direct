// Infinity-Link-Direct — an Infinity Link endpoint for Windows that plays through a DAC maker's own ASIO® driver: the maker's
// path to the DAC, and native DSD where that driver offers it (Windows' own audio has no DSD — WASAPI is PCM / DoP only).
//
// GPLv3 (LICENSE): built with Steinberg's ASIO SDK under its GPLv3 option (third_party/asiosdk). ASIO is a registered
// trademark of Steinberg Media Technologies GmbH. The Infinity Link library is MIT (third_party/infinitylink).
//
//   Infinity-Link-Direct --list                       the ASIO drivers here and what each one plays
//   Infinity-Link-Direct --driver NAME [--name S] [--port N] [--buffer MS] [--volume fixed|software] [--any-address]
//   Infinity-Link-Direct --driver NAME --local        this computer only (127.0.0.1, not announced, port chosen here): prints
//                                            "port N" and quits when its standard input closes — Infinity Core on this PC
//   Infinity-Link-Direct --driver NAME --panel        the driver's own settings window (buffer size, …), then quits
//
// --driver takes part of a name (as --list prints it). --volume software: a volume the sender moves (scaled here, TPDF
// dither below 100 %); DSD is then not offered, so the sender sends it as PCM and the volume always works. Default fixed:
// nothing touches the samples — the volume is the DAC's / amplifier's.
//
// How it runs: every ASIO call happens on one thread (COM single-threaded apartment + a hidden window: drivers expect
// both); the driver calls bufferSwitch on its own thread, which reads a lock-free FIFO the endpoint's player fills. The
// driver is loaded only while something plays (and 10 s after), so other programs can use the DAC when nothing of ours
// plays.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>  // CommandLineToArgvW
#include <objbase.h>

#include <signal.h>
#include <stdio.h>

#include <algorithm>
#include <cstdarg>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"

#include "infinitylink/safe_start.h"
#include "infinitylink/discovery.h"
#include "infinitylink/endpoint.h"
#include "infinitylink/pack.h"

using namespace ilink;

namespace {

const int kPcmRates[] = {44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000};
const int kDsdRates[] = {2822400, 3072000, 5644800, 6144000, 11289600, 12288000, 22579200, 24576000};
constexpr int kIdleReleaseMs = 10000;  // the driver stays loaded this long after a stream (a format change reopens fast)
constexpr int kStallMs = 1000;         // no bufferSwitch this long while started = the DAC went away

std::string utf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

void logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "Infinity-Link-Direct: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

// ───────────────────────────── the drivers installed (HKLM\SOFTWARE\ASIO) ─────────────────────────────
struct DriverEntry { std::string name; CLSID clsid; };

std::vector<DriverEntry> installedDrivers() {
    std::vector<DriverEntry> out;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ | KEY_WOW64_64KEY, &root) != ERROR_SUCCESS) return out;
    for (DWORD i = 0;; i++) {
        wchar_t key[256];
        DWORD kl = 256;
        if (RegEnumKeyExW(root, i, key, &kl, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        wchar_t clsid[64];
        DWORD cb = sizeof clsid, type = 0;
        if (RegGetValueW(root, key, L"CLSID", RRF_RT_REG_SZ, &type, clsid, &cb) != ERROR_SUCCESS) continue;
        DriverEntry d;
        d.name = utf8(key);
        if (CLSIDFromString(clsid, &d.clsid) == S_OK) out.push_back(d);
    }
    RegCloseKey(root);
    return out;
}

// ───────────────────────────── sample types ─────────────────────────────
bool pcmTypeOk(ASIOSampleType t) {
    switch (t) {
        case ASIOSTInt16LSB: case ASIOSTInt24LSB: case ASIOSTInt32LSB: case ASIOSTFloat32LSB: case ASIOSTFloat64LSB:
        case ASIOSTInt32LSB16: case ASIOSTInt32LSB18: case ASIOSTInt32LSB20: case ASIOSTInt32LSB24: return true;
        default: return false;
    }
}
// the PCM depths a type carries as they are (a 32-bit float holds 24 bits exactly)
std::vector<int> pcmBitsFor(ASIOSampleType t) {
    switch (t) {
        case ASIOSTInt32LSB: case ASIOSTFloat64LSB: return {16, 24, 32};
        case ASIOSTInt24LSB: case ASIOSTInt32LSB24: case ASIOSTFloat32LSB: return {16, 24};
        default: return {16};
    }
}
int typeBits(ASIOSampleType t) {  // the bits that count, for the dither
    switch (t) {
        case ASIOSTInt32LSB: case ASIOSTFloat64LSB: return 32;
        case ASIOSTInt24LSB: case ASIOSTInt32LSB24: case ASIOSTFloat32LSB: return 24;
        case ASIOSTInt32LSB20: return 20;
        case ASIOSTInt32LSB18: return 18;
        default: return 16;
    }
}
int typeBytes(ASIOSampleType t) {
    switch (t) {
        case ASIOSTInt16LSB: return 2;
        case ASIOSTInt24LSB: return 3;
        case ASIOSTFloat64LSB: return 8;
        default: return 4;
    }
}
bool dsdTypeOk(ASIOSampleType t) { return t == ASIOSTDSDInt8MSB1 || t == ASIOSTDSDInt8LSB1; }

uint8_t gRev[256];  // bit-reversed bytes: Link's DSD has the earliest bit in the MSB (SPEC §5.3), LSB1 drivers want it low
void initRev() {
    for (int i = 0; i < 256; i++) {
        uint8_t r = 0;
        for (int b = 0; b < 8; b++) if (i & (1 << b)) r |= (uint8_t)(0x80 >> b);
        gRev[i] = r;
    }
}

// ───────────────────────────── a lock-free FIFO: the player writes, the driver's thread reads ─────────────────────────────
struct Fifo {
    std::vector<uint8_t> b;
    std::atomic<uint64_t> w{0}, r{0};  // bytes ever written / read
    void reset(size_t bytes) { b.assign(bytes, 0); w = 0; r = 0; }
    size_t used() const { return (size_t)(w.load(std::memory_order_acquire) - r.load(std::memory_order_acquire)); }
    size_t room() const { return b.size() - used(); }
    void put(const uint8_t* d, size_t n) {  // n <= room()
        uint64_t at = w.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n;) {
            size_t o = (size_t)(at % b.size()), k = std::min(n - i, b.size() - o);
            memcpy(b.data() + o, d + i, k);
            i += k, at += k;
        }
        w.store(at, std::memory_order_release);
    }
    size_t get(uint8_t* d, size_t n) {
        n = std::min(n, used());
        uint64_t at = r.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n;) {
            size_t o = (size_t)(at % b.size()), k = std::min(n - i, b.size() - o);
            memcpy(d + i, b.data() + o, k);
            i += k, at += k;
        }
        r.store(at, std::memory_order_release);
        return n;
    }
};

// ───────────────────────────── the driver, on its own thread ─────────────────────────────
struct Asio;
Asio* gAsio = nullptr;
bool gNoOutputReady = false;  // --no-output-ready: for a driver that handles ASIOOutputReady badly  // ASIO callbacks carry no context; one driver per process anyway

struct Caps {
    std::vector<int> pcmRates, dsdRates;
    ASIOSampleType pcmType = -1, dsdType = -1;
    long outputs = 0;
    std::string name;
};

struct Asio {
    CLSID clsid{};
    std::string want;  // the driver's registry name
    // the ASIO thread
    std::thread th;
    DWORD thId = 0;
    HWND wnd = nullptr;
    std::mutex qm;
    std::deque<std::function<void()>> q;
    std::atomic<bool> quit{false};
    // the driver (ASIO thread only, except what the callback reads)
    IASIO* drv = nullptr;
    bool prepared = false, running = false, dsdMode = false, canOutputReady = false;
    double rate = 0;
    long bufFrames = 0, outLatency = 0;
    ASIOSampleType type = -1;
    ASIOBufferInfo bi[2]{};
    ASIOCallbacks cb{};
    ULONGLONG closedAt = 0;  // when the last stream closed (the idle release)
    // shared with the callback
    Fifo fifo;
    std::atomic<bool> paused{false}, flushReq{false}, started{false};
    std::atomic<ULONGLONG> lastSwitch{0};
    std::atomic<uint64_t> under{0};
    std::atomic<uint64_t> switches{0};
    std::atomic<uint32_t> carried[8]{};  // device frames of music each recent half-buffer held (by switch number)
    std::atomic<double> gain{1.0};
    std::atomic<bool> resetReq{false};
    int frameBytes = 8;  // FIFO bytes per device frame: PCM S32 stereo 8, DSD u8 stereo 2
    HANDLE spaceEv = nullptr;  // set by the callback after it read (write() waits on it)
    std::vector<uint8_t> tmp;
    std::minstd_rand rng{12345};

    // runs fn on the ASIO thread and waits for it
    template <class F> auto call(F fn) -> decltype(fn()) {
        if (GetCurrentThreadId() == thId) return fn();
        std::packaged_task<decltype(fn())()> t(std::move(fn));
        auto f = t.get_future();
        {
            std::lock_guard<std::mutex> l(qm);
            q.emplace_back([&t] { t(); });
        }
        PostThreadMessageW(thId, WM_APP, 0, 0);
        return f.get();
    }

    bool startThread() {
        std::promise<bool> ready;
        auto f = ready.get_future();
        th = std::thread([this, &ready] {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            thId = GetCurrentThreadId();
            MSG m;
            PeekMessageW(&m, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // the thread's message queue, before anyone posts
            wnd = CreateWindowExW(0, L"STATIC", L"Infinity-Link-Direct", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
            ready.set_value(true);
            SetTimer(nullptr, 0, 500, nullptr);  // the idle release and the reset request are checked here
            while (!quit && GetMessageW(&m, nullptr, 0, 0) > 0) {
                if (m.message == WM_APP || m.message == WM_TIMER) {
                    for (;;) {
                        std::function<void()> fn;
                        {
                            std::lock_guard<std::mutex> l(qm);
                            if (q.empty()) break;
                            fn = std::move(q.front());
                            q.pop_front();
                        }
                        fn();
                    }
                    if (m.message == WM_TIMER) tick();
                }
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            unload();
            if (wnd) DestroyWindow(wnd);
            CoUninitialize();
        });
        return f.get();
    }
    void stopThread() {
        quit = true;
        PostThreadMessageW(thId, WM_QUIT, 0, 0);
        if (th.joinable()) th.join();
    }

    std::string driverError() {
        char e[128] = "";
        if (drv) drv->getErrorMessage(e);
        return e[0] ? e : "the driver did not say why";
    }

    bool load(std::string& err) {  // ASIO thread
        if (drv) return true;
        HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, (void**)&drv);  // ASIO: the CLSID is the IID
        if (FAILED(hr) || !drv) {
            char b[96];
            snprintf(b, sizeof b, "the driver could not be loaded (0x%08lx) — a 32-bit-only driver cannot load here", (unsigned long)hr);
            err = b;
            drv = nullptr;
            return false;
        }
        if (!drv->init(wnd)) {
            err = "the driver did not start: " + driverError();
            drv->Release();
            drv = nullptr;
            return false;
        }
        prepared = running = false;
        dsdMode = false;
        rate = 0;
        return true;
    }
    void unprepare() {  // ASIO thread
        if (!drv) return;
        if (running) drv->stop();
        if (prepared) drv->disposeBuffers();
        running = prepared = false;
        started = false;
    }
    void unload() {  // ASIO thread
        unprepare();
        if (drv) { drv->Release(); drv = nullptr; }
    }
    bool setDsd(bool on) {  // ASIO thread, not prepared
        ASIOIoFormat f{};
        f.FormatType = on ? kASIODSDFormat : kASIOPCMFormat;
        ASIOError e = drv->future(kAsioSetIoFormat, &f);
        if (on) { if (e != ASE_SUCCESS) return false; }
        dsdMode = on;
        return true;  // PCM: a driver without DSD does not know the selector — it is in PCM already
    }
    ASIOSampleType outType() {
        ASIOChannelInfo ci{};
        ci.channel = 0;
        ci.isInput = ASIOFalse;
        return drv->getChannelInfo(&ci) == ASE_OK ? ci.type : -1;
    }

    Caps probe(std::string& err) {  // ASIO thread; leaves the driver loaded, in PCM
        Caps k;
        if (!load(err)) return k;
        char nm[64] = "";
        drv->getDriverName(nm);
        k.name = nm;
        long ins = 0;
        drv->getChannels(&ins, &k.outputs);
        if (k.outputs < 2) { err = "the driver has fewer than two outputs"; return k; }
        setDsd(false);
        k.pcmType = outType();
        if (pcmTypeOk(k.pcmType))
            for (int r : kPcmRates) if (drv->canSampleRate(r) == ASE_OK) k.pcmRates.push_back(r);
        ASIOIoFormat f{};
        f.FormatType = kASIODSDFormat;
        if (drv->future(kAsioCanDoIoFormat, &f) == ASE_SUCCESS && setDsd(true)) {
            k.dsdType = outType();
            if (dsdTypeOk(k.dsdType))
                for (int r : kDsdRates) if (drv->canSampleRate(r) == ASE_OK) k.dsdRates.push_back(r);
            setDsd(false);
        }
        if (k.pcmRates.empty() && err.empty()) err = "the driver offers no PCM rate it can play as it is";
        return k;
    }

    // ── the stream ──
    bool open(bool dsd, int r, std::string& err) {  // ASIO thread
        if (!load(err)) return false;
        unprepare();
        if (dsd != dsdMode && !setDsd(dsd)) { err = "the driver would not switch to DSD"; return false; }
        if (rate != r) {
            if (drv->setSampleRate(r) != ASE_OK) { err = "the driver would not run at " + std::to_string(r) + " Hz"; return false; }
            rate = r;
        }
        type = outType();
        if (dsd ? !dsdTypeOk(type) : !pcmTypeOk(type)) { err = "the driver's sample type is not one this plays"; return false; }
        long mn = 0, mx = 0, pref = 0, gr = 0;
        if (drv->getBufferSize(&mn, &mx, &pref, &gr) != ASE_OK || pref <= 0) { err = "the driver gave no buffer size"; return false; }
        bufFrames = pref;
        for (int c = 0; c < 2; c++) bi[c] = ASIOBufferInfo{ASIOFalse, c, {nullptr, nullptr}};
        cb.bufferSwitch = &onSwitch;
        cb.sampleRateDidChange = &onRate;
        cb.asioMessage = &onMessage;
        cb.bufferSwitchTimeInfo = &onSwitchTime;
        if (drv->createBuffers(bi, 2, bufFrames, &cb) != ASE_OK) { err = "the driver could not make its buffers: " + driverError(); return false; }
        prepared = true;
        long il = 0;
        if (drv->getLatencies(&il, &outLatency) != ASE_OK) outLatency = bufFrames;
        canOutputReady = !gNoOutputReady && drv->outputReady() == ASE_OK;
        const size_t half = dsd ? (size_t)bufFrames / 8 : (size_t)bufFrames * typeBytes(type);
        for (int c = 0; c < 2; c++)
            for (int h = 0; h < 2; h++) memset(bi[c].buffers[h], dsd ? 0x69 : 0, half);
        return true;
    }
    bool start(std::string& err) {  // ASIO thread
        if (!prepared) { err = "not open"; return false; }
        if (running) return true;
        lastSwitch = GetTickCount64();
        if (drv->start() != ASE_OK) { err = "the driver would not start: " + driverError(); return false; }
        running = true;
        return true;
    }
    void close() {  // ASIO thread
        unprepare();
        closedAt = GetTickCount64();
    }
    void tick() {  // ASIO thread, every 500 ms
        if (resetReq.exchange(false) && prepared) {  // the driver asked (its panel changed something): load it again
            logf("the driver asked for a reset — reopening");
            const bool dsd = dsdMode, wasRunning = running;
            const int r = (int)rate;
            unload();
            std::string e;
            if (!open(dsd, r, e) || (wasRunning && !start(e))) logf("reopen failed: %s", e.c_str());
        }
        if (drv && !prepared && closedAt && GetTickCount64() - closedAt > (ULONGLONG)kIdleReleaseMs) {
            unload();  // nothing plays: other programs may use the DAC
            closedAt = 0;
        }
    }

    // ── the driver's thread: one half-buffer ──
    void fill(long index) {
        lastSwitch = GetTickCount64();
        if (!prepared) return;
        const bool dsd = dsdMode;
        const size_t n = dsd ? (size_t)bufFrames / 8 : (size_t)bufFrames;  // device frames this half
        uint8_t* L = (uint8_t*)bi[0].buffers[index];
        uint8_t* R = (uint8_t*)bi[1].buffers[index];
        if (flushReq.exchange(false)) fifo.r.store(fifo.w.load());
        size_t got = 0;
        if (started && !paused) {
            if (tmp.size() < n * frameBytes) tmp.resize(n * frameBytes);  // sized in open: no allocation here after the first
            got = fifo.get(tmp.data(), n * frameBytes) / frameBytes;
            if (got < n) under++;
        }
        if (dsd) {
            const bool lsb = type == ASIOSTDSDInt8LSB1;
            for (size_t i = 0; i < n; i++) {
                uint8_t l = 0x69, r = 0x69;  // DSD silence: the DAC stays in DSD
                if (i < got) l = tmp[i * 2], r = tmp[i * 2 + 1];
                L[i] = lsb ? gRev[l] : l;
                R[i] = lsb ? gRev[r] : r;
            }
        } else {
            const double g = gain.load();
            const int bits = typeBits(type);
            const double step = std::ldexp(1.0, 32 - bits), top = std::ldexp(1.0, bits - 1);  // the device's depth
            for (size_t i = 0; i < n; i++)
                for (int c = 0; c < 2; c++) {
                    int32_t s = 0;
                    if (i < got) memcpy(&s, tmp.data() + i * 8 + c * 4, 4);
                    if (g != 1.0 && i < got) {  // software volume: scaled, TPDF dither (±1 step) at the device's depth
                        const double d = ((double)(rng() & 0xffff) - (double)(rng() & 0xffff)) / 65536.0;
                        const double q = std::clamp(std::nearbyint((double)s * g / step + d), -top, top - 1);
                        s = bits == 32 ? (int32_t)q : (int32_t)((uint32_t)(int32_t)q << (32 - bits));
                    }
                    put(c == 0 ? L : R, i, s);
                }
        }
        carried[switches.load() % 8] = (uint32_t)got;
        switches++;
        if (canOutputReady) drv->outputReady();
        if (got) SetEvent(spaceEv);
    }
    void put(uint8_t* dst, size_t i, int32_t s) {  // one S32 sample into the device's type
        switch (type) {
            case ASIOSTInt32LSB: ((int32_t*)dst)[i] = s; break;
            case ASIOSTInt32LSB24: ((int32_t*)dst)[i] = s >> 8; break;
            case ASIOSTInt32LSB20: ((int32_t*)dst)[i] = s >> 12; break;
            case ASIOSTInt32LSB18: ((int32_t*)dst)[i] = s >> 14; break;
            case ASIOSTInt32LSB16: ((int32_t*)dst)[i] = s >> 16; break;
            case ASIOSTInt16LSB: ((int16_t*)dst)[i] = (int16_t)(s >> 16); break;
            case ASIOSTInt24LSB: { const int32_t v = s >> 8; uint8_t* p = dst + i * 3; p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16); break; }
            case ASIOSTFloat32LSB: ((float*)dst)[i] = (float)((double)s / 2147483648.0); break;
            case ASIOSTFloat64LSB: ((double*)dst)[i] = (double)s / 2147483648.0; break;
            default: break;
        }
    }

    static void onSwitch(long index, ASIOBool) { if (gAsio) gAsio->fill(index); }
    static ASIOTime* onSwitchTime(ASIOTime*, long index, ASIOBool) { if (gAsio) gAsio->fill(index); return nullptr; }
    static void onRate(ASIOSampleRate r) { logf("the driver says its rate is now %.0f Hz", (double)r); }
    static long onMessage(long selector, long value, void*, double*) {
        switch (selector) {
            case kAsioSelectorSupported:
                return value == kAsioEngineVersion || value == kAsioResetRequest || value == kAsioResyncRequest ||
                       value == kAsioLatenciesChanged || value == kAsioSupportsTimeInfo ? 1 : 0;
            case kAsioEngineVersion: return 2;
            case kAsioResetRequest: if (gAsio) gAsio->resetReq = true; return 1;  // done on the ASIO thread (tick)
            case kAsioResyncRequest: return 1;
            case kAsioLatenciesChanged: return 1;
            case kAsioSupportsTimeInfo: return 0;  // bufferSwitch is enough
            default: return 0;
        }
    }
};

// ───────────────────────────── the sink the endpoint plays into ─────────────────────────────
struct DirectSink : Sink {
    Asio* a = nullptr;
    const Caps* caps = nullptr;
    Packer pk;
    Format f;
    std::vector<uint8_t> tmp;
    bool dsd = false;
    std::atomic<bool> isOpen{false};

    bool open(const Format& fmt, std::string& err) override {
        dsd = fmt.kind == "dsd";
        if (dsd ? std::find(caps->dsdRates.begin(), caps->dsdRates.end(), fmt.rate) == caps->dsdRates.end()
                : std::find(caps->pcmRates.begin(), caps->pcmRates.end(), fmt.rate) == caps->pcmRates.end()) {
            err = "the driver does not play this format as it is";
            return false;
        }
        const Container c = dsd ? Container::DSD_U8 : Container::S32_LE;
        if (!canCarry(fmt, c)) { err = "the format does not fit"; return false; }
        if (!a->call([&] { return a->open(dsd, fmt.rate, err); })) return false;
        a->frameBytes = dsd ? 2 : 8;
        const size_t perSec = (size_t)(dsd ? fmt.rate / 8 : fmt.rate) * (size_t)a->frameBytes;
        a->fifo.reset(std::max(perSec / 4, (size_t)(dsd ? a->bufFrames / 8 : a->bufFrames) * a->frameBytes * 4));  // ≥ 250 ms
        a->tmp.assign((size_t)(dsd ? a->bufFrames / 8 : a->bufFrames) * a->frameBytes, 0);
        a->paused = false;
        a->started = false;
        a->under = 0;
        for (auto& c : a->carried) c = 0;
        f = fmt;
        pk.reset(fmt, c);
        isOpen = true;
        logf("%s %d %s → %s, buffer %ld", dsd ? "DSD" : "PCM", fmt.rate, dsd ? "Hz" : (std::to_string(fmt.bits) + "-bit").c_str(),
             dsd ? (a->type == ASIOSTDSDInt8LSB1 ? "DSD LSB-first" : "DSD MSB-first") : ("type " + std::to_string(a->type)).c_str(), a->bufFrames);
        return true;
    }

    size_t write(const uint8_t* d, size_t frames) override {
        tmp.clear();
        if (!pk.pack(d, frames, tmp)) return frames;
        const size_t startAt = (size_t)(dsd ? a->bufFrames / 8 : a->bufFrames) * a->frameBytes * 2;  // two halves ready
        for (size_t i = 0; i < tmp.size();) {
            size_t n = std::min(tmp.size() - i, a->fifo.room());
            if (n) {
                a->fifo.put(tmp.data() + i, n);
                i += n;
                if (!a->started && a->fifo.used() >= startAt && !a->paused) {
                    std::string e;
                    a->started = true;
                    if (!a->call([&] { return a->start(e); })) { logf("%s", e.c_str()); return 0; }
                }
                continue;
            }
            if (!a->started && !a->paused) {  // full before it started (a tiny FIFO): start now
                std::string e;
                a->started = true;
                if (!a->call([&] { return a->start(e); })) { logf("%s", e.c_str()); return 0; }
            }
            WaitForSingleObject(a->spaceEv, 50);
            if (a->started && !a->paused && GetTickCount64() - a->lastSwitch.load() > (ULONGLONG)kStallMs) {
                logf("the driver stopped asking for audio (DAC unplugged or off?)");
                return 0;  // the endpoint tells the sender: the device failed
            }
            if (!isOpen) return 0;
        }
        return frames;
    }
    void pause(bool on) override { a->paused = on; }
    void flush() override {
        if (a->started) {
            a->flushReq = true;
            for (int i = 0; i < 40 && a->flushReq; i++) Sleep(5);  // the callback drops it within one half-buffer
        }
        a->fifo.r.store(a->fifo.w.load());
        pk.reset(f, pk.container());
    }
    void drain() override {
        for (int i = 0; i < 300 && delay() > 0 && a->started && !a->paused; i++) Sleep(10);
    }
    void close() override {
        isOpen = false;
        SetEvent(a->spaceEv);
        a->call([&] { a->close(); return 0; });
    }
    uint64_t delay() override {  // protocol frames (DSD: a byte a channel) written but not heard
        if (!isOpen) return 0;
        const uint64_t queued = a->fifo.used() / (size_t)a->frameBytes;
        // in the driver: the music the last half-buffers carried, as many as its latency spans (+ the one it reads now)
        const long halves = std::min(7L, 1 + (a->outLatency + a->bufFrames - 1) / std::max(1L, a->bufFrames));
        const uint64_t sw = a->switches.load();
        uint64_t inDriver = 0;
        for (long k = 1; k <= halves && (uint64_t)k <= sw; k++) inDriver += a->carried[(sw - k) % 8];
        return queued + (a->started ? inDriver : 0);
    }
    uint64_t underruns() override { return a->under; }
};

Endpoint* gEp = nullptr;
void onSignal(int) { if (gEp) gEp->stop(); }

double volumeGain(int v, bool muted) {  // 0..100 → a cube taper (~−18 dB at 50, −60 dB at 10)
    if (muted || v <= 0) return 0.0;
    const double x = std::min(v, 100) / 100.0;
    return x * x * x;
}

std::string dsdName(int r) { return "DSD" + std::to_string(r % 44100 ? r / 48000 : r / 44100); }

void printCaps(const Caps& k) {
    printf("    PCM:");
    for (int r : k.pcmRates) printf(" %d", r);
    printf("  (sample type %ld)\n", (long)k.pcmType);
    if (!k.dsdRates.empty()) {
        printf("    native DSD:");
        for (int r : k.dsdRates) printf(" %s", dsdName(r).c_str());
        printf("  (%s)\n", k.dsdType == ASIOSTDSDInt8LSB1 ? "LSB-first" : "MSB-first");
    } else printf("    native DSD: no\n");
}

}  // namespace

int main(int argc, char** argv) {
    // argv in UTF-8 whatever the PC's code page: Windows hands main() its arguments in the ANSI one, so a Polish
    // "Słuchawki (…)" came as cp1250, matched no device name (UTF-8 from Windows) and the share never ran (2026-11-21);
    // the name it is shown under ("PC · device") lost its "·" in code pages without one
    std::vector<std::string> u8;
    std::vector<char*> u8v;
    int wc = 0;
    if (LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &wc)) {
        for (int i = 0; i < wc; i++) u8.push_back(utf8(w[i]));
        LocalFree(w);
        for (auto& a : u8) u8v.push_back(a.data());
        argc = (int)u8v.size();
        argv = u8v.data();
    }
    std::string driverArg, name, volume = "fixed";
    int port = -1, bufferMs = 0;
    bool list = false, local = false, panel = false, anyAddress = false;
    for (int i = 1; i < argc; i++) {
        std::string x = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (x == "--driver") driverArg = next();
        else if (x == "--name") name = next();
        else if (x == "--port") port = atoi(next().c_str());
        else if (x == "--buffer") bufferMs = atoi(next().c_str());
        else if (x == "--volume") volume = next();
        else if (x == "--any-address") anyAddress = true;
        else if (x == "--local") local = true;
        else if (x == "--panel") panel = true;
        else if (x == "--list") list = true;
        else if (x == "--no-output-ready") gNoOutputReady = true;
        else {
            fprintf(stderr, "usage: Infinity-Link-Direct --list | --driver NAME [--name S] [--port N] [--buffer MS] [--volume fixed|software]\n"
                            "                   [--any-address] [--local] [--panel]\n");
            return 2;
        }
    }
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stderr, nullptr, _IONBF, 0);
    initRev();
    const std::vector<DriverEntry> drivers = installedDrivers();

    auto probeOne = [](const DriverEntry& d, Caps& k, std::string& err) {
        Asio a;
        a.clsid = d.clsid;
        a.want = d.name;
        gAsio = &a;
        a.spaceEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        a.startThread();
        k = a.call([&] { Caps c = a.probe(err); a.unload(); return c; });
        a.stopThread();
        CloseHandle(a.spaceEv);
        gAsio = nullptr;
    };

    if (list) {
        if (drivers.empty()) { printf("No ASIO drivers are installed (HKLM\\SOFTWARE\\ASIO).\n"); return 0; }
        for (auto& d : drivers) {
            printf("%s\n", d.name.c_str());
            Caps k;
            std::string err;
            probeOne(d, k, err);
            if (!err.empty()) printf("    %s\n", err.c_str());
            if (!k.pcmRates.empty()) printCaps(k);
        }
        return 0;
    }

    const DriverEntry* pick = nullptr;
    for (auto& d : drivers)
        if (!driverArg.empty() && d.name == driverArg) pick = &d;
    for (auto& d : drivers)
        if (!pick && !driverArg.empty() && d.name.find(driverArg) != std::string::npos) pick = &d;
    if (!pick) { logf("no ASIO driver \"%s\" (Infinity-Link-Direct --list)", driverArg.c_str()); return 1; }

    Asio asio;
    asio.clsid = pick->clsid;
    asio.want = pick->name;
    gAsio = &asio;
    asio.spaceEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    asio.startThread();

    if (panel) {  // the driver's own window; its window procedure runs on the ASIO thread's message loop
        std::string err;
        const bool ok = asio.call([&] { return asio.load(err) && asio.drv->controlPanel() == ASE_OK; });
        if (!ok) { logf("no settings window: %s", err.empty() ? "the driver has none" : err.c_str()); asio.stopThread(); return 1; }
        // until the driver's windows of this process are closed (some panels are separate programs: then at once)
        for (int idle = 0; idle < 4;) {
            Sleep(500);
            bool any = false;
            EnumWindows([](HWND h, LPARAM p) -> BOOL {
                DWORD pid = 0;
                GetWindowThreadProcessId(h, &pid);
                if (pid == GetCurrentProcessId() && IsWindowVisible(h)) { *(bool*)p = true; return FALSE; }
                return TRUE;
            }, (LPARAM)&any);
            idle = any ? 0 : idle + 1;
        }
        asio.stopThread();
        return 0;
    }

    std::string err;
    Caps caps = asio.call([&] { return asio.probe(err); });
    asio.call([&] { asio.unload(); return 0; });
    if (caps.pcmRates.empty()) { logf("%s", err.c_str()); asio.stopThread(); return 1; }
    if (!local) {
        printf("%s\n", pick->name.c_str());
        printCaps(caps);
    }

    const bool soft = volume == "software";
    wchar_t hw[MAX_COMPUTERNAME_LENGTH + 1] = L"windows";
    DWORD hl = MAX_COMPUTERNAME_LENGTH + 1;
    GetComputerNameW(hw, &hl);
    const std::string host = utf8(hw);  // a computer name of another script too (UTF-8, like the device's)
    EndpointInfo info;
    std::string idTail;
    for (char c : pick->name) if (isalnum((unsigned char)c)) idTail += (char)tolower((unsigned char)c);
    info.id = std::string("Infinity-Link-Direct-") + host + "-" + idTail.substr(0, 24);
    info.name = name.empty() ? std::string(host) + " · " + pick->name : name;
    info.model = pick->name;
    info.vendor = "Windows";
    info.bufferMs = bufferMs > 0 ? bufferMs : local ? 500 : 2000;
    info.anyAddress = anyAddress;
    if (local) info.address = "127.0.0.1";
    info.pcmRates = caps.pcmRates;
    info.pcmBits = pcmBitsFor(caps.pcmType);
    if (!soft) info.dsdRates = caps.dsdRates;  // software volume: DSD comes as PCM, so the volume always applies
    info.volumeKind = soft ? "software" : "fixed";
    info.volume = soft ? 30 : 100;  // software volume starts at 30 %, never full scale at first (Infinity Core 2026-11-19)

    DirectSink sink;
    sink.a = &asio;
    sink.caps = &caps;
    Endpoint ep(info, sink);
    ilink::SafeStart safe;  // a sender that just connected never turns it up (infinitylink/safe_start.h)
    ep.log = [&safe](const std::string& s) {
        safe.note(s);
        logf("%s", s.c_str());
    };
    std::atomic<int> softV{info.volume};  // the level applied here
    std::atomic<bool> softM{false};
    if (soft) {
        asio.gain = volumeGain(info.volume, false);
        ep.onVolume = [&asio, &ep, &safe, &softV, &softM](int v, bool m) {
            if (safe.hold(v, m, softV, softM)) {
                logf("volume %d from a sender that just connected: kept at %d%s", v, softV.load(), softM ? " muted" : "");
                ep.localVolume(softV, softM);
                return;
            }
            softV = v, softM = m;
            asio.gain = volumeGain(v, m);
        };
    }
    if (!ep.listen(port < 0 ? (local ? 0 : kDefaultPort) : port, err)) { logf("port %d: %s", port, err.c_str()); asio.stopThread(); return 1; }
    gEp = &ep;
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    Advertiser ad;
    if (!local && !ad.start(info.name, ep.port(), {{"v", "1"}, {"id", info.id}, {"name", info.name}, {"model", info.model}, {"vendor", info.vendor}}, err))
        logf("not advertised (%s) — senders need the address", err.c_str());
    std::thread watchIn;
    if (local) {  // the Core that started it: its pipe closes when it goes
        printf("port %d\n", ep.port());
        fflush(stdout);
        watchIn = std::thread([&ep] {
            char b[64];
            DWORD n = 0;
            HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
            while (ReadFile(in, b, sizeof b, &n, nullptr) && n > 0) {}
            ep.stop();
        });
        watchIn.detach();
    } else {
        printf("Infinity Link endpoint \"%s\" on port %d (volume %s)\n", info.name.c_str(), ep.port(), info.volumeKind.c_str());
        fflush(stdout);
    }
    ep.run();
    asio.stopThread();
    CloseHandle(asio.spaceEv);
    return 0;
}
