#include "infinitylink/discovery.h"

#include <string.h>

#include <algorithm>

#include "net.h"

#include <chrono>
#include <cctype>

namespace ilink {
namespace {

const char* kService = "_infinitylink._tcp.local";
const char* kGroup = "224.0.0.251";
const int kMdnsPort = 5353;
enum : uint16_t { A = 1, PTR = 12, TXT = 16, SRV = 33, ANY = 255 };

std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

// ── DNS message writing (names uncompressed) and reading (pointers followed)
struct W {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { b.push_back(v >> 8); b.push_back(v & 0xff); }
    void u32(uint32_t v) { u16(v >> 16); u16(v & 0xffff); }
    void name(const std::string& n) {  // "a.b.local"; the first label may hold dots escaped as "\."
        std::string label;
        for (size_t i = 0; i <= n.size(); i++) {
            if (i < n.size() && n[i] == '\\' && i + 1 < n.size()) { label += n[++i]; continue; }
            if (i == n.size() || n[i] == '.') {
                if (!label.empty()) { u8((uint8_t)std::min<size_t>(label.size(), 63)); b.insert(b.end(), label.begin(), label.begin() + std::min<size_t>(label.size(), 63)); }
                label.clear();
            } else label += n[i];
        }
        u8(0);
    }
    size_t rr(const std::string& n, uint16_t type, bool flush, uint32_t ttl) {  // returns where rdlength goes
        name(n);
        u16(type);
        u16((flush ? 0x8000 : 0) | 1);
        u32(ttl);
        size_t at = b.size();
        u16(0);
        return at;
    }
    void fix(size_t at) { size_t len = b.size() - at - 2; b[at] = (uint8_t)(len >> 8); b[at + 1] = (uint8_t)len; }
};

struct R {
    const uint8_t* p;
    size_t n, at = 0;
    bool ok = true;
    uint8_t u8() { if (at + 1 > n) { ok = false; return 0; } return p[at++]; }
    uint16_t u16() { uint16_t a = u8(); return (uint16_t)(a << 8 | u8()); }
    uint32_t u32() { uint32_t a = u16(); return a << 16 | u16(); }
    std::string nameAt(size_t& pos, int depth = 0) {
        std::string out;
        while (ok && pos < n && depth < 16) {
            uint8_t len = p[pos];
            if (len == 0) { pos++; return out; }
            if ((len & 0xC0) == 0xC0) {
                if (pos + 1 >= n) { ok = false; return out; }
                size_t ptr = (size_t)(len & 0x3F) << 8 | p[pos + 1];
                pos += 2;
                std::string rest = nameAt(ptr, depth + 1);
                return out.empty() ? rest : out + "." + rest;
            }
            if (pos + 1 + len > n) { ok = false; return out; }
            std::string label((const char*)p + pos + 1, len);
            out += (out.empty() ? "" : ".") + label;
            pos += 1 + len;
        }
        return out;
    }
    std::string name() { return nameAt(at); }
};

// joins the group on every interface (a phone sharing its Wi-Fi, a Pi on cable and Wi-Fi, a Mac with a VPN: the default
// interface alone misses the others); again later for interfaces that came up since — already joined = harmless error
void joinAll(int fd) {
    bool any = false;
    for (auto& f : net::ipv4Ifaces()) {
        ip_mreq m{};
        inet_pton(AF_INET, kGroup, &m.imr_multiaddr);
        m.imr_interface = f.addr;
        any = setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&m, sizeof m) == 0 || any;
    }
    if (!any) {  // nothing listed (or all joined already): the default interface
        ip_mreq m{};
        inet_pton(AF_INET, kGroup, &m.imr_multiaddr);
        m.imr_interface.s_addr = htonl(INADDR_ANY);
        setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&m, sizeof m);
    }
}

// UDP 5353, shared with the system's responder (Bonjour, Avahi, Android's, Windows'), in the group on every interface
int mcastSocket(std::string& err) {
    net::init();
    int fd = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { err = net::error(); return -1; }
    net::setInt(fd, SOL_SOCKET, SO_REUSEADDR, 1);
#ifdef SO_REUSEPORT
    net::setInt(fd, SOL_SOCKET, SO_REUSEPORT, 1);
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(kMdnsPort);
    if (bind(fd, (sockaddr*)&a, sizeof a) != 0) { err = net::error(); net::closeSocket(fd); return -1; }
    joinAll(fd);
    net::setTtl(fd, 255);
    return fd;
}

void sendTo(int fd, const std::vector<uint8_t>& b, const char* ip, int port) {
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, ip, &to.sin_addr);
    sendto(fd, (const char*)b.data(), (int)b.size(), 0, (sockaddr*)&to, sizeof to);
}

// to the group out of one interface
void sendOn(int fd, const in_addr& ifaddr, const std::vector<uint8_t>& b) {
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&ifaddr, sizeof ifaddr);
    sendTo(fd, b, kGroup, kMdnsPort);
}

}  // namespace

bool Advertiser::start(const std::string& name, int port, const std::map<std::string, std::string>& txt, std::string& err,
                       const std::string& service) {
    service_ = service;
    stop();
    fd_ = mcastSocket(err);
    if (fd_ < 0) return false;
    name_ = name;
    port_ = port;
    txt_ = txt;
    std::string h;  // host label: the name, letters and digits only, plus part of the id (unique on the network)
    for (char c : name) if (isalnum((unsigned char)c)) h += (char)tolower((unsigned char)c);
    std::string id = txt.count("id") ? txt.at("id") : "";
    std::string tail;
    for (char c : id) if (isalnum((unsigned char)c)) tail += (char)tolower((unsigned char)c);
    host_ = (h.empty() ? "infinitylink" : h) + "-" + (tail.size() > 6 ? tail.substr(tail.size() - 6) : tail) + ".local";
    run_ = true;
    th_ = std::thread([this] { loop(); });
    return true;
}

void Advertiser::stop() {
    run_ = false;
    if (fd_ >= 0) shutdown(fd_, SHUT_RDWR);
    if (th_.joinable()) th_.join();
    if (fd_ >= 0) net::closeSocket(fd_);
    fd_ = -1;
}

void Advertiser::loop() {
    net::nameThread("link-mdns");
    std::string inst;  // "<name>._infinitylink._tcp.local" (or its own service), dots in the name escaped
    for (char c : name_) { if (c == '.') inst += '\\'; inst += c; }
    inst += "." + service_;
    auto answer = [&](uint16_t id, bool legacy, const std::vector<uint8_t>& question, const in_addr& ip) {
        W w;
        w.u16(legacy ? id : 0);
        w.u16(0x8400);  // response, authoritative
        w.u16(legacy && !question.empty() ? 1 : 0);
        w.u16(3);  // PTR, SRV, TXT
        w.u16(0);
        w.u16(1);  // A as additional
        if (legacy && !question.empty()) w.b.insert(w.b.end(), question.begin(), question.end());
        uint32_t ttl = legacy ? 10 : 120;
        size_t at = w.rr(service_, PTR, false, ttl * 37);
        w.name(inst);
        w.fix(at);
        at = w.rr(inst, SRV, !legacy, ttl);
        w.u16(0);
        w.u16(0);
        w.u16((uint16_t)port_);
        w.name(host_);
        w.fix(at);
        at = w.rr(inst, TXT, !legacy, ttl * 37);
        for (auto& [k, v] : txt_) {
            std::string kv = k + "=" + v;
            if (kv.size() > 255) kv.resize(255);
            w.u8((uint8_t)kv.size());
            w.b.insert(w.b.end(), kv.begin(), kv.end());
        }
        w.fix(at);
        at = w.rr(host_, A, !legacy, ttl);
        const uint8_t* q = (const uint8_t*)&ip;
        w.b.insert(w.b.end(), q, q + 4);
        w.fix(at);
        return w.b;
    };
    // every interface hears about the address it can reach us on
    auto ifaces = [] {
        std::vector<net::Iface> v = net::ipv4Ifaces();
        if (v.empty()) {  // none listed: the first address there is, out of the default interface
            net::Iface f;
            inet_pton(AF_INET, net::localIPv4().c_str(), &f.addr);
            if (f.addr.s_addr) v.push_back(f);
        }
        return v;
    };
    for (int i = 0; i < 2 && run_; i++) {  // announce at start (RFC 6762 §8.3)
        for (auto& f : ifaces()) sendOn(fd_, f.addr, answer(0, false, {}, f.addr));
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    auto lastJoin = std::chrono::steady_clock::now();
    const std::string svc = lower(service_), me = lower(inst), host = lower(host_);
    uint8_t buf[9000];
    while (run_) {
        if (std::chrono::steady_clock::now() - lastJoin > std::chrono::seconds(30)) {  // Wi-Fi back, a new interface
            joinAll(fd_);
            lastJoin = std::chrono::steady_clock::now();
        }
        if (net::pollIn(fd_, 500) <= 0) continue;
        sockaddr_in from{};
        socklen_t fl = sizeof from;
        long n = recvfrom(fd_, (char*)buf, sizeof buf, 0, (sockaddr*)&from, &fl);
        if (n < 12) continue;
        R r{buf, (size_t)n};
        uint16_t id = r.u16(), flags = r.u16(), qd = r.u16();
        r.u16(); r.u16(); r.u16();
        if (flags & 0x8000) continue;  // a response, not a question
        bool ours = false;
        std::vector<uint8_t> firstQ;
        for (int i = 0; i < qd && r.ok; i++) {
            size_t start = r.at;
            std::string q = lower(r.name());
            uint16_t type = r.u16();
            r.u16();
            if (i == 0) firstQ.assign(buf + start, buf + r.at);
            // the instance name may have reached us with the escape removed: compare without it
            std::string meNoEsc;
            for (char c : me) if (c != '\\') meNoEsc += c;
            if ((q == svc && (type == PTR || type == ANY)) || q == meNoEsc || (q == host && (type == A || type == ANY))) ours = true;
        }
        if (!ours) continue;
        std::vector<net::Iface> all = ifaces();
        if (all.empty()) continue;
        if (ntohs(from.sin_port) != kMdnsPort) {  // a one-shot browser: answered directly, with the address on its network
            const net::Iface* via = &all[0];
            for (auto& f : all)
                if (f.mask.s_addr && (f.addr.s_addr & f.mask.s_addr) == (from.sin_addr.s_addr & f.mask.s_addr)) via = &f;
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
            sendTo(fd_, answer(id, true, firstQ, via->addr), ip, ntohs(from.sin_port));
        } else {
            for (auto& f : all) sendOn(fd_, f.addr, answer(id, false, firstQ, f.addr));
        }
    }
}

std::vector<Found> browse(int waitMs) { return browseService(kService, waitMs); }

std::vector<Found> browseService(const std::string& service, int waitMs) {
    std::vector<Found> out;
    std::string err;
    // from port 5353, so every responder answers to the group — Android's and Windows' ignore one-shot queries
    int fd = mcastSocket(err);
    if (fd < 0) return out;
    W q;
    q.u16(0x1234);
    q.u16(0);
    q.u16(1);
    q.u16(0); q.u16(0); q.u16(0);
    q.name(service.c_str());
    q.u16(PTR);
    q.u16(1);
    auto ask = [&] {
        std::vector<net::Iface> all = net::ipv4Ifaces();
        for (auto& f : all) sendOn(fd, f.addr, q.b);
        if (all.empty()) sendTo(fd, q.b, kGroup, kMdnsPort);
    };
    ask();
    std::map<std::string, Found> inst;  // instance name → what we learned
    std::map<std::string, std::vector<std::string>> addr;  // host name → its IPv4s (a responder answers on every interface)
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
    uint8_t buf[9000];
    int resent = 0;
    for (;;) {
        int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
        if (left <= 0) break;
        if (resent == 0 && left < waitMs / 2) { ask(); resent = 1; }  // once more: Wi-Fi drops multicast
        if (net::pollIn(fd, std::min(left, 200)) <= 0) continue;
        long n = net::recvBytes(fd, buf, sizeof buf);
        if (n < 12) continue;
        R r{buf, (size_t)n};
        r.u16();
        uint16_t flags = r.u16(), qd = r.u16(), an = r.u16(), ns = r.u16(), ar = r.u16();
        if (!(flags & 0x8000)) continue;
        for (int i = 0; i < qd && r.ok; i++) { r.name(); r.u16(); r.u16(); }
        for (int i = 0; i < an + ns + ar && r.ok; i++) {
            std::string nm = r.name();
            uint16_t type = r.u16();
            r.u16();
            r.u32();
            uint16_t len = r.u16();
            size_t end = r.at + len;
            if (end > r.n) break;
            std::string key = lower(nm);
            const std::string svc = "." + lower(service);
            const bool ours = key.size() > svc.size() && key.compare(key.size() - svc.size(), svc.size(), svc) == 0;
            if (type == PTR && key == lower(service)) {
                size_t pos = r.at;
                std::string target = r.nameAt(pos);
                inst[lower(target)].name = target.substr(0, target.find("." + service.substr(0, service.find('.', 1))));
            } else if (type == SRV && ours) {  // the group carries every responder's records: only ours
                r.u16(); r.u16();
                int port = r.u16();
                std::string target = r.name();
                Found& f = inst[key];
                if (f.name.empty()) f.name = nm.substr(0, nm.find("." + service.substr(0, service.find('.', 1))));
                f.port = port;
                f.host = lower(target);  // a host name for now; its address below
            } else if (type == TXT && ours) {
                Found& f = inst[key];
                size_t pos = r.at;
                while (pos < end) {
                    uint8_t l = buf[pos++];
                    if (pos + l > end) break;
                    std::string kv((const char*)buf + pos, l);
                    pos += l;
                    size_t eq = kv.find('=');
                    if (eq != std::string::npos) f.txt[kv.substr(0, eq)] = kv.substr(eq + 1);
                }
            } else if (type == A && len == 4) {
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, buf + r.at, ip, sizeof ip);
                if (std::find(addr[key].begin(), addr[key].end(), ip) == addr[key].end()) addr[key].push_back(ip);
            }
            r.at = end;
        }
    }
    net::closeSocket(fd);
    // of a host's addresses, the one on a network this machine is on (not a VPN's /32, not another LAN)
    const std::vector<net::Iface> mine = net::ipv4Ifaces();
    auto onLan = [&](const std::string& ip) {
        in_addr a{};
        inet_pton(AF_INET, ip.c_str(), &a);
        for (auto& f : mine)
            if (f.mask.s_addr != 0xFFFFFFFFu && (a.s_addr & f.mask.s_addr) == (f.addr.s_addr & f.mask.s_addr)) return true;
        return false;
    };
    for (auto& [k, f] : inst) {
        if (!f.port || addr[f.host].empty()) continue;
        Found g = f;
        g.host = addr[f.host][0];
        for (auto& ip : addr[f.host]) if (onLan(ip)) { g.host = ip; break; }
        if (g.txt.count("name")) g.name = g.txt["name"];
        g.id = g.txt.count("id") ? g.txt["id"] : k;
        g.model = g.txt.count("model") ? g.txt["model"] : "";
        g.vendor = g.txt.count("vendor") ? g.txt["vendor"] : "";
        out.push_back(g);
    }
    return out;
}

}  // namespace ilink
