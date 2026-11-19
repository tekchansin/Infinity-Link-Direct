// Safe start for an endpoint's volume (Infinity Core 2026-11-19: a shared MacBook speaker went loud when a sender connected
// with its own remembered level): a sender that has just connected never turns the volume UP, nor unmutes — for 3 s after
// a stream sender's hello its volume messages that would do either are held. The endpoint keeps its level and tells the
// sender (Endpoint::localVolume), so the sender's slider follows. A turn of its slider after that goes through as always.
//
//   ilink::SafeStart safe;
//   ep.log = [&](const std::string& s) { safe.note(s); /* log it */ };
//   ep.onVolume = [&](int v, bool m) { if (safe.hold(v, m, here, hereMuted)) { ep.localVolume(here, hereMuted); return; } … };
#pragma once

#include <atomic>
#include <chrono>
#include <string>

namespace ilink {

class SafeStart {
public:
    // every line the endpoint logs: a stream sender's hello is "sender: <name>" (endpoint.cpp)
    void note(const std::string& line) {
        if (line.rfind("sender: ", 0) == 0) helloAt_ = now();
    }
    // want / wantMuted: what the sender asks; here / hereMuted: what plays now
    bool hold(int want, bool wantMuted, int here, bool hereMuted) const {
        return now() - helloAt_ < kHoldMs && !wantMuted && (want > here + 2 || hereMuted);
    }
    static constexpr long long kHoldMs = 3000;

private:
    static long long now() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    std::atomic<long long> helloAt_{-kHoldMs * 10};
};

}  // namespace ilink
