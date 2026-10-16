// Distributed solving: `snek worker <host:port> <threads> [name]` connects to the coordinator
// (dist/coordinator.js), receives positions to solve and sends back exact values.
//
// Line protocol over TCP:
//   worker -> coordinator:  hello <name> <threads>
//                           get <n>                      (ask for up to n units)
//                           result <id> <value> <nodes> <seconds>
//   coordinator -> worker:  unit <id> [<node budget>] <move> <move> ...  (0..n of them, then)
//                           (value -9 is returned when the node budget runs out)
//                           end
// With SOLVER_WIN_TEST=1 the worker only asks whether the side to move wins (cheaper): a unit
// that is not a win comes back as 0 (draw or loss) or -1 (proven loss) instead of its exact value.
// A worker that loses the connection reconnects; the coordinator re-queues its units.
// The threads share one transposition table, kept from unit to unit (makeSweepTable); the rest of a
// solver's state is reset for every unit (Solver::newPosition).
#pragma once
#include <condition_variable>
#include <deque>
#include <string>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
static const sock_t BAD_SOCK = INVALID_SOCKET;
static void closeSock(sock_t s) { closesocket(s); }
#else
#include <csignal>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
static const sock_t BAD_SOCK = -1;
static void closeSock(sock_t s) { close(s); }
#endif
#include "solver.hpp"

struct LineConn {
    sock_t s = BAD_SOCK;
    std::string buf;
    std::mutex sendMu;
    bool connectTo(const std::string& host, const std::string& port) {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) return false;
        s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (s == BAD_SOCK) { freeaddrinfo(res); return false; }
        if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) { closeSock(s); s = BAD_SOCK; freeaddrinfo(res); return false; }
        freeaddrinfo(res);
        buf.clear();
        return true;
    }
    bool sendLine(const std::string& line) {
        std::lock_guard<std::mutex> lk(sendMu);
        if (s == BAD_SOCK) return false;
        std::string l = line + "\n";
        size_t off = 0;
        while (off < l.size()) {
            int k = send(s, l.data() + off, (int)(l.size() - off), 0);
            if (k <= 0) return false;
            off += k;
        }
        return true;
    }
    bool readLine(std::string& line) {
        for (;;) {
            size_t p = buf.find('\n');
            if (p != std::string::npos) {
                line = buf.substr(0, p);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                buf.erase(0, p + 1);
                return true;
            }
            char tmp[4096];
            int k = recv(s, tmp, sizeof tmp, 0);
            if (k <= 0) return false;
            buf.append(tmp, k);
        }
    }
    void close() {
        std::lock_guard<std::mutex> lk(sendMu);
        if (s != BAD_SOCK) closeSock(s);
        s = BAD_SOCK;
    }
};

inline int runWorker(const std::string& hostport, int threads, const std::string& name) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    signal(SIGPIPE, SIG_IGN);  // a dropped connection must not kill the worker
#endif
    size_t colon = hostport.rfind(':');
    std::string host = colon == std::string::npos ? hostport : hostport.substr(0, colon);
    std::string port = colon == std::string::npos ? "8090" : hostport.substr(colon + 1);
    LineConn conn;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::pair<std::string, std::string>> queue;  // id, moves
    int busy = 0;
    std::atomic<uint64_t> solvedCount{0};
    auto t0 = std::chrono::steady_clock::now();

    std::vector<std::thread> th;
    std::shared_ptr<LTT> table = makeSweepTable();  // the units are usually siblings
    for (int w = 0; w < threads; w++)
        th.emplace_back([&] {
            Solver S(table);
            bool winTest = envInt("SOLVER_WIN_TEST", 0) != 0;
            for (;;) {
                std::pair<std::string, std::string> u;
                {
                    std::unique_lock<std::mutex> lk(mu);
                    cv.wait(lk, [&] { return !queue.empty(); });
                    u = queue.front();
                    queue.pop_front();
                    busy++;
                }
                Pos s = Pos::start();
                std::istringstream ms(u.second);
                std::string tok;
                bool ok = true;
                uint64_t limit = 0;  // node budget (0 = unlimited); a leading bare number is the budget
                bool first = true;
                while (ms >> tok) {
                    if (first && tok.find(':') == std::string::npos && tok != "pass") { limit = std::strtoull(tok.c_str(), nullptr, 10); first = false; continue; }
                    first = false;
                    Move m;
                    if (!parseMove(tok, m) || !s.legalMove(m)) { ok = false; break; }
                    s.play(m);
                }
                char line[256];
                if (!ok) {
                    snprintf(line, sizeof line, "result %s bad 0 0", u.first.c_str());
                } else {
                    S.newPosition();
                    auto ts = std::chrono::steady_clock::now();
                    int v = S.solve(s, 0, limit ? limit : UINT64_MAX, winTest ? 0 : -1, 1);  // -9 = budget exceeded
                    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count();
                    snprintf(line, sizeof line, "result %s %d %llu %.2f", u.first.c_str(), v, (unsigned long long)S.nodes, sec);
                    solvedCount++;
                }
                conn.sendLine(line);
                {
                    std::lock_guard<std::mutex> lk(mu);
                    busy--;
                }
            }
        });

    printf("worker '%s': %d threads, coordinator %s:%s\n", name.c_str(), threads, host.c_str(), port.c_str());
    fflush(stdout);
    for (;;) {
        if (!conn.connectTo(host, port)) {
            printf("cannot connect to %s:%s, retrying in 5 s...\n", host.c_str(), port.c_str());
            fflush(stdout);
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        printf("connected\n");
        fflush(stdout);
        conn.sendLine("hello " + name + " " + std::to_string(threads));
        bool alive = true;
        auto lastReport = std::chrono::steady_clock::now();
        while (alive) {
            int want;
            {
                std::lock_guard<std::mutex> lk(mu);
                want = threads + 2 - (int)queue.size() - busy;
            }
            if (want <= 0) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue; }
            if (!conn.sendLine("get " + std::to_string(want))) break;
            int got = 0;
            std::string line;
            for (;;) {
                if (!conn.readLine(line)) { alive = false; break; }
                if (line == "end") break;
                if (line.rfind("unit ", 0) == 0) {
                    std::istringstream ls(line.substr(5));
                    std::string id, rest, tok;
                    ls >> id;
                    while (ls >> tok) rest += (rest.empty() ? "" : " ") + tok;
                    {
                        std::lock_guard<std::mutex> lk(mu);
                        queue.push_back({id, rest});
                    }
                    cv.notify_one();
                    got++;
                }
            }
            if (alive && got == 0) std::this_thread::sleep_for(std::chrono::seconds(3));
            if (std::chrono::steady_clock::now() - lastReport > std::chrono::seconds(60)) {
                double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                printf("%llu units solved in %.0f min\n", (unsigned long long)solvedCount.load(), el / 60);
                fflush(stdout);
                lastReport = std::chrono::steady_clock::now();
            }
        }
        conn.close();
        {
            // units still queued locally are re-queued by the coordinator on disconnect
            std::lock_guard<std::mutex> lk(mu);
            queue.clear();
        }
        printf("connection lost, reconnecting...\n");
        fflush(stdout);
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}
