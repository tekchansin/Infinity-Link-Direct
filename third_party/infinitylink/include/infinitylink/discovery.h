// Infinity Link discovery (SPEC §2): DNS-SD over multicast DNS, without a system daemon — a small responder for
// endpoints and a one-shot browse for senders. Portable POSIX (macOS, Linux, Android NDK).
#pragma once
#include <atomic>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace ilink {

// An endpoint's DNS-SD record set: "<name>._infinitylink._tcp.local" → SRV (port, host) + TXT, the host's A record.
class Advertiser {
public:
    ~Advertiser() { stop(); }
    // txt: v, id, name, model, vendor, optional app / library (SPEC §2). False + err when the multicast socket cannot be opened.
    // service: another DNS-SD type than Infinity Link's own (e.g. "_infinity-core._tcp.local", an Infinity Core for apps)
    bool start(const std::string& name, int port, const std::map<std::string, std::string>& txt, std::string& err,
               const std::string& service = "_infinitylink._tcp.local");
    void stop();

private:
    void loop();
    std::string name_, host_, service_;
    int port_ = 0, fd_ = -1;
    std::map<std::string, std::string> txt_;
    std::atomic<bool> run_{false};
    std::thread th_;
};

struct Found {
    std::string name, id, model, vendor;
    std::string host;  // IPv4 address
    int port = 0;
    std::map<std::string, std::string> txt;
};
// Asks the network for Infinity Link endpoints and collects the answers for `waitMs`.
std::vector<Found> browse(int waitMs = 1500);
// the same for any DNS-SD service type, e.g. "_smb._tcp.local" (file servers) — name, host, port, TXT
std::vector<Found> browseService(const std::string& service, int waitMs = 1500);

}  // namespace ilink
