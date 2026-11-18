// Dev harness: stream a WAV in real time through rig::listen::ListenService and serve /api/listen(/set),
// so the Listen pane can be developed in a browser without a second audio engine.
//   clang++ -std=c++20 -O2 -I src tests/listen_serve.cpp -framework Accelerate -o build-listen/listen_serve
//   build-listen/listen_serve file.wav 17791
#include "analysis/ListenAnalyzer.hpp"
#include "audio/WavIO.hpp"
#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

using namespace rig::listen;

static std::string param(const std::string& q, const std::string& k) {
    std::size_t p = 0;
    while ((p = q.find(k + "=", p)) != std::string::npos) {
        if (p == 0 || q[p - 1] == '&') { const auto e = q.find('&', p); return q.substr(p + k.size() + 1, e == std::string::npos ? std::string::npos : e - p - k.size() - 1); }
        ++p;
    }
    return "";
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: listen_serve file.wav port\n"); return 1; }
    ::signal(SIGPIPE, SIG_IGN);
    rig::audio::WavData w;
    if (!rig::audio::readWavFile(argv[1], w)) { std::fprintf(stderr, "bad wav\n"); return 1; }
    const double sr = w.sample_rate;
    // crude "rig" for the output spectrum: drive + tone stack + cab-ish low-pass
    std::vector<float> out(w.left.size());
    { Biquad lp1, lp2; lp1.lowpass(5200, sr, 0.7); lp2.lowpass(5200, sr, 0.7); double hp = 0, px = 0;
      for (std::size_t i = 0; i < out.size(); ++i) { const double x = w.left[i]; hp = 0.995 * (hp + x - px); px = x; out[i] = static_cast<float>(0.3 * lp2.run(lp1.run(std::tanh(hp * 18.0)))); } }
    static ListenService svc;
    std::atomic<bool> run{true};
    static std::atomic<std::size_t> seekTo{static_cast<std::size_t>(-1)};
    std::thread audio([&] {
        std::size_t pos = 0;
        const uint32_t blk = 256;
        auto next = std::chrono::steady_clock::now();
        while (run) {
            const std::size_t sk = seekTo.exchange(static_cast<std::size_t>(-1));
            if (sk != static_cast<std::size_t>(-1)) pos = sk % w.left.size();
            if (svc.wanted()) {
                float a[256], b[256];
                for (uint32_t i = 0; i < blk; ++i) { a[i] = w.left[(pos + i) % w.left.size()]; b[i] = out[(pos + i) % out.size()]; }
                svc.write(a, 1.0f, b, blk);
            }
            pos = (pos + blk) % w.left.size();
            next += std::chrono::microseconds(static_cast<long long>(1e6 * blk / sr));
            std::this_thread::sleep_until(next);
        }
    });
    int fd = ::socket(AF_INET, SOCK_STREAM, 0), one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(static_cast<uint16_t>(std::atoi(argv[2])));
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) || ::listen(fd, 16)) { std::perror("bind"); return 1; }
    std::fprintf(stderr, "listen_serve on %s\n", argv[2]);
    while (true) {
        int c = ::accept(fd, nullptr, nullptr);
        if (c < 0) continue;
        char buf[4096]; const auto n = ::recv(c, buf, sizeof buf - 1, 0);
        if (n <= 0) { ::close(c); continue; }
        buf[n] = 0;
        std::string req(buf), path, query;
        { std::istringstream is(req); std::string m, url; is >> m >> url; const auto q = url.find('?'); path = url.substr(0, q); if (q != std::string::npos) query = url.substr(q + 1); }
        std::string body = "{\"ok\":false}";
        if (path == "/api/listen") {
            static bool once = false;
            if (!once) { once = true; svc.setModelPath(getenv("LB_MODEL") ? getenv("LB_MODEL") : "resources/models/guitar_midi.hgnn"); }
            svc.start([sr] { return sr; });
            body = svc.poll();
            if (body.back() == '}') { body.pop_back(); body += ",\"source\":\"player\",\"latency_ms\":5.80,\"cpu\":0.0900}"; }
        } else if (path == "/api/listen/seek") {
            seekTo = static_cast<std::size_t>(std::atof(param(query, "t").c_str()) * sr);
            body = "{\"ok\":true}";
        } else if (path == "/api/listen/set") {
            const auto a4 = param(query, "a4");
            if (!a4.empty()) svc.setReference(std::atof(a4.c_str()));
            if (query.find("strings=") != std::string::npos) {
                std::vector<double> v; std::string s = param(query, "strings"), tok;
                for (auto& ch : s) if (ch == '%') ch = ' ';
                std::stringstream ss(s);
                while (std::getline(ss, tok, ',')) if (!tok.empty()) v.push_back(std::atof(tok.c_str()));
                svc.setStrings(v);
            }
            if (param(query, "reset_key") == "1") svc.resetKey();
            body = "{\"ok\":true}";
        }
        std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        ::send(c, resp.data(), resp.size(), 0);
        ::close(c);
    }
}
